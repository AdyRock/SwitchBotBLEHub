
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <ESPAsyncWebServer.h>
#include <AsyncUDP.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <heltec.h>

#include "BLE_Device.h"
#include "secrets.h"    //Define a file called secrets.h and put in your WiFi SSID and Password as #defines. e.g. #define WIFI_SSIS "ROUTER_SSID"

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASS;

BLE_Device BLE_Devices;
ClientCallbacks OurCallbacks;

CommandQ BLECommandQ;
AsyncWebServer server( 80 );
AsyncUDP udp;

const int led = 14;

char macAddress[ 18 ];
unsigned long sendBroadcast = 0;
char lookingForBLEAddress[ 18 ];

// The remote service we wish to connect to.
static BLEUUID serviceUUID( "cba20d00-224d-11e6-9fb8-0002a5d5c51b" );
// The characteristic of the remote service we are interested in.
static BLEUUID    charUUID( "cba20002-224d-11e6-9fb8-0002a5d5c51b" );

void handleRoot( AsyncWebServerRequest* request )
{
    digitalWrite( led, 1 );
    request->send( 200, "text/plain", "hello from SwitchBot BLE Hub!" );
    digitalWrite( led, 0 );
}

void handleNotFound( AsyncWebServerRequest* request )
{
    digitalWrite( led, 1 );
    String message = "File Not Found\n\n";
    message += "URI: ";
    message += request->url();
    message += "\nMethod: ";
    message += ( request->method() == HTTP_GET ) ? "GET" : "POST";
    message += "\nArguments: ";
    message += (unsigned long)request->args();
    message += "\n";
    for (uint8_t i = 0; i < request->args(); i++)
    {
        message += " " + request->argName( i ) + ": " + request->arg( i ) + "\n";
    }
    request->send( 404, "text/plain", message );
    digitalWrite( led, 0 );
}

static void notifyCallback(
    BLERemoteCharacteristic* pBLERemoteCharacteristic,
    uint8_t* pData,
    size_t length,
    bool isNotify )
{
    Serial.print( "Notify callback for characteristic " );
    Serial.print( pBLERemoteCharacteristic->getUUID().toString().c_str() );
    Serial.print( " of data length " );
    Serial.println( (unsigned long)length );
    Serial.print( "data: " );
    Serial.println( (char*)pData );
}

/**
   Scan for BLE servers and find the first one that advertises the service we are looking for.
*/
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks
{
    /**
        Called for each advertising BLE server.
    */
    void onResult( BLEAdvertisedDevice* advertisedDevice )
    {
        // We have found a device, let us now see if it contains the service we are looking for.
        if (advertisedDevice->haveServiceUUID() && advertisedDevice->isAdvertisingService( serviceUUID ))
        {
            BLE_Devices.AddDevice( advertisedDevice->getAddress().toString().c_str(), advertisedDevice->getRSSI(), (uint8_t*)advertisedDevice->getServiceData().data(), advertisedDevice->getServiceData().length() );

            if (lookingForBLEAddress[ 0 ] != 0)
            {
                Serial.print( "Found BLE device: " );
                Serial.println( advertisedDevice->getAddress().toString().c_str() );
                if (strcmp( lookingForBLEAddress, advertisedDevice->getAddress().toString().c_str() ) == 0)
                {
                    BLEScan* pBLEScan = BLEDevice::getScan();
                    pBLEScan->stop();
                }

            }
        }
    }; // onResult
}; // MyAdvertisedDeviceCallbacks

