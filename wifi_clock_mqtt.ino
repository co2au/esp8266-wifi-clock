// ESP8266 Matrix Clock (4x 1088AW/MAX7219, Icstation layout) + DS3231M (RTC in UTC)
// Web UI + LittleFS config + MQTT + STA→AP fallback + OTA (HTTP / ArduinoOTA)
// Blinkless clock, steady colon, centred after MQTT scroll, tilt default ACTIVE HIGH
// POSIX TZ (configurable; NSW/ACT default). Heap-safe time handling (no setenv/tzset in loop).
// TZ “working?” detection uses strftime("%z") (reliable on ESP8266); NSW fallback retained.

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <time.h>
#include <Wire.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPUpdateServer.h>  // HTTP OTA (/update)
#include <ArduinoOTA.h>               // ArduinoOTA (port 8266)

// -------------------- Hardware pins & layout --------------------
#define PIN_DIN     13   // D7 to MAX7219 DIN
#define PIN_CLK     14   // D5 to MAX7219 CLK
#define PIN_CS      15   // D8 to MAX7219 CS
#define HW_TYPE     MD_MAX72XX::ICSTATION_HW
#define MODULES     4

// Candidate I2C pairs to scan (SDA,SCL)
struct Pair { int sda, scl; };
Pair I2C_CANDIDATES[] = { {4,5}, {5,4}, {12,14}, {14,12}, {13,14}, {14,13} };

#define PIN_MERCURY 16    // D0 tilt switch, INPUT_PULLUP
const unsigned long ORIENT_DEBOUNCE_MS = 500;

#ifndef LED_BUILTIN
#define LED_BUILTIN 2     // most ESP-12 boards use GPIO2
#endif
const bool LED_ACTIVE_LOW = true; // onboard LED is usually active-LOW (0=on)

const uint8_t RTC_ADDR = 0x68;
const unsigned long RESYNC_INTERVAL_MS = 3600UL * 1000UL; // hourly

// -------------------- Defaults / Config --------------------
// RTC holds UTC. Display shows local time using cfg.tzString if autoDST==true.
struct Settings {
  // Wi-Fi
  String  wifiSsid       = "YOUR_WIFI";
  String  wifiPass       = "YOUR_PASS";

  // Time
  String  ntpHost        = "pool.ntp.org";
  bool    use24h         = true;
  uint8_t brightness     = 0;               // 0..15

  // DST / TZ
  bool    autoDST        = true;            // use tzString when true
  String  tzString       = "AEST-10AEDT-11,M10.1.0/2,M4.1.0/3"; // NSW/ACT default
  long    tzOffset       = 10 * 3600;       // used only if autoDST=false
  bool    dstSimple      = false;           // +3600 if autoDST=false

  // Tilt
  bool    tiltActiveLow  = false;           // default ACTIVE HIGH

  // LED heartbeat
  bool    ledHeartbeat   = true;

  // MQTT
  String  mqttHost       = "";
  uint16_t mqttPort      = 1883;
  String  mqttTopic      = "automation/matrixclock";

  // OTA
  bool    otaHttpEnabled = true;            // /update page
  String  otaHttpUser    = "admin";
  String  otaHttpPass    = "";              // empty = no auth
  bool    otaArduinoEnabled = false;        // ArduinoOTA service
  String  otaArduinoPassword = "";          // empty = no password
} cfg;

MD_Parola P(HW_TYPE, PIN_DIN, PIN_CLK, PIN_CS, MODULES);
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// I2C/RTC
int I2C_SDA = -1, I2C_SCL = -1;
bool rtcPresent = false;

// Tilt/orientation
bool inverted = false;
bool lastRawMerc = false;
unsigned long lastMercMs = 0;

// Time keeping
unsigned long lastResync = 0;
unsigned long lastClockTick = 0;
time_t bootEpoch = 0;           // UTC epoch when seeded (if no RTC)
unsigned long bootMillis = 0;

enum DisplayMode { MODE_CLOCK, MODE_SCROLL };
DisplayMode mode = MODE_CLOCK;
String scrollMessage;
unsigned long scrollUntilMs = 0;

