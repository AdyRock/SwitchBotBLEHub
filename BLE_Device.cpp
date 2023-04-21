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

#include "BLE_Device.h"
#include <string.h>
#include <stdio.h>
#include "Arduino.h"

BLE_Device::BLE_Device()
{
    NumDevices = 0;
    Changed = false;

    memset( BLE_devices, 0, sizeof( BLE_DEVICE ) * 50 );
    // Serial.println( "BLE Device Class initialised" );
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
            // Serial.printf( "Found %s @ %i\n", MAC, i );

            return i;
        }
    }

    return -1;
}

bool BLE_Device::AddDevice( const char* MAC, int rssi, uint8_t* BLEData, uint8_t BLEDataSize, uint8_t* ManufactureData, uint8_t ManufactureDataSize )
{
    if (BLEData[ 0 ] == 'u')
    {
        if ( (ManufactureDataSize != 13) ||  (ManufactureData[ 1 ] != 9) || (ManufactureData[ 0 ] != 0x69))
        {
            Serial.printf( "Invalid bulb data: size = %i, id1 = %i, id2 = %i\n", ManufactureDataSize, ManufactureData[ 1 ], (ManufactureData[ 0 ] ));
            return false;
        }
    }
    else
    {
        if (( BLEDataSize < 3 ) || ( BLEDataSize > sizeof( BLE_devices[ NumDevices ].Data ) ))
        {
            return false;
        }
    }

    if ((BLEData[ 0 ] != 'i') && (BLEData[ 0 ] != 'T') && (BLEData[ 0 ] != 'H') && (BLEData[ 0 ] != 'c') && (BLEData[ 0 ] != 's') && (BLEData[ 0 ] != 'd') && (BLEData[ 0 ] != 'b') && (BLEData[ 0 ] != 'u') && (BLEData[ 0 ] != 'x') && (BLEData[ 0 ] != 'w'))
    {
        return false;
    }

    int i = FindDevice( MAC );

    if (i >= 0)
    {
        // Device already in the array so just update it
        if (CompareDevice( i, rssi, BLEData, BLEDataSize, ManufactureData, ManufactureDataSize ))
        {
            // They are the same
//            Serial.printf( "Matched %s\n", MAC );
            return true;
        }

        // Update the existing device
        UpdateDevice( i, rssi, BLEData, BLEDataSize, ManufactureData, ManufactureDataSize );
        return true;
    }

    if ( NumDevices >= 50 )
    {
        return false;
    }

    Serial.printf( "Added %s @ %i\n", MAC, NumDevices );

    strcpy( BLE_devices[ NumDevices ].MAC, MAC );
    if (BLEData[ 0 ] == 'u')
    {
        // use manufacture data
        memcpy( BLE_devices[ NumDevices ].Data + 1, ManufactureData, ManufactureDataSize );
        BLE_devices[ NumDevices ].Data[ 0 ] = 'u';
        BLE_devices[ NumDevices ].DataSize = ManufactureDataSize + 1;
    }
    else if (BLEData[ 0 ] == 'w')
    {
        // use manufacture data
        memcpy( BLE_devices[ NumDevices ].Data + 1, ManufactureData, ManufactureDataSize );
        BLE_devices[ NumDevices ].Data[ 0 ] = 'w';
        BLE_devices[ NumDevices ].Data[ 2 ] = BLEData[ 2 ];
        BLE_devices[ NumDevices ].DataSize = ManufactureDataSize + 1;
    }
    else
    {
        memcpy( BLE_devices[ NumDevices ].Data, BLEData, BLEDataSize );
        BLE_devices[ NumDevices ].DataSize = BLEDataSize;
    }

    BLE_devices[ NumDevices ].Changed = true;
    BLE_devices[ NumDevices ].rssi = rssi;

    // Serial.printf("Added %s @ %i = %c\n",  BLE_devices[ NumDevices ].MAC, NumDevices, BLEData[ 0 ] );

    Changed = true;
    NumDevices++;


    return true;
}