void setup()
{
    pinMode( led, OUTPUT );
    digitalWrite( led, 0 );
    Heltec.begin( true /*DisplayEnable Enable*/, false /*LoRa Disable*/, true /*Serial Enable*/ );
    Serial.begin( 115200 );
    Serial.println( "Starting Arduino BLE Client application..." );

    WiFi.mode( WIFI_STA );
    WiFi.begin( ssid, password );

    // Wait for connection
    while (WiFi.status() != WL_CONNECTED)
    {
        delay( 500 );
        Serial.print( "." );
    }
    Serial.println( "" );
    Serial.print( "Connected to " );
    Serial.println( ssid );
    Serial.print( "IP address: " );
    Serial.println( WiFi.localIP() );

    BLEDevice::init( "" );

    server.on( "/", handleRoot );

    sendBroadcast = millis();
    server.onRequestBody(
        []( AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total )
        {
            if (request->method() == HTTP_POST)
            {
                digitalWrite( led, 1 );

                if (request->url() == "/api/v1/callback/add")
                {
                    const size_t        JSON_DOC_SIZE = 512U;
                    DynamicJsonDocument jsonDoc( JSON_DOC_SIZE );

                    if (DeserializationError::Ok == deserializeJson( jsonDoc, (const char*)data ))
                    {
                        JsonObject callbackAddress = jsonDoc.as<JsonObject>();
                        if (OurCallbacks.Add( callbackAddress[ "uri" ], millis() ))
                        {
                            String msg = "OK";
                            request->send( 200, "text/plain", msg );
                        }
                        else
                        {
                            String msg = "Too Many Requests";
                            request->send( 429, "text/plain", msg );
                        }
                    }
                    else
                    {
                        String msg = "Bad Request";
                        request->send( 400, "text/plain", msg );
                    }
                }
                else if (request->url() == "/api/v1/callback/remove")
                {
                    const size_t        JSON_DOC_SIZE = 512U;
                    DynamicJsonDocument jsonDoc( JSON_DOC_SIZE );

                    if (DeserializationError::Ok == deserializeJson( jsonDoc, (const char*)data ))
                    {
                        JsonObject callbackAddress = jsonDoc.as<JsonObject>();
                        if (OurCallbacks.Remove( (const char*)callbackAddress[ "uri" ] ))
                        {
                            request->send( 200, "text/plain", "OK" );
                        }
                        else
                        {
                            request->send( 400, "text/plain", "Bad Request" );
                        }
                    }
                    else
                    {
                        String msg = "Bad Request";
                        request->send( 400, "text/plain", msg );
                    }
                }
                else if (request->url() == "/api/v1/device/write")
                {
                    const size_t        JSON_DOC_SIZE = 512U;
                    DynamicJsonDocument jsonDoc( JSON_DOC_SIZE );

                    if (DeserializationError::Ok == deserializeJson( jsonDoc, (const char*)data ))
                    {
                        JsonObject writeParameters = jsonDoc.as<JsonObject>();
                        String clientAddress = writeParameters[ "address" ];

                        // Check that we have seen that device
                        int deviceIdx = BLE_Devices.FindDevice( clientAddress.c_str() );
                        if (deviceIdx >= 0)
                        {
                            String dataToWrite = writeParameters[ "data" ];
                            Serial.printf( "Received request to write device %s with %s (%i)\n", clientAddress.c_str(), dataToWrite.c_str(), dataToWrite.length() );

                            if (BLECommandQ.Push( clientAddress, dataToWrite ))
                            {
                                request->send( 200, "text/plain", "OK" );
                            }
                            else
                            {
                                String msg = "Too Many Requests";
                                request->send( 429, "text/plain", msg );
                                Serial.println( "I have too much in my command Q" );
                            }
                        }
                        else
                        {
                            String msg = "Unknown device";
                            request->send( 422, "text/plain", msg );
                            Serial.printf( "Received request to write device but I have not seen that device)\n", clientAddress.c_str() );
                        }
                    }
                    else
                    {
                        String msg = "Bad Request";
                        request->send( 400, "text/plain", msg );
                    }
                }

                digitalWrite( led, 0 );

            }
        }
    );

    server.on( "/api/v1/devices", HTTP_GET, []( AsyncWebServerRequest* request )
        {
            digitalWrite( led, 1 );

            Serial.println( "Received request for devices" );
            char* buf = (char*)malloc( 2048 );
            BLE_Devices.AllToJson( buf, 2048, false, macAddress );
            Serial.println( buf );
            request->send( 200, "application/json", buf );
            free( buf );
            digitalWrite( led, 0 );

        } );

    server.on( "/api/v1/device", HTTP_GET, []( AsyncWebServerRequest* request )
        {
            digitalWrite( led, 1 );
            Serial.println( "Received request for device" );
            String address = request->arg( "address" );

            int deviceIdx = BLE_Devices.FindDevice( address.c_str() );

            char* buf = (char*)malloc( 2048 );
            BLE_Devices.DeviceToJson( deviceIdx, buf, 2048, macAddress );
            Serial.println( buf );
            request->send( 200, "application/json", buf );
            free( buf );
            digitalWrite( led, 0 );
        } );


    server.onNotFound( handleNotFound );

    server.begin();
    Serial.println( "HTTP server started" );

    BLEDevice::init( "" );

    // Retrieve a Scanner and set the callback we want to use to be informed when we
    // have detected a new device.  Specify that we want active scanning and start the
    // scan to run for 5 seconds.
    BLEScan* pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks( new MyAdvertisedDeviceCallbacks(), true );
    pBLEScan->setInterval( 510 );
    pBLEScan->setWindow( 200 );
    pBLEScan->setActiveScan( true );
    pBLEScan->start( 0, nullptr, false );

    Serial.println( "Application started" );

    uint8_t mac[ 6 ];
    WiFi.macAddress( mac );
    sprintf( macAddress, "%0.2x:%0.2x:%0.2x:%0.2x:%0.2x:%0.2x", mac[ 5 ], mac[ 4 ], mac[ 3 ], mac[ 2 ], mac[ 1 ], mac[ 0 ] );

    if (udp.listenMulticast( IPAddress( 239, 1, 2, 3 ), 1234 ))
    {
        Serial.print( "UDP Listening on IP: " );
        Serial.println( WiFi.localIP() );
        udp.onPacket( []( AsyncUDPPacket packet )
            {
                // Serial.println();
                // Serial.print( "UDP Packet: " );
                // Serial.printf( ", Data (len %i): ", packet.length() );
                // Serial.write( packet.data(), packet.length() );
                // Serial.println();
                // Serial.println();
                if (strncmp( (char*)packet.data(), "Are you there SwitchBot?", packet.length() ) == 0)
                {
                    Serial.println( "Broadcasting logon ASAP" );
                    sendBroadcast = millis();
                }
            } );
    }

} // End of setup.