// --- Compact 3x5 font (upper-case + digits + space) ---
// Format compatible with MD_Parola/MD_MAX72XX setFont() tables.
// Each character: [width][col bytes...]; 0x00 as spacer included in glyphs minimally.
// Height is 7px in driver but we only use top 5 bits.
// Index map: ' ' (32) .. 'Z' (90), '0'..'9'
const uint8_t PROGMEM kFont3x5[] = {
  0, 0, // 0..31 not used
  // 32 ' ' (space)
  1, 0x00,

  // 33..47 minimal punctuation if needed (set to 1,0x00)
  1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00,
  1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00, 1, 0x00,

  // 48 '0' .. 57 '9' (3x5)
  3, 0x1E,0x11,0x1E,  // 0
  3, 0x00,0x12,0x1F,  // 1
  3, 0x19,0x15,0x12,  // 2
  3, 0x11,0x15,0x1F,  // 3
  3, 0x07,0x04,0x1F,  // 4
  3, 0x17,0x15,0x0D,  // 5
  3, 0x1F,0x15,0x1D,  // 6
  3, 0x01,0x01,0x1F,  // 7
  3, 0x1F,0x15,0x1F,  // 8
  3, 0x17,0x15,0x1F,  // 9

  // 58..64 unused
  1,0x00, 1,0x00, 1,0x00, 1,0x00, 1,0x00, 1,0x00, 1,0x00,

  // 65 'A' .. 90 'Z' (3x5)
  3, 0x1E,0x05,0x1E,  // A
  3, 0x1F,0x15,0x0A,  // B
  3, 0x0E,0x11,0x11,  // C
  3, 0x1F,0x11,0x0E,  // D
  3, 0x1F,0x15,0x11,  // E
  3, 0x1F,0x05,0x01,  // F
  3, 0x0E,0x11,0x1D,  // G
  3, 0x1F,0x04,0x1F,  // H
  3, 0x11,0x1F,0x11,  // I
  3, 0x08,0x10,0x0F,  // J
  3, 0x1F,0x04,0x1B,  // K
  3, 0x1F,0x10,0x10,  // L
  3, 0x1F,0x02,0x1F,  // M
  3, 0x1F,0x02,0x1F,  // N (same narrow style)
  3, 0x0E,0x11,0x0E,  // O
  3, 0x1F,0x05,0x02,  // P
  3, 0x0E,0x11,0x1E,  // Q
  3, 0x1F,0x05,0x1A,  // R
  3, 0x12,0x15,0x09,  // S
  3, 0x01,0x1F,0x01,  // T
  3, 0x0F,0x10,0x0F,  // U
  3, 0x07,0x18,0x07,  // V
  3, 0x1F,0x08,0x1F,  // W
  3, 0x1B,0x04,0x1B,  // X
  3, 0x03,0x1C,0x03,  // Y
  3, 0x19,0x15,0x13,  // Z
};


// --------- BCD helpers (RTC) ----------
static uint8_t bcd2dec(uint8_t v){ return ((v >> 4) * 10) + (v & 0x0F); }
static uint8_t dec2bcd(uint8_t v){ return ((v/10)<<4) | (v%10); }

// --------- TZ helpers (robust on ESP8266) ----------
static long parse_tz_offset(const char *zz){
  if (!zz || (zz[0]!='+' && zz[0]!='-')) return 0;
  int sign = (zz[0]=='-') ? -1 : +1;
  if (!isdigit(zz[1]) || !isdigit(zz[2]) || !isdigit(zz[3]) || !isdigit(zz[4])) return 0;
  int hh = (zz[1]-'0')*10 + (zz[2]-'0');
  int mm = (zz[3]-'0')*10 + (zz[4]-'0');
  return sign * (hh*3600L + mm*60L);
}

static bool tzAppearsWorking(){
  // Probe a fixed UTC instant
  struct tm tUTC{}; tUTC.tm_year = 2025-1900; tUTC.tm_mon = 5; tUTC.tm_mday = 15; // 2025-06-15
  tUTC.tm_hour = 12; tUTC.tm_min = 0; tUTC.tm_sec = 0;
  // Convert broken-down UTC -> epoch
  auto epochFromUtcTm = [](const struct tm& t){
    int Y=t.tm_year+1900, M=t.tm_mon+1, D=t.tm_mday;
    if (M<=2){ Y--; M+=12; }
    long eraDays = 365L*(Y-1970) + ((Y-1969)/4);
    long monthDays = (153L*(M-3)+2)/5;
    long days = eraDays + monthDays + (D-1);
    return (time_t)(days*86400L + t.tm_hour*3600L + t.tm_min*60L + t.tm_sec);
  };
  time_t probeUTC = epochFromUtcTm(tUTC);
  struct tm lt{}; localtime_r(&probeUTC, &lt);
  char zbuf[8]={0}; strftime(zbuf, sizeof(zbuf), "%z", &lt);
  long off = parse_tz_offset(zbuf);
  return (off >= -14*3600L && off <= 14*3600L && off != 0);
}

// --------- RTC read/write (UTC) ----------
bool rtcRead(struct tm &tUTC) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(RTC_ADDR, (uint8_t)7) != 7) return false;

  int ss = bcd2dec(Wire.read() & 0x7F);      // mask CH bit
  int mi = bcd2dec(Wire.read() & 0x7F);
  uint8_t hr = Wire.read();
  int hh;
  if (hr & 0x40) { hh = bcd2dec(hr & 0x1F); if (hr & 0x20) hh = (hh % 12) + 12; }
  else { hh = bcd2dec(hr & 0x3F); }
  Wire.read(); // DOW
  int dd = bcd2dec(Wire.read() & 0x3F);
  int mm = bcd2dec(Wire.read() & 0x1F);
  int yy = 2000 + bcd2dec(Wire.read());

  tUTC = {};
  tUTC.tm_year = yy - 1900;
  tUTC.tm_mon  = mm - 1;
  tUTC.tm_mday = dd;
  tUTC.tm_hour = hh;
  tUTC.tm_min  = mi;
  tUTC.tm_sec  = ss;
  return true;
}