// Return true if the device data is the same
bool BLE_Device::CompareDevice( uint8_t Index, int rssi, uint8_t* BLEData, uint8_t BLEDataSize, uint8_t* ManufactureData, uint8_t ManufactureDataSize )
{
    if ((BLEData[0] == 'u') || (BLEData[0] == 'w') || (BLEData[0] == 'x'))
    {
        if (ManufactureDataSize != BLE_devices[ Index ].DataSize - 1)
        {
//            Serial.printf("Compare MF Data Size different %s @ %i = %c\n",  BLE_devices[ Index ].MAC, Index, BLEData[ 0 ] );
            return false;
        }
    }
    else
    {
        if (BLEDataSize != BLE_devices[ Index ].DataSize)
        {
//            Serial.printf("Compare AD Data Size different %s @ %i = %c\n",  BLE_devices[ Index ].MAC, Index, BLEData[ 0 ] );
            return false;
        }
    }

    if (BLEData[0] == 's')
    {
        // Compare the presence sensor differently as there are bytes that change continuosly
        if ((BLEData[1] != BLE_devices[ Index ].Data[1]) || (BLEData[2] != BLE_devices[ Index ].Data[2]) || (BLEData[5] != BLE_devices[ Index ].Data[5]))
        {
            return false;
        }

        return true;
    }
    
    if (BLEData[0] == 'd')
    {
        // Compare the contact sensor differently as there are bytes that change continuosly
        if ((BLEData[1] != BLE_devices[ Index ].Data[1]) || (BLEData[2] != BLE_devices[ Index ].Data[2]) || (BLEData[3] != BLE_devices[ Index ].Data[3]) || (BLEData[8] != BLE_devices[ Index ].Data[8]))
        {
            return false;
        }

        return true;
    }
    
    if (BLEData[0] == 'u')
    {
        // Compare the bulb sensor differently as it uses manufacture data
        if ((ManufactureData[ 8 ] != BLE_devices[ Index ].Data[ 9 ]) || (ManufactureData[ 9 ] != BLE_devices[ Index ].Data[ 10 ]) || (ManufactureData[ 10 ] != BLE_devices[ Index ].Data[ 11 ]))
        {
            return false;
        }

        return true;
    }
    
    if (BLEData[0] == 'w')
    {
        // Compare the IO TH sensor differently as it uses manufacture data
        if ((BLEData[ 2 ] != BLE_devices[ Index ].Data[ 2 ]) || (ManufactureData[ 10 ] != BLE_devices[ Index ].Data[ 11 ]) || (ManufactureData[ 11 ] != BLE_devices[ Index ].Data[ 12 ]) || (ManufactureData[ 12 ] != BLE_devices[ Index ].Data[ 13 ]))
        {
            return false;
        }

        return true;
    }
    
    if (BLEData[0] == 'x')
    {
        // Compare the blind / tilt differently as it uses manufacture data
        if ((BLEData[ 2 ] != BLE_devices[ Index ].Data[ 2 ]) || (ManufactureData[ 10 ] != BLE_devices[ Index ].Data[ 11 ]))
        {
            Serial.printf("Compare Blind Tilt false: %i to %i and %i to %i\n", BLEData[ 2 ], BLE_devices[ Index ].Data[ 2 ], ManufactureData[ 10 ], BLE_devices[ Index ].Data[ 11 ] );
            return false;
        }

//        Serial.printf("Compare Blind Tilt true: %i to %i and %i to %i\n", BLEData[ 2 ], BLE_devices[ Index ].Data[ 2 ], ManufactureData[ 8 ], BLE_devices[ Index ].Data[ 9 ] );

        return true;
    }

    return ( memcmp( BLE_devices[ Index ].Data, BLEData, BLE_devices[ Index ].DataSize ) == 0 );
}

