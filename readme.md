# SwitchBot BLE / Network Hub

This code is written for ESP32 Arduino compatible boards and provides a network interface to the SwitchBot BLE devices, e.g. it is a hub.

It is primarily targetted to work with the **Athom Homey** and the **SwitchBot** app, but the interface is based on REST so could be used with any network controller that has code adapted for it.

When used with the Homey SwitchBot app (available in the Athom Homey store), the discovery is completely automatic via UDP Multicast messages.
the Homey app can supptort multiple hubs on a LAN so they can be place within range of the BLE device.
If two hubs are within range of one BLE device then Homey choses the on that has the best signal strength to send commands.

When adding devices to Homey, the hub provides a list of BLE devices that it has detected so that you can add devices that are not normally within range of Homey.

The hub constantly scans the BLE frequencies to provide continuous status updates of the BLE devicess and posts any changes back to Homey.

The 2 boards that I have tested are from Amazon:

* MakerHawk ESP32 Module OLED Display WiFi Development Board WIFI Kit 32 Low Power Consumption 240 MHZ Dual Core with CP2012 Chip 0.96inch Display for Arduino Nodemcu
by seamuing
Learn more: <https://www.amazon.co.uk/dp/B076P8GRWV/ref=cm_sw_em_r_mt_dp_EHPV76ACPSF8JHC9J1VA?_encoding=UTF8&psc=1>

* 3 pack ESP32 ESP-32S Development Board 2.4 GHz Dual Core WLAN WiFi + Bluetooth 2-In-1 Microcontroller ESP-WROOM-32 Chip CP2102 for Arduino
by AiTrip Eu
Learn more: <https://www.amazon.co.uk/dp/B08DR5T897/ref=cm_sw_em_r_mt_dp_1JMZDAYK05AJWEYVM1HS?_encoding=UTF8&psc=1>

## Installation Method 1 via the Arduino IDE

1. Download the code from this repository and unzip it to a local folder on your computer.
2. In the Arduino IDE you need to set the following links in Fie - Preferences - Additional Boards Maneger URLs:

    ```html
    https://dl.espressif.com/dl/package_esp32_index.json
    http://arduino.esp8266.com/stable/package_esp8266com_index.json
    https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
    https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_dev_index.json

    ```

3. In the Tools - Boards manager select the esp32 by Espressif Systems v 1.0.4
4. Then select the board type as ESP32 Arduino - ESP32 Dev Module.
5. Next, change the Tools - Partion Scheme to Minimal SPIFFS (1.9MB App with OTA/190KB SPIFFS ). This allows the programs size to fit on the board plus leave room for the bootloader.
6. You will need to install the following libraries into your system from the Sketch - Include Library menu:

    ```html
    ArduinoJson by Benoit Blanchon version 0.1.2
    NimBLE-Arduino by h2zero version 1.2.0
    ElegantOTA version 3
    ```

7. Install the follwing packages from the internet:

   ```html
    AsyncTCP from https://github.com/me-no-dev/AsyncTCP
    ESPAsyncWebServer from https://github.com/me-no-dev/ESPAsyncWebServer
    ESPAsyncWiFiManager from https://github.com/alanswx/ESPAsyncWiFiManager
   ```

8. Edit the file ElegantOTA.h located in your Arduino library folder (...\Arduino\libraries\ElegantOTA\src) and change the line '#define ELEGANTOTA_USE_ASYNC_WEBSERVER 0' to '#define ELEGANTOTA_USE_ASYNC_WEBSERVER 1'

9. Connect the board to you computer via USB and select the Port on the Tools menu.

10. Build and download the code to the board.


## Installation Method 2 via the Flash downloader

1. Download the SwitchBotBLEHub.ino.merged.bin file from the build/esp32.esp32.esp32 folder

2. Download the Flash Downloads Tool from https://www.espressif.com/en/support/download/other-tools

3. Connect the board to  you computer via USB

4. Unpack the zip file and run the flash_download_tool_3.9.7.exe

5. Add the SwitchBotBLEHub.ino.merged.bin to the first line of the top section

6. Set the value of the right hand field to 0x0

7. Make sure the check box to the left of the filename in ticked.

8. Set the SPI SPEED to 80MHz

9. Set the SPI MODE to QIO

10. Select the COM port of the ESP32 device

11. Set the BAUD to 921600

12. Click on start

13. When the status shows FINISH cycle the power on the ESP32.

## Connecting the ESP32 to your WiFi

1. If the device hasn't ben connected to your WiFi already then connect to the SwitchBot_ESP32 Access Point.
   Note: it might take a minute for the ESP bootloader to start up for the first time and appear as a WiFi hotspot.

2. If you don't get a join network pop-up then use a browser to open any web page (it should redirect) or connect to 192.168.4.1.

3. Choose one of the detected access points and enter the password, then click save.

4. The hub will try to connect to your WiFi and then start working. If the connection fails, reconnect to the AP and reconfigure.

Once the board is up and running, take it to a location that is withing range of the SwitchBot device(s) and connect it to a USB PSU. It should detect the BLE devices within a few seconds.

## Updating the firmware OTA

The board also supports Over The Air updates so future updates can be sent directly to the board. 

Download the SwitchBotBLEHub.ino.bin file from https://github.com/AdyRock/SwitchBotBLEHub/tree/master/build/esp32.esp32.esp32.

Open a browser on a computer connected to the same network as the board. Navigate to http://board_ip_address/update

Select the Firmware option and then choose the SwitchBotBLEHub.ino.bin file you downloaded.
The file download should start straight away.