bool rtcWrite(const struct tm &tUTC) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x00);
  Wire.write(dec2bcd(tUTC.tm_sec) & 0x7F);   // ensure CH=0
  Wire.write(dec2bcd(tUTC.tm_min));
  Wire.write(dec2bcd(tUTC.tm_hour));         // 24h UTC
  Wire.write(dec2bcd(0));                    // DOW optional
  Wire.write(dec2bcd(tUTC.tm_mday));
  Wire.write(dec2bcd(tUTC.tm_mon + 1));
  Wire.write(dec2bcd((tUTC.tm_year + 1900) % 100));
  return Wire.endTransmission() == 0;
}

// --------- TZ handling (set once; NOT in loop) ----------
void applyTZ() {
  if (cfg.autoDST) {
    setenv("TZ", cfg.tzString.c_str(), 1);  // POSIX TZ from settings
  } else {
    unsetenv("TZ");                          // fixed-offset path uses configTime
  }
  tzset();
}

// Convert a UTC broken-down time to Unix epoch without touching TZ/env (1970..2099)
time_t epochFromUtcTm(const struct tm& t) {
  int y = t.tm_year + 1900;
  int m = t.tm_mon + 1;
  int d = t.tm_mday;
  if (m <= 2) { y -= 1; m += 12; }
  long eraDays = 365L * (y - 1970) + ((y - 1969) / 4);     // leap years since 1970
  long monthDays = (153L * (m - 3) + 2) / 5;
  long days = eraDays + monthDays + (long)(d - 1);
  return (time_t)(days * 86400L + t.tm_hour * 3600L + t.tm_min * 60L + t.tm_sec);
}

// --------- NTP sync -> write UTC to RTC or seed SW clock (UTC) ----------
bool syncFromNTP() {
  applyTZ(); // safe; called rarely

  if (cfg.autoDST) {
    configTime(0, 0, cfg.ntpHost.c_str()); // TZ env handles local; time() is UTC
  } else {
    long tz = cfg.tzOffset + (cfg.dstSimple ? 3600 : 0);
    configTime(tz, 0, cfg.ntpHost.c_str());
  }

  time_t now = 0;
  for (int i = 0; i < 40 && now < 8*3600; i++) { delay(250); now = time(nullptr); }
  if (now < 8*3600) return false;

  struct tm tUTC; gmtime_r(&now, &tUTC);
  if (rtcPresent) {
    if (rtcWrite(tUTC)) Serial.println(F("RTC set to UTC from NTP"));
    else               Serial.println(F("RTC write failed (UTC) after NTP"));
  } else {
    bootEpoch  = now;         // keep software clock in UTC
    bootMillis = millis();
    Serial.println(F("Seeded software clock (UTC) from NTP"));
  }
  return true;
}

// --------- Orientation (mercury switch) ----------
void applyFlip(bool on) {
  P.setZoneEffect(0, on, PA_FLIP_UD);
  P.setZoneEffect(0, on, PA_FLIP_LR);
}

void updateOrientation() {
  bool raw = (digitalRead(PIN_MERCURY) == (cfg.tiltActiveLow ? LOW : HIGH));
  unsigned long now = millis();
  if (raw != lastRawMerc) { lastRawMerc = raw; lastMercMs = now; }
  if ((now - lastMercMs) >= ORIENT_DEBOUNCE_MS) {
    if (raw != inverted) {
      inverted = raw;
      applyFlip(inverted);
      Serial.printf("Orientation: %s\n", inverted ? "inverted" : "upright");
    }
  }
}

// --------- Formatting ----------
String formatTime(int hh, int mi) {
  if (!cfg.use24h) {
    int h12 = hh % 12; if (h12 == 0) h12 = 12;
    char buf[6]; snprintf(buf, sizeof(buf), "%2d:%02d", h12, mi);
    if (buf[0] == ' ') buf[0] = ' ';
    return String(buf);
  } else {
    char buf[6]; snprintf(buf, sizeof(buf), "%02d:%02d", hh, mi);
    return String(buf);
  }
}

// --------- Config persistence ----------
const char* CONFIG_PATH = "/config.json";

bool saveConfig() {
  StaticJsonDocument<900> d;
  d["wifiSsid"]      = cfg.wifiSsid;
  d["wifiPass"]      = cfg.wifiPass;
  d["ntpHost"]       = cfg.ntpHost;
  d["use24h"]        = cfg.use24h;
  d["brightness"]    = cfg.brightness;

  d["autoDST"]       = cfg.autoDST;
  d["tzString"]      = cfg.tzString;
  d["tzOffset"]      = cfg.tzOffset;
  d["dstSimple"]     = cfg.dstSimple;

  d["tiltActiveLow"] = cfg.tiltActiveLow;
  d["ledHeartbeat"]  = cfg.ledHeartbeat;

  d["mqttHost"]      = cfg.mqttHost;
  d["mqttPort"]      = cfg.mqttPort;
  d["mqttTopic"]     = cfg.mqttTopic;

  d["otaHttpEnabled"]= cfg.otaHttpEnabled;
  d["otaHttpUser"]   = cfg.otaHttpUser;
  d["otaHttpPass"]   = cfg.otaHttpPass;
  d["otaArduinoEnabled"] = cfg.otaArduinoEnabled;
  d["otaArduinoPassword"]= cfg.otaArduinoPassword;

  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) return false;
  bool ok = (serializeJson(d, f) > 0);
  f.close();
  Serial.println(ok ? F("Config saved") : F("Config save failed"));
  return ok;
}

