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

## Installation

1. Download the code from this repository and unzip it to a local folder on your computer.
2. In the Arduino IDE you need to set the following links in Fie - Preferences - Additional Boards Maneger URLs:

    ```html
    https://resource.heltec.cn/download/package_heltec_esp32_index.json
    https://dl.espressif.com/dl/package_esp32_index.json
    http://arduino.esp8266.com/stable/package_esp8266com_index.json
    https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
    https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_dev_index.json
    ```

3. In the Tools - Boards manager select the esp32 by Espressif Systems v 1.0.4
4. Then select the board type as ESP32 Arduino - Heltek Wifi Kit 32.
5. Next, change the To0ls - Partion Scheme to Huge App. This allows the programs size to fit on the board.
6. You will need to install the following libraries into your system from the Sketch - Include Library menu:

    ```html
    ArduinoJson by Benoit Blanchon version 0.1.2
    NimBLE-Arduino by h2zero version 1.2.0
    Heltec ESP32 Dev-Boards by Heltec Automation version 1.1.0
    ```

7. Install the follwing packages from the internet:

   ```html
    AsyncTCP from https://github.com/me-no-dev/AsyncTCP
    ESPAsyncWebServer from https://github.com/me-no-dev/ESPAsyncWebServer
   ```

8. Finally create file called secrets.h and add lines to define your WiFi SSID and password:

   ```c
    #define WIFI_SSID "Your WiFi SSID here"
    #define WIFI_PASS "Your WiFi Password here"
   ```

9. Connect the board to you computer via USB and select the Port on the Tools menu.

10. Build and download the code to the board.

On the Heltec board that has a small display, you should see the first detected curtain position and the first detect temperature devices temperature and humidity. These can obviously be changed in the code.

Once the board is up and running, take it to a location that is withing range of the SwitchBot device(s) and connect it to a USB PSU. It should detect the BLE devices within a few seconds.