// This is the Arduino main loop function.
void loop()
{
    for (;;)
    {
        if (millis() >= sendBroadcast)
        {
            //Send multicast
            Serial.printf( "\n***Broadcasting my details: %s***\n", macAddress );
            udp.printf( "SwitchBot BLE Hub! %s", macAddress );
            sendBroadcast = millis() + 60000;
        }

        // Check if there is a BLE command to send
        BLE_COMMAND BLECommand;
        if (BLECommandQ.Pop( &BLECommand ))
        {
            digitalWrite( led, 1 );
            WriteToBLEDevice( &BLECommand );
            digitalWrite( led, 0 );
        }

        if (BLE_Devices.HasChanged())
        {
            static char outstr[ 50 ];

            if (OurCallbacks.HasCallbacks())
            {
                SendChangedDevices();
            }

            SWITCHBOT device;
            uint8_t i = 0;

            // clear the display
            Heltec.display->clear();

            while (BLE_Devices.GetSWDevice( i++, device ))
            {
                if (device.model == 'c')
                {
                    sprintf( outstr, "Pos: %i %%", device.curtain.position );
                    Heltec.display->drawString( 0, 0, outstr );
                    //                    Serial.println( outstr );
                }
                else if (device.model == 'T')
                {
                    sprintf( outstr, "Temp: %0.1f °C", device.thermometer.temperature );
                    Heltec.display->drawString( 0, 20, outstr );
                    //                    Serial.println( outstr );

                    sprintf( outstr, "Hum: %i %%", device.thermometer.humidity );
                    Heltec.display->drawString( 0, 40, outstr );
                    //                    Serial.println( outstr );
                }
            }

            // write the buffer to the display
            Heltec.display->display();
        }

        OurCallbacks.Check( millis() ); // Check if any of the registered callbacks have timedout
    } // end of endless loop ;-)

} // End of loop