bool loadConfig() {
  if (!LittleFS.exists(CONFIG_PATH)) return false;
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) return false;
  StaticJsonDocument<900> d;
  auto e = deserializeJson(d, f);
  f.close();
  if (e) return false;

  cfg.wifiSsid      = d["wifiSsid"]      | cfg.wifiSsid;
  cfg.wifiPass      = d["wifiPass"]      | cfg.wifiPass;
  cfg.ntpHost       = d["ntpHost"]       | cfg.ntpHost;
  cfg.use24h        = d["use24h"]        | cfg.use24h;
  cfg.brightness    = d["brightness"]    | cfg.brightness;

  cfg.autoDST       = d["autoDST"]       | cfg.autoDST;
  cfg.tzString      = d["tzString"]      | cfg.tzString;
  cfg.tzOffset      = d["tzOffset"]      | cfg.tzOffset;
  cfg.dstSimple     = d["dstSimple"]     | cfg.dstSimple;

  cfg.tiltActiveLow = d["tiltActiveLow"] | cfg.tiltActiveLow;
  cfg.ledHeartbeat  = d["ledHeartbeat"]  | cfg.ledHeartbeat;

  cfg.mqttHost      = d["mqttHost"]      | cfg.mqttHost;
  cfg.mqttPort      = d["mqttPort"]      | cfg.mqttPort;
  cfg.mqttTopic     = d["mqttTopic"]     | cfg.mqttTopic;

  cfg.otaHttpEnabled= d["otaHttpEnabled"]| cfg.otaHttpEnabled;
  cfg.otaHttpUser   = d["otaHttpUser"]   | cfg.otaHttpUser;
  cfg.otaHttpPass   = d["otaHttpPass"]   | cfg.otaHttpPass;
  cfg.otaArduinoEnabled = d["otaArduinoEnabled"] | cfg.otaArduinoEnabled;
  cfg.otaArduinoPassword= d["otaArduinoPassword"]| cfg.otaArduinoPassword;

  Serial.println(F("Config loaded"));
  return true;
}

