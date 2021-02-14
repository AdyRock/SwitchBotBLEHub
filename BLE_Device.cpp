#include "BLE_Device.h"
#include <string.h>
#include <stdio.h>
#include "Arduino.h"

BLE_Device::BLE_Device()
{
    NumDevices = 0;
    Changed = false;

    memset( BLE_devices, 0, sizeof( BLE_devices ) );
    Serial.println( "BLE Device Class initialised" );
}

BLE_Device::~BLE_Device()
{

}

bool BLE_Device::HasChanged()
{
    return Changed;
}

int BLE_Device::FindDevice( const char* MAC )
{
    for (uint8_t i = 0; i < NumDevices; i++)
    {
        if (strcmp( BLE_devices[ i ].MAC, MAC ) == 0)
        {
            //            Serial.printf( "Found %s @ %i\n", MAC, i );

            return i;
        }
    }

    return -1;
}

bool BLE_Device::AddDevice( const char* MAC, int rssi, uint8_t* BLEData, uint8_t BLEDataSize )
{
    if (( NumDevices >= 50 ) || ( BLEDataSize < 3 ) || ( BLEDataSize > sizeof( BLE_devices[ NumDevices ].Data ) ))
    {
        return false;
    }

    int i = FindDevice( MAC );

    if (i >= 0)
    {
        // Device already in the array so just update it
        if (CompareDevice( i, rssi, BLEData, BLEDataSize ))
        {
            // They are the same
//            Serial.printf( "Matched %s\n", MAC );
            return true;
        }

        // Update the existing device
        UpdateDevice( i, rssi, BLEData, BLEDataSize );
        return true;
    }

    //    Serial.printf( "Added %s @ %i\n", MAC, NumDevices );

    strcpy( BLE_devices[ NumDevices ].MAC, MAC );
    memcpy( BLE_devices[ NumDevices ].Data, BLEData, BLEDataSize );
    BLE_devices[ NumDevices ].DataSize = BLEDataSize;
    BLE_devices[ NumDevices ].Changed = true;
    Changed = true;
    NumDevices++;

    return true;
}

bool BLE_Device::CompareDevice( uint8_t Index, int rssi, uint8_t* BLEData, uint8_t BLEDataSize )
{
    if (BLEDataSize != BLE_devices[ Index ].DataSize)
    {
        return false;
    }

    return ( memcmp( BLE_devices[ Index ].Data, BLEData, BLE_devices[ Index ].DataSize ) == 0 );
}

void BLE_Device::UpdateDevice( uint8_t Index, int rssi, uint8_t* BLEData, uint8_t BLEDataSize )
{
    memcpy( BLE_devices[ Index ].Data, BLEData, BLE_devices[ Index ].DataSize );
    BLE_devices[ Index ].DataSize = BLEDataSize;
    BLE_devices[ Index ].Changed = true;
    Changed = true;

    //    Serial.printf("Updated %s @ %i\n",  BLE_devices[ Index ].MAC, Index );
}

// Returns false if Index is beyond the last entry
bool BLE_Device::GetSWDevice( uint8_t Index, SWITCHBOT& Device )
{
    if (Index < NumDevices)
    {
        parseDevice( BLE_devices[ Index ], Device );
        return true;
    }
    return false;
}

