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

#ifndef ARDUINO_BLE_DEVICE_H
#define ARDUINO_BLE_DEVICE_H

#include <Arduino.h>
#include <stdint.h>

typedef struct BLE_COMMAND
{
	char Address[ 18 ];
	char ReplyTo[ 50 ];
	uint8_t Data[ 20 ];
	int8_t DataLen;
};
typedef struct BLE_DEVICE
{
	char MAC[ 18 ];
	int rssi;
	uint8_t Data[ 21 ];
	uint8_t DataSize;
	bool Changed;
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

struct SWICHBOT_BLIND
{
	uint8_t battery;
	uint8_t position;
	uint8_t version;
};

struct SWICHBOT_THERMOMETER
{
	uint8_t battery;
	float temperature;
	uint8_t humidity;
};

struct SWICHBOT_PRESENCE
{
	uint8_t battery;
	uint8_t motion;
	uint8_t light;
};

struct SWICHBOT_CONTACT
{
	uint8_t battery;
	uint8_t motion;
	uint8_t light;
	uint8_t contact;
	uint8_t leftOpen;
	uint8_t lastMotion;
	uint8_t lastContact;
	uint8_t buttonPresses;
	uint8_t entryCount;
	uint8_t exitCount;
};

struct SWICHBOT_REMOTE
{
	uint8_t data1;
	uint8_t data2;
	uint8_t data3;
};

struct SWICHBOT_BULB
{
	uint8_t sequence;
	uint8_t on_off;
	uint8_t dim;
	uint8_t lightState;
};

struct SWICHBOT_WATERLEAK
{
	uint8_t status;
	uint8_t battery;
};

struct SWICHBOT_CO2
{
	uint8_t battery;
	float temperature;
	uint8_t humidity;
  uint16_t co2;
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
		struct SWICHBOT_PRESENCE Presence;
		struct SWICHBOT_CONTACT Contact;
		struct SWICHBOT_REMOTE Remote;
		struct SWICHBOT_BULB Bulb;
		struct SWICHBOT_BLIND blind;
		struct SWICHBOT_WATERLEAK WaterLeak;
		struct SWICHBOT_CO2 MeterProCO2;
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
	bool parseBlind( BLE_DEVICE& Device, SWITCHBOT& SW_Device );
	bool parseThermometer( BLE_DEVICE& Device, SWITCHBOT& SW_Device );
	bool parsePresence( BLE_DEVICE& Device, SWITCHBOT& SW_Device );
	bool parseContac( BLE_DEVICE& Device, SWITCHBOT& SW_Device );
	bool parseRemote( BLE_DEVICE& Device, SWITCHBOT& SW_Device );
	bool parseBulb( BLE_DEVICE& Device, SWITCHBOT& SW_Device );
	bool parseIOTH( BLE_DEVICE& Device, SWITCHBOT& SW_Device );
	bool parseWaterLeak( BLE_DEVICE& Device, SWITCHBOT& SW_Device );
  bool parseMeterPro( BLE_DEVICE& Device, SWITCHBOT& SW_Device );
  bool parseMeterProCO2( BLE_DEVICE& Device, SWITCHBOT& SW_Device );

  public:
	BLE_Device();
	~BLE_Device();

	int FindDevice( const char* MAC );
	bool AddDevice( const char* MAC, int rssi, uint8_t* BLEData,
					uint8_t BLEDataSize, uint8_t* ManufactureData,
					uint8_t ManufactureDataSize );
	void UpdateDevice( uint8_t Index, int rssi, uint8_t* BLEData,
					   uint8_t BLEDataSize, uint8_t* ManufactureData,
					   uint8_t ManufactureDataSize );
	bool CompareDevice( uint8_t Index, int rssi, uint8_t* BLEData,
						uint8_t BLEDataSize, uint8_t* ManufactureData,
						uint8_t ManufactureDataSize );
	bool GetSWDevice( uint8_t Index, SWITCHBOT& Device );
	int DeviceToJson( uint8_t Index, char* Buf, int BufSize, char* macAddress );
	int AllToJson( char* Buf, int BufSize, bool OnlyChanged, char* macAddress );
	void ClearChanged();
	bool HasChanged();
	int GetNumberOfDevices()
	{
		return NumDevices;
	};
};

typedef struct CALL_BACK
{
	char url[ 255 ];
	unsigned long activatedTime;
	int refusals;
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
	bool Find( const char* url, char* full_url, int bufSize );
	bool Remove( uint8_t Index );
	bool Remove( const char* url );
	bool Get( uint8_t Index, char* buf, int BufLength );
	void Check( unsigned long t );	  // Call this periodically to remove expired callbacks
	void addRefusal( uint8_t Index );
	void resetRefusal( uint8_t Index );
  bool HasCallbacks();
};

class CommandQ
{
  private:
#define QSize 20
	BLE_COMMAND Callbacks[ QSize ];
	int NumQd;
	int QEntry;
	int QExit;

  public:
	CommandQ();
	~CommandQ();

	bool Push( String Address, String Data, String ReplyTo );
	bool Pop( BLE_COMMAND* pBLE_Command );
};

#endif
