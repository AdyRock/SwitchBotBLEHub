/**
   A BLE client example that is rich in capabilities.
   There is a lot new capabilities implemented.
   author unknown
   updated by chegewara

   https://github.com/nkolban/esp32-snippets/blob/master/Documentation/BLE%20C%2B%2B%20Guide.pdf
*/

#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include "BLE_Device.h"

#include "secrets.h"    //Define a file called secrets.h and put in your WiFi SSID and Password as #defines. e.g. #define WIFI_SSIS "ROUTER_SSID"

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASS;

BLE_Device BLE_Devices;
ClientCallbacks OurCallbacks;

CommandQ BLECommandQ;
AsyncWebServer server( 80 );

const int led = 13;

//#include "BLEDevice.h"
#include <NimBLEDevice.h>

#include <ArduinoJson.h>
#include <heltec.h>

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
    message += request->args();
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
    Serial.println( length );
    Serial.print( "data: " );
    Serial.println( (char*)pData );
}

class MyBLEClientCallback : public BLEClientCallbacks
{
    void onConnect( BLEClient* pclient )
    {
        Serial.println( "BLE onConnect" );
    }

    void onDisconnect( BLEClient* pclient )
    {
        Serial.println( "BLE onDisconnect" );
    }
};


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

    if (MDNS.begin( "switchbotble" ))
    {
        MDNS.enableWorkstation();
        Serial.println( "MDNS responder started" );
    }

    BLEDevice::init( "" );

    server.on( "/", handleRoot );



    server.onRequestBody(
        []( AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total )
        {
            if (request->method() == HTTP_POST)
            {
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
                        }
                    }
                    else
                    {
                        String msg = "Bad Request";
                        request->send( 400, "text/plain", msg );
                    }
                }
            }
        }
    );

    server.on( "/api/v1/devices", HTTP_GET, []( AsyncWebServerRequest* request )
        {
            Serial.println( "Received request for devices" );
            char* buf = (char*)malloc( 2048 );
            BLE_Devices.AllToJson( buf, 2048, false );
            Serial.println( buf );
            request->send( 200, "application/json", buf );
            free( buf );
        } );

    server.on( "/api/v1/device", HTTP_GET, []( AsyncWebServerRequest* request )
        {
            Serial.println( "Received request for device" );
            String address = request->arg( "address" );

            int deviceIdx = BLE_Devices.FindDevice( address.c_str() );
            
            char* buf = (char*)malloc( 2048 );
            BLE_Devices.DeviceToJson( deviceIdx, buf, 2048 );
            Serial.println( buf );
            request->send( 200, "application/json", buf );
            free( buf );
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

} // End of setup.


// This is the Arduino main loop function.
void loop()
{
    for (;;)
    {
        // Check if there is a BLE command to send
        BLE_COMMAND BLECommand;
        if (BLECommandQ.Pop( &BLECommand ))
        {
            WriteToBLEDevice( &BLECommand );
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
        Serial.printf( "[HTTP] POST... code: %d\n", httpCode );
    }
    else
    {
        Serial.printf( "[HTTP] POST... failed, code %i, error: %s\n", httpCode, http.errorToString( httpCode ).c_str() );
    }

    http.end();

    return httpCode;
}

void SendChangedDevices()
{
    // This object changed so send to registered callbacks
    char* deviceBuf = (char*)malloc( 2048 );
    int bytes = BLE_Devices.AllToJson( deviceBuf, 2048, true );

    Serial.print( "Sending: " );
    Serial.println( deviceBuf );

    char* addresBuf = (char*)malloc( 256 );
    uint8_t i = 0;
    while (OurCallbacks.Get( i++, addresBuf, 255 ))
    {
        Serial.println( "Sending Device Changes" );
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

    bool complete = false;
    int retries = 5;

    BLEAddress bleAddress( BLECommand->Address );

    while (!complete && ( retries-- > 0 ))
    {
        NimBLEScanResults results = pBLEScan->start( 2 );
        for (int i = 0; ( i < results.getCount() ) && !complete; i++)
        {
            NimBLEAdvertisedDevice device = results.getDevice( i );

            if (device.getAddress() == bleAddress)
            {
                NimBLEClient* pBLEClient = NimBLEDevice::createClient();

                Serial.println( "Connecting to client..." );
                if (pBLEClient->connect( &device ))
                {
                    //success
                    Serial.println( "Client connected" );

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
                    Serial.println( "Disconnect client" );
                }
                else
                {
                    Serial.println( "Failed to connected to client" );
                }

                NimBLEDevice::deleteClient( pBLEClient );
                break;
            }
        }
    }

    pBLEScan->start( 0, nullptr, false );

}