int BLE_Device::DeviceToJson( uint8_t Index, char* Buf, int BufSize )
{
    SWITCHBOT Device;
    if (GetSWDevice( Index, Device ))
    {
        int bytes = snprintf( Buf, BufSize, "{\"address\":\"%s\",\"rssi\":%i,\"serviceData\":", Device.MAC, Device.rssi );

        if (Device.model == 'c')
        {
            bytes += snprintf( Buf + bytes, BufSize - bytes, "{\"model\":\"c\",\"modelName\":\"WoCurtain\",\"calibration\":%s,\"battery\":%i,\"position\":%i,\"lightLevel\":%i}}",
                ( Device.curtain.calibration ? "true" : "false" ), Device.curtain.battery, Device.curtain.position, Device.curtain.lightLevel );

            return bytes;
        }

        if (Device.model == 'H')
        {
            bytes += snprintf( Buf + bytes, BufSize - bytes, "{\"model\":\"H\",\"modelName\":\"WoHand\",\"mode\":%s,\"battery\":%i,\"state\":%i}}",
                ( Device.bot.mode ? "true" : "false" ), Device.curtain.battery, Device.bot.state );

            return bytes;
        }

        if (Device.model == 'T')
        {
            bytes += snprintf( Buf + bytes, BufSize - bytes, "{\"model\":\"T\",\"modelName\":\"WoSensorTH\",\"temperature\":{\"c\": %0.1f},\"battery\":%i,\"humidity\":%i}}",
                Device.thermometer.temperature, Device.thermometer.battery, Device.thermometer.humidity );

            return bytes;
        }
    }

    return 0;
}

int BLE_Device::AllToJson( char* Buf, int BufSize, bool OnlyChanged )
{
    int totaleBytes = 1;
    *Buf = '[';
    for (uint8_t i = 0; i < NumDevices; i++)
    {
        if (totaleBytes >= BufSize)
        {
            break;
        }

        if (OnlyChanged && !BLE_devices[ i ].Changed)
        {
            continue;
        }

        int bytes = DeviceToJson( i, Buf + totaleBytes, BufSize - totaleBytes );
        if (bytes > 0)
        {
            totaleBytes += bytes;
            Buf[ totaleBytes++ ] = ',';
        }
    }

    totaleBytes--;

    Buf[ totaleBytes++ ] = ']';
    Buf[ totaleBytes ] = 0;

    return totaleBytes;
}

void BLE_Device::ClearChanged()
{
    for (uint8_t i = 0; i < NumDevices; i++)
    {
        BLE_devices[ i ].Changed = false;
    }

    Changed = false;
}

// ********************************* Private functions ********************************************

bool BLE_Device::parseDevice( BLE_DEVICE& Device, SWITCHBOT& SW_Device )
{
    // Set model to 0 incase the device is not valid
    SW_Device.model = 0;

    if (Device.DataSize < 3)
    {
        return false;
    }

    SW_Device.model = Device.Data[ 0 ];
    strcpy( SW_Device.MAC, Device.MAC );
    SW_Device.rssi = Device.rssi;

    if (Device.Data[ 0 ] == 'T')
    {
        return parseThermometer( Device, SW_Device );
    }
    else if (Device.Data[ 0 ] == 'H')
    {
        return parseBot( Device, SW_Device );
    }
    else if (Device.Data[ 0 ] == 'c')
    {
        return parseCurtain( Device, SW_Device );
    }

    return false;
}

bool BLE_Device::parseBot( BLE_DEVICE& Device, SWITCHBOT& SW_Device )
{
    if (Device.DataSize != 3)
    {
        return false;
    }

    uint8_t byte1 = Device.Data[ 1 ];
    uint8_t byte2 = Device.Data[ 2 ];

    SW_Device.bot.mode = ( ( byte1 & 0b10000000 ) != 0 ); // Whether the light switch Add-on is used or not
    SW_Device.bot.state = ( ( byte1 & 0b01000000 ) != 0 ); // Whether the switch status is ON or OFF
    SW_Device.bot.battery = byte2 & 0b01111111; // %

//    Serial.printf( "Parsed Bot: state = %i, battery = %i\n", SW_Device.bot.state, SW_Device.bot.battery );
    return true;
}