int SendDeviceChange( const char* host, const char* data, int bytes )
{
    //host = "192.168.1.1" ip or dns

    Serial.print( "Connecting to " );
    Serial.println( host );

    HTTPClient http;

    // configure target server and url
    http.begin( host ); //HTTP
    http.addHeader( "Content-Type", "application/json" );

    // start connection and send HTTP header
    int httpCode = http.POST( (uint8_t*)data, bytes );

    // httpCode will be negative on error
    if (httpCode > 0)
    {
        // HTTP header has been send and Server response header has been handled
        Serial.printf( "[HTTP] POST response code: %d\n", httpCode );
    }
    else
    {
        Serial.printf( "[HTTP] POST failed, code %i, error: %s\n", httpCode, http.errorToString( httpCode ).c_str() );
    }

    http.end();

    return httpCode;
}

void SendChangedDevices()
{
    // This object changed so send to registered callbacks
    char* deviceBuf = (char*)malloc( 2048 );
    int bytes = BLE_Devices.AllToJson( deviceBuf, 2048, true, macAddress );

    Serial.print( "\nSending: " );
    Serial.println( deviceBuf );

    char* addresBuf = (char*)malloc( 256 );
    uint8_t i = 0;
    while (OurCallbacks.Get( i++, addresBuf, 255 ))
    {
        if (SendDeviceChange( addresBuf, deviceBuf, bytes ) == -1)
        {
            // remove refused connection
            i--;
            OurCallbacks.Remove( i );
        }
    }

    free( addresBuf );
    free( deviceBuf );
}

void WriteToBLEDevice( BLE_COMMAND* BLECommand )
{
    BLEScan* pBLEScan = BLEDevice::getScan();
    pBLEScan->stop();

    BLEAddress bleAddress( BLECommand->Address );
    Serial.print( "Looking for BLE device: " );
    Serial.println( BLECommand->Address );

    // Register the device we are looking for so the scan stop as soon as it is found
    strcpy( lookingForBLEAddress, BLECommand->Address );
    
    // Scan for max 10 seconds or until the device is found
    NimBLEScanResults results = pBLEScan->start( 10 );

    // Clear the registered device to look for
    lookingForBLEAddress[ 0 ] = 0;

    // Get the device (might be null if not found)
    NimBLEAdvertisedDevice* pDevice = results.getDevice( bleAddress );

    if (pDevice)
    {
        // The device was found so create a clinet to connect to it
        NimBLEClient* pBLEClient = NimBLEDevice::createClient();
        pBLEClient->setConnectionParams( 32, 160, 0, 500 );
        
        bool complete = false;
        int retries = 5;

        while (!complete && ( retries-- > 0 ))
        {
            Serial.println( "Connecting to device..." );
            if (pBLEClient->connect( pDevice ))
            {
                //success
                Serial.println( "Device connected" );

                BLERemoteService* rs = pBLEClient->getService( serviceUUID );
                if (rs != nullptr)
                {
                    Serial.println( "Got remote service" );

                    BLERemoteCharacteristic* rc = rs->getCharacteristic( charUUID );
                    if (rs != nullptr)
                    {
                        Serial.println( "Got remote characteristic" );

                        rc->writeValue( BLECommand->Data, BLECommand->DataLen );
                        Serial.println( "Date sent" );
                        complete = true;
                    }
                    else
                    {
                        Serial.println( "Failed to get characteristic" );
                    }
                }
                else
                {
                    Serial.println( "Failed to get service" );
                }

                pBLEClient->disconnect();
                Serial.println( "Disconnected device" );
            }
            else
            {
                Serial.println( "Failed to connected to device" );
            }
        }

        NimBLEDevice::deleteClient( pBLEClient );
    }

    // Restart the continuos scan
    pBLEScan->start( 0, nullptr, false );

}