// --------- Web UI ----------
const char HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<title>Matrix Clock — Settings</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
 body{font-family:sans-serif;max-width:760px;margin:24px auto;padding:0 12px}
 h1{font-size:20px} fieldset{margin:12px 0;padding:12px}
 label{display:block;margin:8px 0 4px} input,select{width:100%;padding:8px}
 .row{display:flex;gap:12px;flex-wrap:wrap} .row>div{flex:1;min-width:220px}
 button{padding:10px 16px;margin-top:12px}
 .note{font-size:12px;color:#555}
 code{background:#f3f3f3;padding:2px 4px;border-radius:4px}
</style></head><body>
<h1>Matrix Clock Settings</h1>
<form method="POST" action="/save">
<fieldset><legend>Wi-Fi</legend>
<label>SSID <input name="wifiSsid" value="%WIFI_SSID%"></label>
<label>Password <input name="wifiPass" type="password" value="%WIFI_PASS%"></label>
</fieldset>

<fieldset><legend>Time &amp; DST</legend>
<label>NTP Host <input name="ntpHost" value="%NTP_HOST%"></label>
<div class="row">
  <div><label>24-hour format
    <select name="use24h"><option value="1" %USE24H_ON%>Yes</option><option value="0" %USE24H_OFF%>No</option></select>
  </label></div>
  <div><label>Brightness (0–15)
    <input name="brightness" type="number" min="0" max="15" value="%BRIGHT%">
  </label></div>
</div>
<label>Automatic DST (uses POSIX TZ string)
  <select name="autoDST"><option value="1" %ADST_ON%>Enabled</option><option value="0" %ADST_OFF%>Disabled</option></select>
</label>
<label>Time zone (POSIX TZ) <input name="tzString" value="%TZSTR%"></label>
<p class="note">Examples: NSW/ACT/VIC/TAS <code>AEST-10AEDT-11,M10.1.0/2,M4.1.0/3</code>, QLD <code>AEST-10</code>, SA <code>ACST-9:30ACDT-10:30,M10.1.0/2,M4.1.0/3</code>, NT <code>ACST-9:30</code>, WA <code>AWST-8</code>.</p>
<div class="row">
  <div><label>Fixed time-zone offset (seconds) <input name="tzOffset" type="number" value="%TZO%"></label></div>
  <div><label>Add simple DST (+3600)
    <select name="dstSimple"><option value="1" %DST_ON%>Yes</option><option value="0" %DST_OFF%>No</option></select>
  </label></div>
</div>
</fieldset>

<fieldset><legend>Tilt &amp; LED</legend>
<div class="row">
  <div><label>Mercury switch logic
    <select name="tiltActiveLow"><option value="1" %TILT_LOW%>Active LOW = inverted</option><option value="0" %TILT_HIGH%>Active HIGH = inverted</option></select>
  </label></div>
  <div><label>Blink board LED each second
    <select name="ledHeartbeat"><option value="1" %LED_ON%>Enabled</option><option value="0" %LED_OFF%>Disabled</option></select>
  </label></div>
</div>
</fieldset>

<fieldset><legend>MQTT</legend>
<div class="row">
  <div><label>Broker host <input name="mqttHost" value="%MQTT_HOST%"></label></div>
  <div><label>Port <input name="mqttPort" type="number" value="%MQTT_PORT%"></label></div>
  <div><label>Topic <input name="mqttTopic" value="%MQTT_TOPIC%"></label></div>
</div>
<p class="note">Messages on this topic scroll for 5 seconds, then the clock returns.</p>
</fieldset>

<fieldset><legend>OTA</legend>
<div class="row">
  <div><label>HTTP Update at <code>/update</code>
    <select name="otaHttpEnabled"><option value="1" %HOTA_ON%>Enabled</option><option value="0" %HOTA_OFF%>Disabled</option></select>
  </label></div>
  <div><label>HTTP User <input name="otaHttpUser" value="%HOTA_USER%"></label></div>
  <div><label>HTTP Password <input name="otaHttpPass" type="password" value="%HOTA_PASS%"></label></div>
</div>
<div class="row">
  <div><label>ArduinoOTA (port 8266)
    <select name="otaArduinoEnabled"><option value="1" %AOTA_ON%>Enabled</option><option value="0" %AOTA_OFF%>Disabled</option></select>
  </label></div>
  <div><label>ArduinoOTA Password <input name="otaArduinoPassword" type="password" value="%AOTA_PASS%"></label></div>
</div>
<p class="note">If HTTP password is left blank, the /update page will be unsecured on your LAN.</p>
</fieldset>

<button type="submit">Save &amp; Reboot</button>
</form>
<p class="note">IP: %IP% • Mode: %MODE%</p>
</body></html>
)HTML";

String expandTemplate(const char* tpl, const String& modeStr) {
  String s(tpl);
  s.replace("%WIFI_SSID%",  cfg.wifiSsid);
  s.replace("%WIFI_PASS%",  cfg.wifiPass);
  s.replace("%NTP_HOST%",   cfg.ntpHost);
  s.replace("%USE24H_ON%",  cfg.use24h ? "selected" : "");
  s.replace("%USE24H_OFF%", cfg.use24h ? "" : "selected");
  s.replace("%BRIGHT%",     String(cfg.brightness));

  s.replace("%ADST_ON%",    cfg.autoDST ? "selected" : "");
  s.replace("%ADST_OFF%",   cfg.autoDST ? "" : "selected");
  s.replace("%TZSTR%",      cfg.tzString);
  s.replace("%TZO%",        String(cfg.tzOffset));
  s.replace("%DST_ON%",     cfg.dstSimple ? "selected" : "");
  s.replace("%DST_OFF%",    cfg.dstSimple ? "" : "selected");

  s.replace("%TILT_LOW%",   cfg.tiltActiveLow ? "selected" : "");
  s.replace("%TILT_HIGH%",  cfg.tiltActiveLow ? "" : "selected");
  s.replace("%LED_ON%",     cfg.ledHeartbeat ? "selected" : "");
  s.replace("%LED_OFF%",    cfg.ledHeartbeat ? "" : "selected");

  s.replace("%MQTT_HOST%",  cfg.mqttHost);
  s.replace("%MQTT_PORT%",  String(cfg.mqttPort));
  s.replace("%MQTT_TOPIC%", cfg.mqttTopic);

  s.replace("%HOTA_ON%",    cfg.otaHttpEnabled ? "selected" : "");
  s.replace("%HOTA_OFF%",   cfg.otaHttpEnabled ? "" : "selected");
  s.replace("%HOTA_USER%",  cfg.otaHttpUser);
  s.replace("%HOTA_PASS%",  cfg.otaHttpPass);
  s.replace("%AOTA_ON%",    cfg.otaArduinoEnabled ? "selected" : "");
  s.replace("%AOTA_OFF%",   cfg.otaArduinoEnabled ? "" : "selected");
  s.replace("%AOTA_PASS%",  cfg.otaArduinoPassword);

  s.replace("%IP%",         WiFi.localIP().toString());
  s.replace("%MODE%",       modeStr);
  return s;
}

void handleRoot() {
  String modeStr = (WiFi.getMode() & WIFI_AP) ? "AP" : "STA";
  String html = expandTemplate(HTML, modeStr);
  server.send(200, "text/html; charset=utf-8", html);
}

long toLongOr(const String& v, long def) { if (v.length()==0) return def; return v.toInt(); }
uint16_t toU16Or(const String& v, uint16_t def){ if (v.length()==0) return def; long t=v.toInt(); if(t<0)t=0; if(t>65535)t=65535; return (uint16_t)t; }
uint8_t clampByte(long v, uint8_t lo, uint8_t hi){ if (v<lo) v=lo; if (v>hi) v=hi; return (uint8_t)v; }

void handleSave() {
  if (server.method() != HTTP_POST) { server.send(405, "text/plain", "Use POST"); return; }
  cfg.wifiSsid      = server.arg("wifiSsid");
  cfg.wifiPass      = server.arg("wifiPass");
  cfg.ntpHost       = server.arg("ntpHost");
  cfg.use24h        = (server.arg("use24h") == "1");
  cfg.brightness    = clampByte(server.arg("brightness").toInt(), 0, 15);

  cfg.autoDST       = (server.arg("autoDST") == "1");
  cfg.tzString      = server.arg("tzString");
  cfg.tzOffset      = toLongOr(server.arg("tzOffset"), cfg.tzOffset);
  cfg.dstSimple     = (server.arg("dstSimple") == "1");

  cfg.tiltActiveLow = (server.arg("tiltActiveLow") == "1");
  cfg.ledHeartbeat  = (server.arg("ledHeartbeat") == "1");

  cfg.mqttHost      = server.arg("mqttHost");
  cfg.mqttPort      = toU16Or(server.arg("mqttPort"), 1883);
  cfg.mqttTopic     = server.arg("mqttTopic");

  cfg.otaHttpEnabled    = (server.arg("otaHttpEnabled") == "1");
  cfg.otaHttpUser       = server.arg("otaHttpUser");
  cfg.otaHttpPass       = server.arg("otaHttpPass");
  cfg.otaArduinoEnabled = (server.arg("otaArduinoEnabled") == "1");
  cfg.otaArduinoPassword= server.arg("otaArduinoPassword");

  saveConfig();
  applyTZ();

  server.send(200, "text/plain", "Saved. Rebooting...");
  delay(400);
  ESP.restart();
}

void handleConfigJson() {
  StaticJsonDocument<900> d;
  d["wifiSsid"]      = cfg.wifiSsid;
  d["ntpHost"]       = cfg.ntpHost;
  d["use24h"]        = cfg.use24h;
  d["brightness"]    = cfg.brightness;

  d["autoDST"]       = cfg.autoDST;
  d["tzString"]      = cfg.tzString;
  d["tzOffset"]      = cfg.tzOffset;
  d["dstSimple"]     = cfg.dstSimple;

  d["tiltActiveLow"] = cfg.tiltActiveLow;
  d["ledHeartbeat"]  = cfg.ledHeartbeat;

  d["mqttHost"]      = cfg.mqttHost;
  d["mqttPort"]      = cfg.mqttPort;
  d["mqttTopic"]     = cfg.mqttTopic;

  d["otaHttpEnabled"]= cfg.otaHttpEnabled;
  d["otaHttpUser"]   = cfg.otaHttpUser;
  d["otaHttpPass"]   = cfg.otaHttpPass;
  d["otaArduinoEnabled"] = cfg.otaArduinoEnabled;
  d["otaArduinoPassword"]= cfg.otaArduinoPassword;

  String out; serializeJson(d, out);
  server.send(200, "application/json", out);
}

// --------- MQTT ----------
void showMessageFor5s(const String& m) {
  scrollMessage = m;
  mode = MODE_SCROLL;
  scrollUntilMs = millis() + 5000;
  // Scroll so you read words in normal order
  P.setTextAlignment(PA_RIGHT);
  P.displaySuspend(false);
  P.displayText((char*)scrollMessage.c_str(),
                PA_RIGHT,
                40, 0,
                PA_SCROLL_RIGHT,
                PA_SCROLL_RIGHT);
  Serial.printf("MQTT: \"%s\"\n", scrollMessage.c_str());
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg; msg.reserve(length);
  for (unsigned int i=0; i<length; i++) msg += (char)payload[i];
  msg.replace("\r"," "); msg.replace("\n"," ");
  if (msg.length()) showMessageFor5s(msg);
}

void ensureMqttConnected() {
  if (mqtt.connected()) return;
  String clientId = "matrixclock-" + String(ESP.getChipId(), HEX);
  if (mqtt.connect(clientId.c_str())) {
    mqtt.subscribe(cfg.mqttTopic.c_str());
    Serial.printf("MQTT connected, subscribed to %s\n", cfg.mqttTopic.c_str());
  }
}

// --------- Helpers ----------
bool findRTC() {
  for (auto &p : I2C_CANDIDATES) {
    Wire.begin(p.sda, p.scl);
    delay(10);
    Wire.beginTransmission(RTC_ADDR);
    if (Wire.endTransmission() == 0) {
      I2C_SDA = p.sda; I2C_SCL = p.scl;
      Serial.printf("RTC found at 0x68 on SDA=%d SCL=%d\n", I2C_SDA, I2C_SCL);
      return true;
    }
  }
  Serial.println(F("RTC not found on any candidate I2C pins"));
  return false;
}

void bootSplash() {
  // Remember current spacing/alignment
  int8_t oldSpacing = P.getCharSpacing();
  textPosition_t oldAlign = P.getTextAlignment();

  // Tighter spacing so 6 chars fit nicely on 32 cols
  P.setCharSpacing(0);
  P.setTextAlignment(PA_CENTER);
  P.displaySuspend(false);

  // "MATRIX" (no effects)
  P.displayText((char*)"MATRIX", PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
  while (!P.displayAnimate()) { /* wait for draw */ }
  delay(1500);

  P.setCharSpacing(oldSpacing);

  // "CLOCK" (no effects)
  P.displayText((char*)"CLOCK", PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
  while (!P.displayAnimate()) { /* wait for draw */ }
  delay(1500);

  // Restore your usual settings (centre is what you use for the clock anyway)
  P.setTextAlignment(oldAlign);
}


// Resolve local time: read UTC (RTC or SW), convert to local using TZ (or fallback)
bool getLocalTimeNow(struct tm &tLocal) {
  struct tm tUTC{};
  if (rtcPresent) {
    if (!rtcRead(tUTC)) {
      Serial.println(F("RTC read failed, falling back to software clock (UTC)"));
      goto use_sw;
    }
  } else {
  use_sw:
    if (bootEpoch == 0) {
      tUTC = {}; tUTC.tm_year = 124; tUTC.tm_mon = 0; tUTC.tm_mday = 1; // 2024-01-01 00:00:00 UTC
    } else {
      time_t nowUTC = bootEpoch + (time_t)((millis() - bootMillis)/1000);
      gmtime_r(&nowUTC, &tUTC);
    }
  }

  time_t epochUTC = epochFromUtcTm(tUTC);

  if (cfg.autoDST) {
    // libc path
    struct tm lt{}; localtime_r(&epochUTC, &lt);
    // Read numeric offset via %z (robust on ESP8266)
    char zbuf[8]={0}; strftime(zbuf, sizeof(zbuf), "%z", &lt);
    long off = parse_tz_offset(zbuf);
    if (off != 0) { tLocal = lt; return true; }

    // Last-ditch NSW fallback if TZ ignored (rare)
    auto firstSunday=[&](int Y,int M){
      int y=Y, m=M; if(m<3){ m+=12; y-=1; }
      int K=y%100, J=y/100;
      int h=(1+(13*(m+1))/5+K+K/4+J/4+5*J)%7; // 0=Sat..6=Fri
      int dow=(h+6)%7;                         // 0=Sun..6=Sat
      return (dow==0)?1:(8-dow);
    };
    int Y; { struct tm u; gmtime_r(&epochUTC,&u); Y=u.tm_year+1900; }
    int sd=firstSunday(Y,10), ed=firstSunday(Y,4);
    auto mkUTC=[&](int y,int M,int D,int h,int mi){
      struct tm t{}; t.tm_year=y-1900; t.tm_mon=M-1; t.tm_mday=D; t.tm_hour=h; t.tm_min=mi;
      return epochFromUtcTm(t);
    };
    time_t startUTC = mkUTC(Y,10,sd,16,0);  // enter DST (02:00 AEST = 16:00Z prev day)
    time_t endUTC   = mkUTC(Y, 4,ed,16,0);  // leave DST (03:00 AEDT = 16:00Z prev day)
    bool dst = (epochUTC >= startUTC) || (epochUTC < endUTC);
    long fix = dst ? 11*3600L : 10*3600L;
    time_t localEpoch = epochUTC + fix;
    gmtime_r(&localEpoch, &tLocal);
    tLocal.tm_isdst = dst ? 1 : 0;
    return true;

  } else {
    long off = cfg.tzOffset + (cfg.dstSimple ? 3600 : 0);
    time_t localEpoch = epochUTC + off;
    gmtime_r(&localEpoch, &tLocal);
    return true;
  }
}

// -------------------- OTA setup helpers --------------------
void setupHttpUpdater() {
  if (!cfg.otaHttpEnabled) return;
  if (cfg.otaHttpPass.length() > 0) {
    httpUpdater.setup(&server, "/update", cfg.otaHttpUser, cfg.otaHttpPass);
    Serial.println(F("HTTP OTA enabled at /update (auth on)"));
  } else {
    httpUpdater.setup(&server, "/update"); // no auth
    Serial.println(F("HTTP OTA enabled at /update (no auth)"));
  }
}

void setupArduinoOTA() {
  if (!cfg.otaArduinoEnabled) return;
  ArduinoOTA.setHostname(("matrixclock-" + String(ESP.getChipId(), HEX)).c_str());
  if (cfg.otaArduinoPassword.length() > 0) {
    ArduinoOTA.setPassword(cfg.otaArduinoPassword.c_str());
  }
  ArduinoOTA.onStart([](){ Serial.println(F("ArduinoOTA start")); });
  ArduinoOTA.onEnd([](){ Serial.println(F("ArduinoOTA end")); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t){
    Serial.printf("ArduinoOTA %u%%\n", (p*100)/t);
  });
  ArduinoOTA.onError([](ota_error_t e){ Serial.printf("ArduinoOTA error %u\n", e); });
  ArduinoOTA.begin();
  Serial.println(F("ArduinoOTA service started"));
}

// -------------------- Setup / Loop --------------------
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println(); Serial.println(F("Booting Matrix Clock (Icstation / UTC RTC + POSIX TZ + OTA)"));

  // Filesystem
  LittleFS.begin();
  if (!loadConfig()) Serial.println(F("Using default config"));

  applyTZ(); // set TZ env before any time ops
  Serial.printf("TZ applied: %s\n", cfg.tzString.c_str());
  Serial.printf("TZ working? %s\n", tzAppearsWorking() ? "yes" : "no (will fallback if needed)");

  // Display
  P.begin();
  P.setZone(0, 0, MODULES - 1);
  P.setIntensity(cfg.brightness);
  P.setTextAlignment(PA_CENTER);   // clock centred by default
  P.displaySuspend(false);

  // LED heartbeat setup
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LED_ACTIVE_LOW ? HIGH : LOW); // start off

  // Mercury switch
  pinMode(PIN_MERCURY, INPUT_PULLUP);
  lastRawMerc = (digitalRead(PIN_MERCURY) == (cfg.tiltActiveLow ? LOW : HIGH));
  lastMercMs = millis();
  applyFlip(lastRawMerc);
  inverted = lastRawMerc;

  // Find RTC
  rtcPresent = findRTC();
  if (rtcPresent) Wire.begin(I2C_SDA, I2C_SCL);

  // Splash (kept visible)
  bootSplash();

  // Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
  Serial.printf("Connecting Wi-Fi SSID \"%s\" ...\n", cfg.wifiSsid.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) { delay(200); Serial.print('.'); }
  Serial.println();

  String modeStr = "STA";
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Wi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
  } else {
    WiFi.mode(WIFI_AP);
    String apSsid = String("MatrixClock-") + String(ESP.getChipId(), HEX);
    WiFi.softAP(apSsid.c_str());
    Serial.printf("STA failed; AP started: SSID=%s, IP=%s\n",
                  apSsid.c_str(), WiFi.softAPIP().toString().c_str());
    modeStr = "AP";
  }

  // Web server (settings & HTTP OTA)
  server.on("/", [](){ handleRoot(); });
  server.on("/save", HTTP_POST, [](){ handleSave(); });
  server.on("/config.json", [](){ handleConfigJson(); });
  setupHttpUpdater();
  server.begin();
  Serial.printf("Web UI ready (%s). Open http://%s/\n", modeStr.c_str(),
                ((modeStr=="AP") ? WiFi.softAPIP().toString().c_str()
                                 : WiFi.localIP().toString().c_str()));

  // MQTT
  mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
  mqtt.setCallback(mqttCallback);

  // ArduinoOTA (optional)
  setupArduinoOTA();

  // NTP (writes UTC to RTC or seeds SW clock in UTC)
  if ((WiFi.getMode() & WIFI_STA) && WiFi.status() == WL_CONNECTED) {
    if (!syncFromNTP()) Serial.println(F("NTP sync failed; using RTC or software clock"));
  } else {
    Serial.println(F("No STA; skipping NTP for now"));
  }

  lastResync = millis();
  lastClockTick = millis();

  // Leave splash visible until first render
}