bool BLE_Device::parseCurtain( BLE_DEVICE& Device, SWITCHBOT& SW_Device )
{
    if (Device.DataSize != 5)
    {
        return false;
    }

    uint8_t byte1 = Device.Data[ 1 ];
    uint8_t byte2 = Device.Data[ 2 ];
    uint8_t byte3 = Device.Data[ 3 ];
    uint8_t byte4 = Device.Data[ 4 ];

    SW_Device.curtain.calibration = ( ( byte1 & 0b01000000 ) != 0 ); // Whether the calibration is completed
    SW_Device.curtain.battery = ( byte2 & 0b01111111 ); // %
    SW_Device.curtain.position = ( byte3 & 0b01111111 ); // current position %
    SW_Device.curtain.moving = ( byte3 & 0b10000000 ) != 0 ? true : false;
    SW_Device.curtain.lightLevel = ( byte4 >> 4 ) & 0b00001111; // light sensor level (1-10)

//    Serial.printf( "Parsed Curtain: position = %i, battery = %i\n", SW_Device.curtain.position, SW_Device.curtain.battery );

    return true;
}

bool BLE_Device::parseThermometer( BLE_DEVICE& Device, SWITCHBOT& SW_Device )
{
    if (Device.DataSize != 6)
    {
        return false;
    }

    uint8_t byte2 = Device.Data[ 2 ];
    uint8_t byte3 = Device.Data[ 3 ];
    uint8_t byte4 = Device.Data[ 4 ];
    uint8_t byte5 = Device.Data[ 5 ];

    uint8_t temp_sign = ( byte4 & 0b10000000 ) ? 1 : -1;
    SW_Device.thermometer.temperature = temp_sign * ( ( byte4 & 0b01111111 ) + ( (float)byte3 / 10 ) );

    SW_Device.thermometer.humidity = ( byte5 & 0b01111111 );
    SW_Device.thermometer.battery = ( byte2 & 0b01111111 );

    //    Serial.printf( "Parsed Thermometer: temperature = %0.1f, humidity = %i, battery = %i\n",SW_Device.thermometer.temperature, SW_Device.thermometer.humidity, SW_Device.thermometer.battery );

    return true;
}

//=============================================================================================
// ClientCallbacks Class

ClientCallbacks::ClientCallbacks()
{
    memset( Callbacks, 0, 5 );
    NumCallbacks = 0;
}

ClientCallbacks::~ClientCallbacks()
{

}

bool ClientCallbacks::Add( const char* url, unsigned long t )
{
    if (( url == nullptr ) || ( *url == 0 ))
    {
        Serial.println( "Request to add URI failed: URI is not defined" );
        return false;
    }
    
    Serial.printf( "Request to add: %s\n", url );

    for (uint8_t i = 0; i < NumCallbacks; i++)
    {
        if (strcmp( Callbacks[ i ].url, url ) == 0)
        {
            Callbacks[ i ].activatedTime = t;
            Serial.println( "Request OK, URI is already registered." );
            return true;
        }
    }

    Callbacks[ NumCallbacks ].activatedTime = t;
    strcpy( Callbacks[ NumCallbacks ].url, url );
    NumCallbacks++;

    Serial.println( "Request OK, URI has been added." );

    return true;

}

bool ClientCallbacks::Get( uint8_t Index, char* buf, int BufLength )
{
    if (Index < NumCallbacks)
    {
        strncpy( buf, Callbacks[ Index ].url, BufLength );
        return true;
    }

    return false;
}

bool ClientCallbacks::Remove( uint8_t Index )
{
    for (int8_t x = Index; x < NumCallbacks - 1; x++)
    {
        Callbacks[ x ] = Callbacks[ x + 1 ];
    }
    
    NumCallbacks--;
}

bool ClientCallbacks::Remove( const char* url )
{
    if (( url == nullptr ) || ( *url == 0 ))
    {
        return false;
    }
    
    for (uint8_t i = 0; i < NumCallbacks; i++)
    {
        // Search for the entry
        if (strcmp( Callbacks[ i ].url, url ) == 0)
        {
            // Found it so remove it.
            return Remove( i );
        }
    }

    return false;
}

void ClientCallbacks::Check( unsigned long t )
{
    for (uint8_t i = 0; i < NumCallbacks; i++)
    {
        if (Callbacks[ i ].activatedTime > ( t + ( 5 * 60 * 1000 ) ))
        {
            Remove( i );
            i--;
        }
    }
}

bool ClientCallbacks::HasCallbacks()
{
    return ( NumCallbacks > 0 );
}