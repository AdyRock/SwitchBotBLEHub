#ifndef ARDUINO_BLE_DEVICE_H
#define ARDUINO_BLE_DEVICE_H

#include <Arduino.h>
#include <stdint.h>

typedef struct BLE_DEVICE
{
    char MAC[ 18 ];
    int rssi;
    uint8_t Data[ 10 ];
    uint8_t DataSize;
    bool	Changed;
};

struct SWICHBOT_BOT
{
    bool mode;
    bool state;
    uint8_t battery;
};

struct SWICHBOT_CUTAIN
{
    bool calibration;
    uint8_t battery;
    uint8_t position;
    uint8_t lightLevel;
    bool moving;
};

struct SWICHBOT_THERMOMETER
{
    uint8_t battery;
    float temperature;
    uint8_t humidity;
};

typedef struct SWITCHBOT
{
    char MAC[ 18 ];
    int rssi;
    char model;
    union
    {
        struct SWICHBOT_BOT bot;
        struct SWICHBOT_CUTAIN curtain;
        struct SWICHBOT_THERMOMETER thermometer;
    };
};

class BLE_Device
{
private:
    BLE_DEVICE BLE_devices[ 50 ];
    uint8_t NumDevices;
    bool Changed;
    bool parseDevice( BLE_DEVICE& Device, SWITCHBOT& SW_Device );
    bool parseBot( BLE_DEVICE& Device, SWITCHBOT& SW_Device );
    bool parseCurtain( BLE_DEVICE& Device, SWITCHBOT& SW_Device );
    bool parseThermometer( BLE_DEVICE& Device, SWITCHBOT& SW_Device );

public:

    BLE_Device();
    ~BLE_Device();

    int FindDevice( const char* MAC );
    bool AddDevice( const char* MAC, int rssi, uint8_t* BLEData, uint8_t BLEDataSize );
    void UpdateDevice( uint8_t Index, int rssi, uint8_t* BLEData, uint8_t BLEDataSize );
    bool CompareDevice( uint8_t Index, int rssi, uint8_t* BLEData, uint8_t BLEDataSize );
    bool GetSWDevice( uint8_t Index, SWITCHBOT& Device );
    int DeviceToJson( uint8_t Index, char* Buf, int BufSize );
    int AllToJson( char* Buf, int BufSize, bool OnlyChanged );
    void ClearChanged();
    bool HasChanged();
};

typedef struct CALL_BACK
{
    char url[ 255 ];
    unsigned long activatedTime;
};

class ClientCallbacks
{
private:
    CALL_BACK Callbacks[ 5 ];
    int NumCallbacks;

public:
    ClientCallbacks();
    ~ClientCallbacks();

    bool Add( const char* url, unsigned long t );
    bool Remove( uint8_t Index );
    bool Remove( const char* url );
    bool Get( uint8_t Index, char* buf, int BufLength );
    void Check( unsigned long t );   // Call this periodically to remove expired callbacks
    bool HasCallbacks();
};
#endif