void loop() {
  updateOrientation();

  // Web & HTTP OTA
  server.handleClient();

  // ArduinoOTA
  if (cfg.otaArduinoEnabled) ArduinoOTA.handle();

  // MQTT
  if ((WiFi.getMode() & WIFI_STA) && WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) ensureMqttConnected();
    mqtt.loop();
  }

  // Hourly NTP resync
  if ((WiFi.getMode() & WIFI_STA) && WiFi.status() == WL_CONNECTED &&
      millis() - lastResync >= RESYNC_INTERVAL_MS) {
    if (syncFromNTP()) Serial.println(F("Re-synced from NTP"));
    lastResync = millis();
  }

  // Display state machine
  switch (mode) {
    case MODE_SCROLL: {
      if (P.displayAnimate()) {
        if (millis() >= scrollUntilMs) {
          mode = MODE_CLOCK;
          P.setTextAlignment(PA_CENTER);   // restore centring after scroll
        } else {
          P.displayReset(); // keep scrolling during the window
        }
      }
    } break;

    case MODE_CLOCK: {
      if (millis() - lastClockTick >= 1000) {
        lastClockTick += 1000;

        // hardware LED heartbeat (optional)
        static bool ledOn = false;
        if (cfg.ledHeartbeat) {
          ledOn = !ledOn;
          digitalWrite(LED_BUILTIN, LED_ACTIVE_LOW ? (ledOn ? LOW : HIGH)
                                                   : (ledOn ? HIGH : LOW));
        } else {
          digitalWrite(LED_BUILTIN, LED_ACTIVE_LOW ? HIGH : LOW); // off
        }

        struct tm tLocal;
        getLocalTimeNow(tLocal);
        String s = formatTime(tLocal.tm_hour, tLocal.tm_min); // steady colon
        P.print(s.c_str());
      }
    } break;
  }
}
