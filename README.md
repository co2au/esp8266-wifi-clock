# ESP8266 Wi-Fi Matrix Clock

Firmware for an ESP8266-based LED matrix clock with RTC backup, Wi-Fi NTP sync, web configuration, MQTT message scrolling, and OTA updates.

![LED Matrix Clock](https://ae01.alicdn.com/kf/S9f6b0f4c0b9942df9a0a3ebfb9c2f09fM.jpg)

Reference hardware: [AliExpress 1088AW MAX7219 4-in-1 LED Matrix Module](https://www.aliexpress.com/item/1005009324894617.html)

---

## ✨ Features
- 4× MAX7219 1088AW LED matrix (Icstation hardware layout).
- DS3231M RTC support (backup timekeeping in UTC).
- Wi-Fi STA mode with AP fallback if connection fails.
- NTP time sync (configurable server).
- POSIX time zone support (DST rules configurable in Web UI).
- Web configuration page served from ESP8266.
- Persistent settings stored in LittleFS (`/config.json`).
- MQTT integration: subscribe to a topic and scroll messages for 5 seconds.
- Orientation auto-flip using mercury tilt switch.
- Adjustable brightness (0–15).
- 12h/24h display mode.
- Blinkless steady colon.
- OTA updates:
  - HTTP firmware update at `/update` (with optional auth).
  - ArduinoOTA (port 8266, optional).
- Boot splash text (`MATRIX CLOCK`).

---

## 🛠 Libraries Used
Ensure you have the following libraries installed in Arduino IDE:

- `ESP8266WiFi`
- `ESP8266WebServer`
- `LittleFS`
- `time`
- `Wire`
- `SPI`
- `MD_Parola`
- `MD_MAX72XX`
- `PubSubClient`
- `ArduinoJson` (v6.x)
- `ESP8266HTTPUpdateServer`
- `ArduinoOTA`

---

## ⚙️ Default Configuration
From the sketch:

- **Wi-Fi**
  - SSID: `YOUR_WIFI`
  - Password: `YOUR_PASS`
- **NTP Host**: `pool.ntp.org`
- **Brightness**: `0`
- **Time Zone (POSIX)**: `AEST-10AEDT-11,M10.1.0/2,M4.1.0/3` (NSW/ACT default)
- **MQTT**
  - Host: *(blank by default, disabled unless set)*
  - Port: `1883`
  - Topic: `automation/matrixclock`
- **OTA**
  - HTTP OTA: enabled, path `/update`, no password by default
  - ArduinoOTA: disabled by default

---

## 🔌 Hardware Wiring

| ESP8266 Pin | Function            | Connected To              |
|-------------|---------------------|---------------------------|
| D7 (GPIO13) | MAX7219 DIN         | DIN pin                   |
| D5 (GPIO14) | MAX7219 CLK         | CLK pin                   |
| D8 (GPIO15) | MAX7219 CS          | CS pin                    |
| D1 (GPIO5)  | I²C SDA (RTC)       | DS3231M SDA               |
| D2 (GPIO4)  | I²C SCL (RTC)       | DS3231M SCL               |
| D0 (GPIO16) | Mercury tilt switch | One side of switch (other side GND) |
| GPIO2       | Onboard LED         | Active LOW (heartbeat)    |

**RTC:** Found consistently on SDA=5, SCL=4.  
**Module:** 4× 1088AW (MAX7219), Icstation hardware layout.  

---

## 🚀 Build & Upload (Arduino IDE)

1. Open the sketch in Arduino IDE.
2. In **Tools → Board**, select **Generic ESP8266 Module**.
3. Configure these settings:
   - Flash Size: **4MB (FS:1MB OTA:~1019KB)**
   - Crystal Frequency: **26 MHz**
   - Flash Mode: **DOUT**
   - CPU Frequency: **80 MHz**
4. Install the required libraries (see above).
5. Upload the code via USB (CH340 adapter).
6. On first boot, connect to Wi-Fi or access the AP fallback SSID `MatrixClock-<chipid>`.

---

## 📌 Special Notes
- RTC runs in **UTC**. The ESP8266 converts to local time using TZ rules.
- POSIX TZ strings use **negative offsets** for east-of-UTC zones (e.g., `AEST-10` for UTC+10).
- If MQTT host is left blank, MQTT stays disabled.
- Boot splash shows “MATRIX” then “CLOCK” centred.
- Colon is steady (no blink).
- Mercury switch logic (Active HIGH/LOW) is configurable in Web UI.

---

## 📜 License
This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.
