# ESP8266 Clock

An ESP8266 NTP clock with OLED display.
Based on hardware from Aliexpress:
https://www.aliexpress.com/item/1005009324894617.html

## Features
- NTP sync with TZ and DST for Australia
- Small OLED splash “Matrix Clock”
- Config via `include/secrets.h`
- OTA Firmware uptating via HTTP/Arduino

## Build
### Arduino IDE
- Board: ESP8266 (e.g. NodeMCU 1.0)
- Install ESP8266 core, select correct COM/USB
- Copy `include/secrets.example.h` to `include/secrets.h` and set your values
- Compile and upload

### PlatformIO
- `platformio run -e nodemcuv2`
- `platformio run -e nodemcuv2 -t upload`

## File system (optional)
If using SPIFFS or LittleFS, put assets in `data/`, then:
- `pio run -t uploadfs` (PlatformIO) or Arduino plugin for FS upload.

## Licensing
MIT unless you prefer another licence.