void BLE_Device::UpdateDevice( uint8_t Index, int rssi, uint8_t* BLEData, uint8_t BLEDataSize, uint8_t* ManufactureData, uint8_t ManufactureDataSize )
{
    if ((BLEData[ 0 ] == 'u') && (ManufactureData[ 1 ] == 9) && (ManufactureData[ 0 ] == 0x69))
    {
        if (ManufactureDataSize > 20)
        {
            ManufactureDataSize = 20;
        }
        memcpy( BLE_devices[ Index ].Data + 1, ManufactureData, ManufactureDataSize );
        BLE_devices[ Index ].DataSize = ManufactureDataSize + 1;
    }
    else if ((BLEData[ 0 ] == 'w') && (ManufactureData[ 1 ] == 9) && (ManufactureData[ 0 ] == 0x69))
    {
        if (ManufactureDataSize > 20)
        {
            ManufactureDataSize = 20;
        }
        memcpy( BLE_devices[ Index ].Data + 1, ManufactureData, ManufactureDataSize );
        BLE_devices[ Index ].Data[ 2 ] = BLEData[ 2 ];
        BLE_devices[ Index ].DataSize = ManufactureDataSize + 1;
    }
    else if ((BLEData[ 0 ] == 'x') )
    {
        if (ManufactureDataSize > 20)
        {
            ManufactureDataSize = 20;
        }
        memcpy( BLE_devices[ Index ].Data + 1, ManufactureData, ManufactureDataSize );
        BLE_devices[ Index ].Data[ 2 ] = BLEData[ 2 ];
        BLE_devices[ Index ].DataSize = ManufactureDataSize + 1;
    }
    else
    {
        memcpy( BLE_devices[ Index ].Data, BLEData, BLEDataSize );
        BLE_devices[ Index ].DataSize = BLEDataSize;
    }
    BLE_devices[ Index ].Changed = true;
    BLE_devices[ Index ].rssi = rssi;
    Changed = true;

    Serial.printf("Updated %s @ %i = %c\n",  BLE_devices[ Index ].MAC, Index, BLEData[ 0 ] );
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

int BLE_Device::DeviceToJson( uint8_t Index, char* Buf, int BufSize, char* macAddress )
{
    SWITCHBOT Device;
    if (GetSWDevice( Index, Device ))
    {
        int bytes = snprintf( Buf, BufSize, "{\"hubMAC\":\"%s\",\"address\":\"%s\",\"rssi\":%i,\"serviceData\":", macAddress, Device.MAC, Device.rssi );

        if (Device.model == 'c')
        {
            bytes += snprintf( Buf + bytes, BufSize - bytes, "{\"model\":\"%c\",\"modelName\":\"WoCurtain\",\"calibration\":%s,\"battery\":%i,\"position\":%i,\"lightLevel\":%i}}",
                Device.model, ( Device.curtain.calibration ? "true" : "false" ), Device.curtain.battery, Device.curtain.position, Device.curtain.lightLevel );

            return bytes;
        }

        if (Device.model == 'x')
        {
            bytes += snprintf( Buf + bytes, BufSize - bytes, "{\"model\":\"%c\",\"modelName\":\"WoBlindTilt\",\"battery\":%i,\"position\":%i,\"version\":%i}}",
                Device.model, Device.blind.battery, Device.blind.position, Device.blind.version );

            return bytes;
        }

        if (Device.model == 'H')
        {
            bytes += snprintf( Buf + bytes, BufSize - bytes, "{\"model\":\"%c\",\"modelName\":\"WoHand\",\"mode\":%s,\"battery\":%i,\"state\":%i}}",
                Device.model, ( Device.bot.mode ? "true" : "false" ), Device.bot.battery, Device.bot.state );

            return bytes;
        }

        if ((Device.model == 'T') || (Device.model == 'i'))
        {
            bytes += snprintf( Buf + bytes, BufSize - bytes, "{\"model\":\"%c\",\"modelName\":\"WoSensorTH\",\"temperature\":{\"c\": %0.1f},\"battery\":%i,\"humidity\":%i}}",
                Device.model, Device.thermometer.temperature, Device.thermometer.battery, Device.thermometer.humidity );

            return bytes;
        }

        if (Device.model == 's')
        {
            bytes += snprintf( Buf + bytes, BufSize - bytes, "{\"model\":\"%c\",\"modelName\":\"WoPresence\",\"motion\":%i,\"battery\":%i,\"light\":%i}}",
                Device.model, Device.Presence.motion, Device.Presence.battery, Device.Presence.light );

            return bytes;
        }

        if (Device.model == 'd')
        {
            bytes += snprintf( Buf + bytes, BufSize - bytes, "{\"model\":\"%c\",\"modelName\":\"WoContact\",\"motion\":%i,\"battery\":%i,\"light\":%i,\"contact\":%i,\"leftOpen\":%i,\"lastMotion\":%i,\"lastContact\":%i,\"buttonPresses\":%i,\"entryCount\":%i,\"exitCount\":%i}}",
                Device.model, Device.Contact.motion, Device.Contact.battery, Device.Contact.light, Device.Contact.contact, Device.Contact.leftOpen, Device.Contact.lastMotion, Device.Contact.lastContact, Device.Contact.buttonPresses, Device.Contact.entryCount, Device.Contact.exitCount );

            return bytes;
        }

        if (Device.model == 'b')
        {
            bytes += snprintf( Buf + bytes, BufSize - bytes, "{\"model\":\"%c\",\"modelName\":\"WoRemote\",\"data1\":%i,\"data2\":%i,\"data3\":%i}}",
                Device.model, Device.Remote.data1, Device.Remote.data2, Device.Remote.data3 );

            return bytes;
        }

        if (Device.model == 'u')
        {
            bytes += snprintf( Buf + bytes, BufSize - bytes, "{\"model\":\"%c\",\"modelName\":\"WoBulb\",\"sequence\":%i,\"on_off\":%i,\"dim\":%i,\"lightState\":%i}}",
                Device.model, Device.Bulb.sequence, Device.Bulb.on_off, Device.Bulb.dim, Device.Bulb.lightState );

            return bytes;
        }

        if (Device.model == 'w')
        {
            bytes += snprintf( Buf + bytes, BufSize - bytes, "{\"model\":\"%c\",\"modelName\":\"WoIOSensor\",\"temperature\":{\"c\": %0.1f},\"battery\":%i,\"humidity\":%i}}",
                Device.model, Device.thermometer.temperature, Device.thermometer.battery, Device.thermometer.humidity );

            return bytes;
        }

        bytes += snprintf( Buf + bytes, BufSize - bytes, "{\"error\": \"Unknown model %c\"}}", Device.model);
        return bytes;
    }

    return snprintf( Buf, BufSize, "{\"hubMAC\":\"%s\",\"serviceData\":{\"error\": \"Invalid index %i\"}}", macAddress, Index);
}

int BLE_Device::AllToJson( char* Buf, int BufSize, bool OnlyChanged, char* macAddress )
{
    static boolean inAllToJson = false;

    while (inAllToJson)
    {
        
    };

    inAllToJson = true;

    if (OnlyChanged)
    {
        Changed = false;
    }

    int totaleBytes = 1;
    *Buf = '[';
    for (uint8_t i = 0; i < NumDevices; i++)
    {
        if (totaleBytes >= BufSize)
        {
            break;
        }

        if (OnlyChanged)
        {
            if (!BLE_devices[ i ].Changed)
            {
                continue;
            }

            BLE_devices[ i ].Changed = false;
        }

        int bytes = DeviceToJson( i, Buf + totaleBytes, BufSize - totaleBytes, macAddress );
        if (bytes > 0)
        {
            totaleBytes += bytes;
            Buf[ totaleBytes++ ] = ',';
        }
    }

    if (totaleBytes < 3)
    {
        inAllToJson = false;
        return 0;
    }

    totaleBytes--;
    Buf[ totaleBytes++ ] = ']';
    Buf[ totaleBytes ] = 0;

    // Serial.println( Buf );

    inAllToJson = false;
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

    if ((Device.Data[ 0 ] == 'T') || (Device.Data[ 0 ] == 'i'))
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
    else if (Device.Data[ 0 ] == 's')
    {
        return parsePresence( Device, SW_Device );
    }
    else if (Device.Data[ 0 ] == 'd')
    {
        return parseContac( Device, SW_Device );
    }
    else if (Device.Data[ 0 ] == 'b')
    {
        return parseRemote( Device, SW_Device );
    }
    else if (Device.Data[ 0 ] == 'u')
    {
        return parseBulb( Device, SW_Device );
    }
    else if (Device.Data[ 0 ] == 'w')
    {
        return parseIOTH( Device, SW_Device );
    }
    else if (Device.Data[ 0 ] == 'x')
    {
        return parseBlind( Device, SW_Device );
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

    // Serial.printf( "Bot: MAC = %s, state = %i, battery = %i\n", Device.MAC, SW_Device.bot.state, SW_Device.bot.battery );
    return true;
}

bool BLE_Device::parseCurtain( BLE_DEVICE& Device, SWITCHBOT& SW_Device )
{
    if (Device.DataSize < 5)
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

    // Serial.printf( "Curtain: MAC = %s, position = %i, battery = %i\n", Device.MAC, SW_Device.curtain.position, SW_Device.curtain.battery );

    return true;
}

bool BLE_Device::parseBlind( BLE_DEVICE& Device, SWITCHBOT& SW_Device )
{
    if (Device.DataSize < 3)
    {
        return false;
    }
    
    uint8_t byte2 = Device.Data[ 2 ];
    SW_Device.blind.battery = ( byte2 & 0b01111111 ); // %

    if (Device.DataSize >= 11)
    {
        uint8_t byte11 = Device.Data[ 11 ];
        SW_Device.blind.position = ( byte11 & 0b01111111 ); // current position %
        SW_Device.blind.version = 2;
    }
    else
    {
        SW_Device.blind.version = 1;
    }
    // Serial.printf( "Blind: MAC = %s, battery = %i\n", Device.MAC, SW_Device.blind.battery );

    return true;
}


bool BLE_Device::parseIOTH( BLE_DEVICE& Device, SWITCHBOT& SW_Device )
{
    if (Device.DataSize != 13)
    {
        return false;
    }

    uint8_t byte2 = Device.Data[ 2 ];
    uint8_t byte10 = Device.Data[ 11 ];
    uint8_t byte11 = Device.Data[ 12 ];
    uint8_t byte12 = Device.Data[ 13 ];

    uint8_t temp_sign = ( byte11 & 0b10000000 ) ? 1 : -1;
    SW_Device.thermometer.temperature = temp_sign * ( ( byte11 & 0b01111111 ) + ( (float)byte10 / 10 ) );

    SW_Device.thermometer.humidity = ( byte12 & 0b01111111 );
    SW_Device.thermometer.battery = ( byte2 & 0b01111111 );

    // Serial.printf( "Thermometer: MAC = %s, temperature = %0.1f, humidity = %i, battery = %i\n", Device.MAC, SW_Device.thermometer.temperature, SW_Device.thermometer.humidity, SW_Device.thermometer.battery );

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

    // Serial.printf( "Thermometer: MAC = %s, temperature = %0.1f, humidity = %i, battery = %i\n", Device.MAC, SW_Device.thermometer.temperature, SW_Device.thermometer.humidity, SW_Device.thermometer.battery );

    return true;
}

bool BLE_Device::parsePresence( BLE_DEVICE& Device, SWITCHBOT& SW_Device )
{
    if (Device.DataSize != 6)
    {
        return false;
    }

    uint8_t byte1 = Device.Data[ 1 ];
    uint8_t byte2 = Device.Data[ 2 ];
    uint8_t byte3 = Device.Data[ 3 ];
    uint8_t byte4 = Device.Data[ 4 ];
    uint8_t byte5 = Device.Data[ 5 ];

    SW_Device.Presence.light = ((byte5 & 0b00000011) == 2);
    SW_Device.Presence.motion =  ((byte1 & 0b01000000) == 0b01000000);
    SW_Device.Presence.battery = ( byte2 & 0b01111111 );

    // Serial.printf( "Presence: MAC = %s, motion = %i, light = %i, battery = %i\n", Device.MAC, SW_Device.Presence.motion, SW_Device.Presence.light, SW_Device.Presence.battery );

    return true;
}

bool BLE_Device::parseContac( BLE_DEVICE& Device, SWITCHBOT& SW_Device )
{
    if (Device.DataSize != 9)
    {
        return false;
    }

    uint8_t byte1 = Device.Data[ 1 ];
    uint8_t byte2 = Device.Data[ 2 ];
    uint8_t byte3 = Device.Data[ 3 ];
    uint8_t byte4 = Device.Data[ 4 ];
    uint8_t byte5 = Device.Data[ 5 ];
    uint8_t byte6 = Device.Data[ 6 ];
    uint8_t byte7 = Device.Data[ 7 ];
    uint8_t byte8 = Device.Data[ 8 ];

    SW_Device.Contact.motion =  ((byte1 & 0b01000000) == 0b01000000);
    SW_Device.Contact.battery = ( byte2 & 0b01111111 );
    SW_Device.Contact.light = (byte3 & 0b00000001) == 0b00000001;
    SW_Device.Contact.contact = ((byte3 & 0b00000110) != 0);
    SW_Device.Contact.leftOpen = ((byte3 & 0b00000100) != 0);
    SW_Device.Contact.lastMotion = (byte4 * 256) + byte5;
    SW_Device.Contact.lastContact = (byte6 * 256) + byte7;
    SW_Device.Contact.buttonPresses = (byte8 & 0b00001111); // Increments every time button is pressed
    SW_Device.Contact.entryCount = ((byte8 >> 6) & 0b00000011); // Increments every time someone enters
    SW_Device.Contact.exitCount = ((byte8 >> 4) & 0b00000011); // Increments every time someone exits

    // Serial.printf( "Contact: MAC = %s, contact = %i, motion = %i, light = %i, left open = %i, button presses = %i, entry count = %i, exit count = %i, battery = %i\n", Device.MAC, SW_Device.Contact.contact, SW_Device.Presence.motion, SW_Device.Presence.light, SW_Device.Contact.leftOpen, SW_Device.Contact.buttonPresses, SW_Device.Contact.entryCount, SW_Device.Contact.exitCount, SW_Device.Presence.battery );
    
    return true;
}

bool BLE_Device::parseRemote( BLE_DEVICE& Device, SWITCHBOT& SW_Device )
{
    if (Device.DataSize != 4)
    {
        return false;
    }

    uint8_t byte1 = Device.Data[ 1 ];
    uint8_t byte2 = Device.Data[ 2 ];
    uint8_t byte3 = Device.Data[ 3 ];

    SW_Device.Remote.data1 = byte1;
    SW_Device.Remote.data2 = byte2;
    SW_Device.Remote.data3 = byte3;

    // Serial.printf( "Remote: MAC = %s, %02x %02x %02x\n", Device.MAC, byte1, byte2, byte3 );

    return true;
}

bool BLE_Device::parseBulb( BLE_DEVICE& Device, SWITCHBOT& SW_Device )
{
    if (Device.DataSize != 14)
    {
        return false;
    }

    uint8_t byte8 = Device.Data[ 9 ];
    uint8_t byte9 = Device.Data[ 10 ];
    uint8_t byte10 = Device.Data[ 11 ];

    SW_Device.Bulb.sequence = byte8;
    SW_Device.Bulb.on_off = ((byte9 & 0x80) == 0x80);
    SW_Device.Bulb.dim = (byte9 & 0x7F);
    SW_Device.Bulb.lightState = (byte10 & 0x03);

    // Serial.printf( "Bulb: MAC = %s, Sequence = %i, On_Off = %i, Dim = %i, LightState = %i\n", Device.MAC, SW_Device.Bulb.sequence,  SW_Device.Bulb.on_off, SW_Device.Bulb.dim, SW_Device.Bulb.lightState );

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
        //Serial.println( "Request to add URI failed: URI is not defined" );
        return false;
    }

   // Serial.printf( "Request to add: %s\n", url );

    for (uint8_t i = 0; i < NumCallbacks; i++)
    {
        if (strcmp( Callbacks[ i ].url, url ) == 0)
        {
            Callbacks[ i ].activatedTime = t;
            // Serial.println( "Request OK, URI is already registered." );
            return true;
        }
    }

    Callbacks[ NumCallbacks ].activatedTime = t;
    strcpy( Callbacks[ NumCallbacks ].url, url );
    NumCallbacks++;

    // Serial.printf( "Request OK, URI %s has been added.\n", url );

    return true;

}

bool ClientCallbacks::Find( const char* base_url, char* full_url, int bufSize )
{
    if (( base_url == nullptr ) || ( *base_url == 0 ))
    {
        return false;
    }

    for (uint8_t i = 0; i < NumCallbacks; i++)
    {
        if (strstr( Callbacks[ i ].url, base_url ) != nullptr)
        {
            strncpy( full_url, Callbacks[ i ].url, bufSize );
            return true;
        }
    }

    return false;
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

CommandQ::CommandQ()
{
    NumQd = 0;
    QEntry = 0;
    QExit = 0;
}

CommandQ::~CommandQ()
{

}

bool CommandQ::Push( String Address, String Data, String ReplyTo )
{
    if (NumQd < QSize)
    {
        NumQd++;
        
        BLE_COMMAND* entry = &Callbacks[ QEntry++ ];
        if (QEntry == QSize)
        {
            // Wrap entry index back to the beggining
            QEntry = 0;
        }

        strncpy( entry->Address, Address.c_str(), 18 );
        strncpy( entry->ReplyTo, ReplyTo.c_str(), 50 );

        uint8_t* buf = entry->Data;
        char* endPtr = (char*)Data.c_str();

        // Serial.printf( "Converting string %s to buffer: ", endPtr );

        int bytes = 0;
        do
        {
            endPtr++;
            buf[ bytes ] = strtol( endPtr, &endPtr, 10 );
            // Serial.printf( "%i, ", buf[ bytes ] );
            bytes++;
        }
        while (( endPtr != nullptr ) && ( *endPtr != 0 ) && ( bytes < 10 ));

        // Serial.printf( "bytes = %i\n", bytes );
        entry->DataLen = bytes;

        return true;
    }

    return false;
}

bool CommandQ::Pop( BLE_COMMAND* pBLE_Command )
{
    if (NumQd > 0)
    {
        NumQd--;
        
        BLE_COMMAND* pEntry = &Callbacks[ QExit++ ];
        memcpy( pBLE_Command, pEntry, sizeof( BLE_COMMAND ) );

        if (QExit == QSize)
        {
            // Wrap entry index back to the beggining
            QExit = 0;
        }

        return true;
    }

    return false;
}
