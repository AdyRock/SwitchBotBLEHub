/*
    <SwitchBotBLEHub:- Turn a ESP32 Arduio compatible board into a hub>
    Copyright (C) <2020>  <Adrian Rockall>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <ESPAsyncWebServer.h>    // https://github.com/me-no-dev/ESPAsyncWebServer
#include <ESPAsyncWiFiManager.h>  // https://github.com/alanswx/ESPAsyncWiFiManager
#include <AsyncUDP.h>             // https://github.com/espressif/arduino-esp32/tree/master/libraries/AsyncUDP
#include <NimBLEDevice.h>         // https://github.com/h2zero/NimBLE-Arduino/blob/master/docs/New_user_guide.md
#include <ArduinoJson.h>
#include <ElegantOTA.h>           // https://github.com/ayushsharma82/ElegantOTA

#include "BLE_Device.h"

const char* version = "Hello! SwitchBot BLE Hub V1.7";

const char HTML[] PROGMEM = "<!DOCTYPE html>\n<html>\n  <head>\n    <meta http-equiv=\"content-type\" content=\"text/html; charset=UTF-8\">\n    <title>Home</title>\n  </head>\n  <body>\n    <h1><b>Welcome to the ESP32 SwitchBot BLE hub for Homey.</b></h1>\n    <p><i>Version 1.7</i></p>\n    <p><a href=\"/update\">Update the firmware</a></p>\n    <p><a href=\"/api/v1/devices\">View the registered devices</a></p>\n  </body>\n</html>\n";
BLE_Device BLE_Devices;
ClientCallbacks OurCallbacks;

CommandQ BLECommandQ;
AsyncWebServer server( 80 );
DNSServer dns;
AsyncUDP udp;

unsigned long ota_progress_millis = 0;

const int led = 14;

char macAddress[ 18 ];
unsigned long sendBroadcast = 0;
char lookingForBLEAddress[ 18 ];
uint8_t BLENotifyData[ 50 ];
int BLENotifyLength = 0;

// The remote service we wish to connect to.
static BLEUUID serviceUUID( "cba20d00-224d-11e6-9fb8-0002a5d5c51b" );
// The characteristic of the remote service we are interested in.
static BLEUUID    charUUID( "cba20002-224d-11e6-9fb8-0002a5d5c51b" );
// The characteristic of the notification service we are interested in.
static BLEUUID    notifyUUID( "cba20003-224d-11e6-9fb8-0002a5d5c51b" );

void handleRoot( AsyncWebServerRequest* request )
{
    digitalWrite( led, 1 );
    request->send( 200, "text/html", HTML );
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

void notifyCallback(
    BLERemoteCharacteristic* pBLERemoteCharacteristic,
    uint8_t* pData,
    size_t length,
    bool isNotify )
{
    // Serial.print( "Notify callback for characteristic " );
    // Serial.print( pBLERemoteCharacteristic->getUUID().toString().c_str() );
    // Serial.print( " of data length " );
    // Serial.println( (unsigned long)length );

    if (length > 50)
    {
        length = 50;
    }

    memcpy( BLENotifyData, pData, length );
    BLENotifyLength = length;
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
        NimBLEUUID id1((uint16_t)0x0d00);
        NimBLEUUID id2((uint16_t)0xfd3d);
        NimBLEUUID devicId = advertisedDevice->getServiceDataUUID();
        if ((devicId == id1)  || (devicId == id2))
        {
            if (BLE_Devices.AddDevice( advertisedDevice->getAddress().toString().c_str(), advertisedDevice->getRSSI(), (uint8_t*)advertisedDevice->getServiceData().data(), advertisedDevice->getServiceData().length(), (uint8_t*)advertisedDevice->getManufacturerData().data(), advertisedDevice->getManufacturerData().length() ))
            {
                if (lookingForBLEAddress[ 0 ] != 0)
                {
                    if (strcmp( lookingForBLEAddress, advertisedDevice->getAddress().toString().c_str() ) == 0)
                    {
                        BLEScan* pBLEScan = BLEDevice::getScan();
                        pBLEScan->stop();
                    }

                }
            }
        }
    }; // onResult
}; // MyAdvertisedDeviceCallbacks

void onOTAStart()
{
  // Log when OTA has started
  Serial.println("OTA update started!");
  // <Add your own code here>
}

void onOTAProgress(size_t current, size_t final)
{
  // Log every 1 second
  if (millis() - ota_progress_millis > 1000) {
    ota_progress_millis = millis();
    Serial.printf("OTA Progress Current: %u bytes, Final: %u bytes\n", current, final);
  }
}

void onOTAEnd(bool success)
{
  // Log when OTA has finished
  if (success) {
    Serial.println("OTA update finished successfully!");
  } else {
    Serial.println("There was an error during OTA update!");
  }
  // <Add your own code here>
}

void setup()
{    
    pinMode( led, OUTPUT );
    digitalWrite( led, 0 );
    Serial.begin( 115200 );
    Serial.println( "Starting Arduino BLE Client application..." );

    AsyncWiFiManager wifiManager(&server,&dns);
//    wifiManager.resetSettings();
    wifiManager.autoConnect("SwitchBot_ESP32");   
     
    Serial.println( "" );
    Serial.print( "Connected to " );
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
                            char Buf[ 100 ];
                            int bytes = snprintf( Buf, 100, "OK: %i", BLE_Devices.GetNumberOfDevices() );

                            String msg = Buf;
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
                            String sourcIP = request->client()->remoteIP().toString();
                            // Serial.printf( "Received request to write device %s with %s (%i) from %s\n", clientAddress.c_str(), dataToWrite.c_str(), dataToWrite.length(), sourcIP.c_str() );

                            if (BLECommandQ.Push( clientAddress, dataToWrite, sourcIP ))
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
            char* buf = (char*)malloc( 4096 );
            BLE_Devices.AllToJson( buf, 4096, false, macAddress );
            Serial.println( buf );
            request->send( 200, "application/json", buf );
            free( buf );
            digitalWrite( led, 0 );

        } );

    server.on( "/api/v1/device", HTTP_GET, []( AsyncWebServerRequest* request )
        {
            digitalWrite( led, 1 );
            String address = request->arg( "address" );
            Serial.printf( "Received request for device: %s\n", address.c_str() );

            int deviceIdx = BLE_Devices.FindDevice( address.c_str() );

            char* buf = (char*)malloc( 2048 );
            BLE_Devices.DeviceToJson( deviceIdx, buf, 2048, macAddress );
            Serial.println( buf );
            request->send( 200, "application/json", buf );
            free( buf );
            digitalWrite( led, 0 );
        } );


    server.onNotFound( handleNotFound );

    ElegantOTA.begin(&server);
    
    // ElegantOTA callbacks
    ElegantOTA.onStart(onOTAStart);
    ElegantOTA.onProgress(onOTAProgress);
    ElegantOTA.onEnd(onOTAEnd);

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
                    Serial.println( "Received: Are you there SwitchBot?" );
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
            Serial.printf( "\n***Broadcasting my details: %s, %s***\n", macAddress, WiFi.localIP().toString().c_str() );
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
        }

        OurCallbacks.Check( millis() ); // Check if any of the registered callbacks have timedout
    } // end of endless loop ;-)

} // End of loop

int SendDeviceChange( const char* host, const char* data, int bytes )
{
    //host = "192.168.1.1", ip or dns

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
    if (bytes > 0)
    {
        // Serial.print( "\nSending: " );
        // Serial.println( deviceBuf );

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
    }
    
    free( deviceBuf );
}

void WriteToBLEDevice( BLE_COMMAND* BLECommand )
{
    BLEScan* pBLEScan = BLEDevice::getScan();
    pBLEScan->stop();

    BLEAddress bleAddress( BLECommand->Address );
    Serial.printf( "Sending command to BLE device: %s\n", BLECommand->Address );

    // Register the device we are looking for so the scan stop as soon as it is found
    strcpy( lookingForBLEAddress, BLECommand->Address );
    strlwr(lookingForBLEAddress);
    
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
            // Serial.println( "Connecting to device..." );
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

                        // Get the notification characteristic
                        BLERemoteCharacteristic* rn = nullptr;
                        if ( ((BLECommand->Data[ 0 ] == 87) && (BLECommand->Data[ 1 ] == 15) && (BLECommand->Data[ 2 ] == 72) && (BLECommand->Data[ 3 ] == 1)) ||
                             (BLECommand->Data[ 0 ] == 87) && (BLECommand->Data[ 1 ] == 2))
                        {
                            // This request requires data to be return via the notification
                            // Serial.println( "Getting notification characteristic" );
                            rn = rs->getCharacteristic( notifyUUID );
                            
                            if (rn)
                            {
                                // Serial.println( "Registering notification" );
                                BLENotifyLength = 0;
                                if (!rn->subscribe( true, notifyCallback ))
                                {
                                    Serial.println( "Registering notification FAILED!" );
                                }
                            }
                        }

                        rc->writeValue( BLECommand->Data, BLECommand->DataLen );
                        Serial.println( "Data sent" );
                        
                        if (rn)
                        {
                            Serial.println( "Waiting for notification" );
                            unsigned long endTime = millis() + 2000;
                            while ((BLENotifyLength == 0) && (millis() < endTime));
                            if (BLENotifyLength > 0)
                            {
                                Serial.println( "Got notification" );

                                // Return data
                                char* replyBuf = (char*)malloc( 300 );
    
                                int idx = BLE_Devices.FindDevice( BLECommand->Address );
                                SWITCHBOT Device;
                                if (BLE_Devices.GetSWDevice( idx, Device ))
                                {
                                    int bytes = 0;

                                    if (Device.model == 'u')
                                    {
                                        bytes = snprintf( replyBuf, 300, "[{\"hubMAC\":\"%s\",\"address\":\"%s\",\"serviceData\":{\"model\":\"u\",\"modelName\":\"WoBulb\"},\"replyData\":[",
                                            macAddress, BLECommand->Address );
                                    }
                                    else if (Device.model == 'x')                                  
                                    {
                                        bytes = snprintf( replyBuf, 300, "[{\"hubMAC\":\"%s\",\"address\":\"%s\",\"serviceData\":{\"model\":\"x\",\"modelName\":\"WoBlindTilt\"},\"replyData\":[",
                                            macAddress, BLECommand->Address );
                                    }
                                    
                                    if (bytes > 0)
                                    {
                                        // Convert raw data to JsonArray
                                        for (int i = 0; i < BLENotifyLength; i++)
                                        {
                                            bytes += snprintf( replyBuf + bytes, 300 - bytes, "%i,", BLENotifyData[i]);
                                        }
        
                                        bytes--;
                                        bytes += snprintf( replyBuf + bytes, 300 - bytes, "]}]");
        
                                        char* replyAddress = (char*)malloc( 300 );
                                        if (OurCallbacks.Find( BLECommand->ReplyTo, replyAddress, 300 ))
                                        {
                                            // Serial.printf( "Sending to %s: %s\n", replyAddress, replyBuf );
                                            SendDeviceChange( replyAddress, replyBuf, bytes );
                                        }
                                        else
                                        {
                                            Serial.printf( "Callback URL %s not found\n", BLECommand->ReplyTo );
                                        }
    
                                        free( replyAddress );
                                    }
                                      else
                                      {
                                          Serial.printf( "Don't understand for for model %c\n", Device.model );
                                      }
                                }
                                else
                                {
                                    Serial.printf( "Callback URL %s not found\n", BLECommand->ReplyTo );
                                }
                                
                                free( replyBuf );
                            }

                            rn->unsubscribe();
                        }
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
