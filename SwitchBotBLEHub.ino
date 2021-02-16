/**
   A BLE client example that is rich in capabilities.
   There is a lot new capabilities implemented.
   author unknown
   updated by chegewara

   https://github.com/nkolban/esp32-snippets/blob/master/Documentation/BLE%20C%2B%2B%20Guide.pdf
*/

#if CONFIG_FREERTOS_UNICORE
#define ARDUINO_RUNNING_CORE 0
#else
#define ARDUINO_RUNNING_CORE 1
#endif

#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "BLE_Device.h"

#include "secrets.h"    //Define a file called secrets.h and put in your WiFi SSID and Password as #defines. e.g. #define WIFI_SSIS "ROUTER_SSID"

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASS;

BLE_Device BLE_Devices;
ClientCallbacks OurCallbacks;

void TaskWebserver( void* pvParameters );
void TaskBLEClient( void* pvParameters );

WebServer server( 80 );

const int led = 13;

#include "BLEDevice.h"
#include <ArduinoJson.h>
#include <heltec.h>

// The remote service we wish to connect to.
static BLEUUID serviceUUID( "cba20d00-224d-11e6-9fb8-0002a5d5c51b" );
// The characteristic of the remote service we are interested in.
static BLEUUID    charUUID( "cba20003-224d-11e6-9fb8-0002a5d5c51b" );

void handleRoot()
{
    digitalWrite( led, 1 );
    server.send( 200, "text/plain", "hello from SwitchBot BLE Hub!" );
    digitalWrite( led, 0 );
}

void handleNotFound()
{
    digitalWrite( led, 1 );
    String message = "File Not Found\n\n";
    message += "URI: ";
    message += server.uri();
    message += "\nMethod: ";
    message += ( server.method() == HTTP_GET ) ? "GET" : "POST";
    message += "\nArguments: ";
    message += server.args();
    message += "\n";
    for (uint8_t i = 0; i < server.args(); i++)
    {
        message += " " + server.argName( i ) + ": " + server.arg( i ) + "\n";
    }
    server.send( 404, "text/plain", message );
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

class MyClientCallback : public BLEClientCallbacks
{
    void onConnect( BLEClient* pclient )
    {}

    void onDisconnect( BLEClient* pclient )
    {
        Serial.println( "onDisconnect" );
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
    void onResult( BLEAdvertisedDevice advertisedDevice )
    {
        // We have found a device, let us now see if it contains the service we are looking for.
        if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService( serviceUUID ))
        {
            // Serial.print( "\nService UUID = " );
            // Serial.print( advertisedDevice.getServiceDataUUID().toString().c_str() );
            // Serial.print( " Address = " );
            // Serial.print( advertisedDevice.getAddress().toString().c_str() );
            // Serial.print( " Service adata:  = " );
            // char* pHex = BLEUtils::buildHexData( nullptr, (uint8_t*)advertisedDevice.getServiceData().data(), advertisedDevice.getServiceData().length() );
            // Serial.println( pHex );
            // Serial.println( "" );

            BLE_Devices.AddDevice( advertisedDevice.getAddress().toString().c_str(), advertisedDevice.getRSSI(), (uint8_t*)advertisedDevice.getServiceData().data(), advertisedDevice.getServiceData().length() );
        }
        else
        {
            //Serial.println( "Wrong service ID" );
        }
    } // onResult
}; // MyAdvertisedDeviceCallbacks

void restartBLE()
{
    Serial.println( "Resting BLE and staring new scan" );
    //    BLEDevice::init( "" );

    BLEScan* pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks( new MyAdvertisedDeviceCallbacks(), true );
    pBLEScan->setInterval( 1000 );
    pBLEScan->setWindow( 200 );
    pBLEScan->setActiveScan( true );
    pBLEScan->start( 5, false );
    Serial.println( "BLE scan complete" );
}

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

    server.on( "/api/v1/devices", []()
        {
            Serial.println( "Received request for devices" );
            char* buf = (char*)malloc( 2048 );
            BLE_Devices.AllToJson( buf, 2048, false );
            Serial.println( buf );
            server.send( 200, "application/json", buf );
            free( buf );
        } );

    server.on( "/api/v1/callback/add", HTTP_POST, []()
        {
            //Handling function implementation
            String body = server.arg( "plain" );
            DynamicJsonDocument doc( 500 );
            deserializeJson( doc, body );
            JsonObject callbackAddress = doc.as<JsonObject>();
            if (OurCallbacks.Add( callbackAddress[ "uri" ], millis() ))
            {
                String msg = "OK";
                server.send( 200, "text/plain", msg );
            }
            else
            {
                String msg = "Bad Request";
                server.send( 400, "text/plain", msg );
            }
        } );

    server.on( "/api/v1/callback/remove", HTTP_POST, []()
        {
            //Handling function implementation
            String body = server.arg( "plain" );
            Serial.println( body );
            DynamicJsonDocument doc( 500 );
            deserializeJson( doc, body );
            JsonObject callbackAddress = doc.as<JsonObject>();
            if (OurCallbacks.Remove( (const char*)callbackAddress[ "uri" ] ))
            {
                server.send( 200, "text/plain", "OK" );
            }
            else
            {
                server.send( 400, "text/plain", "Bad Request" );
            }
        } );

    server.onNotFound( handleNotFound );

    server.begin();
    Serial.println( "HTTP server started" );

    // Retrieve a Scanner and set the callback we want to use to be informed when we
    // have detected a new device.  Specify that we want active scanning and start the
    // scan to run for 5 seconds.
    BLEScan* pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks( new MyAdvertisedDeviceCallbacks(), true );
    pBLEScan->setInterval( 510 );
    pBLEScan->setWindow( 200 );
    pBLEScan->setActiveScan( true );
    pBLEScan->start( 0, nullptr, false );

    xTaskCreatePinnedToCore(
        TaskBLEClient
        , "TaskBLEClient"   // A name just for humans
        , 4096  // This stack size can be checked & adjusted by reading the Stack Highwater
        , NULL
        , 2  // Priority, with 3 (configMAX_PRIORITIES - 1) being the highest, and 0 being the lowest.
        , NULL
        , ARDUINO_RUNNING_CORE );


    xTaskCreatePinnedToCore(
        TaskWebserver
        , "TaskWebserver"   // A name just for humans
        , 4096  // This stack size can be checked & adjusted by reading the Stack Highwater
        , NULL
        , 1  // Priority, with 3 (configMAX_PRIORITIES - 1) being the highest, and 0 being the lowest.
        , NULL
        , ARDUINO_RUNNING_CORE );


    Serial.println( "Application started" );

} // End of setup.


// This is the Arduino main loop function.
void loop()
{

} // End of loop

int SendDeviceChange( const char* host )
{
    //host = "192.168.1.1" ip or dns

    char* buf = (char*)malloc( 2048 );
    int bytes = BLE_Devices.AllToJson( buf, 2048, true );

    Serial.print( "Connecting to " );
    Serial.println( host );

    HTTPClient http;

    // configure target server and url
    http.begin( host ); //HTTP
    http.addHeader("Content-Type", "application/json"); 

    Serial.print( "Sending: " );
    Serial.println( buf );

    // start connection and send HTTP header
    int httpCode = http.POST( (uint8_t*)buf, bytes );

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

    free( buf );

    return httpCode;
}
void SendChangedDevices()
{
    // This object changed so send to registered callbacks
    char* buf = (char*)malloc( 256 );
    uint8_t i = 0;
    while (OurCallbacks.Get( i++, buf, 255 ))
    {
        Serial.println( "Sending Device Changes" );
        if (SendDeviceChange( buf ) == -1)
        {
            // remove refused connection
            i--;
            OurCallbacks.Remove( i );
        }
    }
    free( buf );
}

void TaskBLEClient( void* pvParameters )  // This is a task.
{
    for (;;)
    {
        //        Serial.println( "Start of BLE loop" );
        static char outstr[ 50 ];

        if (BLE_Devices.HasChanged())
        {
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

            BLE_Devices.ClearChanged();
        }
        else
        {
            //            Serial.println( "No Changes" );
        }

        OurCallbacks.Check( millis() );

        // if (BLEDevice::getInitialized())
        // {
        //     // Re-initialise the BLE stack to ensure it picks up the devices again
        //     BLEDevice::deinit( false );
        vTaskDelay( 1000 ); // Delay a second between loops.
   // }

   //restartBLE();
//        Serial.println( "End of BLE loop" );
    }
}
void TaskWebserver( void* pvParameters )  // This is a task.
{
    (void)pvParameters;

    for (;;)
    {
//        OurCallbacks.Check( millis() );
        server.handleClient();
//        vTaskDelay( 100 );
    }
}
