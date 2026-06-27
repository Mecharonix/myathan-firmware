/** MyAthan v2.2 - Location & UI Refresh + GitHub OTA
 *
 * WIRING (unchanged):
 * - MP3 Module TX      ->  ESP8266 D1 (GPIO 5)
 * - MP3 Module RX      ->  ESP8266 D2 (GPIO 4)
 * - LED / Relay (+)    ->  ESP8266 D8 (GPIO 15)
 * - Volume UP Button   ->  ESP8266 D6 (GPIO 12)
 * - Volume DOWN Button ->  ESP8266 D7 (GPIO 13)
 */

#include <Arduino.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <JQ6500_Serial.h>
#include <SoftwareSerial.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <ESP8266httpUpdate.h>

// --- Configuration ---
const char *AP_SSID = "MyAthan Setup";
const int READY_PIN = 15;       // D8 Trigger
SoftwareSerial mp3Serial(5, 4); // RX, TX (D1, D2)
JQ6500_Serial mp3(mp3Serial);
ESP8266WebServer server(80);
DNSServer dnsServer;

// --- Firmware version + GitHub-synced OTA ---
// Bump FW_VERSION on every release; the CI publishes it to version.txt and the
// device compares against it. URLs point at a fixed pre-release tag so the
// ESP8266 never picks up the repo's ESP32-C3 releases.
#define FW_VERSION "2.2.3"
// Daily automatic OTA check, deliberately offset from the 00:00 prayer refresh so they never collide
#define OTA_CHECK_HOUR 0
#define OTA_CHECK_MIN  30
const char *OTA_VERSION_URL =
    "https://github.com/Mecharonix/myathan-firmware/releases/download/esp8266-latest/version.txt";
const char *OTA_BIN_URL =
    "https://github.com/Mecharonix/myathan-firmware/releases/download/esp8266-latest/firmware.bin";
String otaStatus = "Idle";

// --- EEPROM Layout ---
#define EE_VOL     0  // 1 byte volume
#define EE_MAGIC1  1  // 1 byte magic
#define EE_MAGIC2  2  // 1 byte magic
#define EE_MANUAL  3  // 1 byte manual-mode flag
#define EE_LAT     4  // 4 bytes float
#define EE_LON     8  // 4 bytes float
#define EE_OFFSET 12  // 4 bytes long (tz seconds)
#define EE_METHOD 16  // 1 byte AlAdhan calculation method id
#define EE_SCHOOL 17  // 1 byte Asr school (0=Standard, 1=Hanafi)
#define EE_LATADJ 18  // 1 byte high-latitude rule (0=None,1=MidNight,2=OneSeventh,3=AngleBased)
#define EE_AUTOTZ 19  // 1 byte auto-timezone flag (derive UTC offset from API)
#define EE_TUNE_FLAG 20  // 1 byte sentinel (0xA5 = per-prayer tune offsets stored)
#define EE_TUNE   21  // 5 x int16_t = 10 bytes, per-prayer minute offsets (21..30)
#define EE_TUNE_SET 0xA5
#define EE_SIZE  512
#define MAGIC1 0xCA
#define MAGIC2 0xFE

// --- State ---
bool isAPMode = false;
bool firstSync = true;
float myLat = 0.0, myLon = 0.0;
long currentOffset = 0;
bool isManual = false;
byte currentVolume = 20;
bool isAthanActive = false;
unsigned long athanOffTime = 0;
unsigned long statusGracePeriod = 0;

// --- Prayer-time calculation settings (configurable via myathan.local) ---
byte calcMethod  = 4;     // AlAdhan method id (4 = Umm al-Qura, matches Quran Majeed)
byte asrSchool   = 0;     // 0 = Standard (Shafi/Maliki/Hanbali), 1 = Hanafi
byte latAdjMethod = 3;    // high-latitude rule: 0=None, 1=MidNight, 2=OneSeventh, 3=AngleBased
bool autoTz      = true;  // derive DST-correct UTC offset from the API (keeps triggers on time)
long apiOffset   = 0;     // UTC offset (seconds) parsed from the last API response
bool apiOffsetValid = false;
// Per-prayer fine-tune offsets in minutes (Fajr, Dhuhr, Asr, Maghrib, Isha); may be negative
int  tuneOffset[5] = {0, 0, 0, 0, 0};

bool hasFirstSync = false;
unsigned long lastFullSync = 0;     // millis() of last sync ATTEMPT (success or fail)
bool forceSync = false;             // one-shot flag set by SSID change / manual trigger
String lastSeenSSID = "";
String lastSyncStatus = "Pending";
time_t lastSyncEpoch = 0;           // unix time of last SUCCESSFUL sync

struct Prayer {
  String name;
  int hour;
  int minute;
  int readyHour;
  int readyMinute;
  bool played;
};
Prayer prayers[5];
const char *prayerNames[] = {"Fajr", "Dhuhr", "Asr", "Maghrib", "Isha"};

// --- Forward decls ---
void doFullSync();
String runOtaCheck(bool applyUpdate);
bool fetchPrayerTimes();

// --- EEPROM Helpers ---
void saveLocationToEEPROM() {
  EEPROM.write(EE_MAGIC1, MAGIC1);
  EEPROM.write(EE_MAGIC2, MAGIC2);
  EEPROM.write(EE_MANUAL, isManual ? 1 : 0);
  EEPROM.put(EE_LAT, myLat);
  EEPROM.put(EE_LON, myLon);
  EEPROM.put(EE_OFFSET, currentOffset);
  EEPROM.write(EE_METHOD, calcMethod);
  EEPROM.write(EE_SCHOOL, asrSchool);
  EEPROM.write(EE_LATADJ, latAdjMethod);
  EEPROM.write(EE_AUTOTZ, autoTz ? 1 : 0);
  EEPROM.write(EE_TUNE_FLAG, EE_TUNE_SET);
  for (int i = 0; i < 5; i++)
    EEPROM.put(EE_TUNE + i * 2, (int16_t)tuneOffset[i]);
  if (EEPROM.commit())
    Serial.println("[EEPROM] Location saved");
  else
    Serial.println("[EEPROM] Save FAILED");
}

void loadLocationFromEEPROM() {
  if (EEPROM.read(EE_MAGIC1) != MAGIC1 || EEPROM.read(EE_MAGIC2) != MAGIC2) {
    Serial.println("[EEPROM] No saved location yet");
    return;
  }
  isManual = (EEPROM.read(EE_MANUAL) == 1);
  EEPROM.get(EE_LAT, myLat);
  EEPROM.get(EE_LON, myLon);
  EEPROM.get(EE_OFFSET, currentOffset);

  // Calculation settings (added later) - fall back to defaults for upgraded
  // devices whose EEPROM never stored these bytes (read back as 0xFF/out-of-range).
  byte m = EEPROM.read(EE_METHOD);
  calcMethod = (m <= 23) ? m : 4;   // valid AlAdhan ids are 0..23; anything else -> Umm al-Qura
  byte sc = EEPROM.read(EE_SCHOOL);
  asrSchool = (sc <= 1) ? sc : 0;
  byte la = EEPROM.read(EE_LATADJ);
  latAdjMethod = (la <= 3) ? la : 3;
  byte tz = EEPROM.read(EE_AUTOTZ);
  autoTz = (tz <= 1) ? (tz == 1) : true;

  // Per-prayer tune offsets: only trust them when the sentinel is present, otherwise
  // an unwritten EEPROM (0xFFFF -> -1) would silently shift every prayer.
  if (EEPROM.read(EE_TUNE_FLAG) == EE_TUNE_SET) {
    for (int i = 0; i < 5; i++) {
      int16_t v = 0;
      EEPROM.get(EE_TUNE + i * 2, v);
      if (v < -1440) v = -1440;
      if (v >  1440) v =  1440;
      tuneOffset[i] = v;
    }
  } else {
    for (int i = 0; i < 5; i++) tuneOffset[i] = 0;
  }

  Serial.printf("[EEPROM] Loaded Lat=%.4f Lon=%.4f Offset=%lds %s | method=%d school=%d latAdj=%d autoTz=%d tune=%d,%d,%d,%d,%d\n",
                myLat, myLon, currentOffset, isManual ? "MANUAL" : "AUTO",
                calcMethod, asrSchool, latAdjMethod, autoTz,
                tuneOffset[0], tuneOffset[1], tuneOffset[2], tuneOffset[3], tuneOffset[4]);
}

// --- Utilities ---
boolean isIp(String str) {
  for (size_t i = 0; i < str.length(); i++) {
    int c = str.charAt(i);
    if (c != '.' && (c < '0' || c > '9') && c != ':')
      return false;
  }
  return true;
}

String jsonEscape(const String &s) {
  String out;
  out.reserve(s.length() + 4);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((unsigned char)c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

// =====================================================
// SETUP PAGE (AP mode) - unchanged design
// =====================================================
void serveSetupPage() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");

  String html = "<!DOCTYPE html><html><head><meta name='viewport' "
                "content='width=device-width, initial-scale=1'>";
  html += "<title>MyAthan Setup</title>";
  html += "<style>";
  html += "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', "
          "Roboto, Helvetica, Arial, sans-serif; background: #f0f2f5; display: "
          "flex; align-items: center; justify-content: center; height: 100vh; "
          "margin: 0; }";
  html += ".card { background: white; padding: 2rem; border-radius: 12px; "
          "box-shadow: 0 4px 6px rgba(0,0,0,0.1); width: 100%; max-width: 320px; }";
  html += "h1 { color: #1a1a1a; margin-top: 0; font-size: 24px; text-align: center; }";
  html += "input { width: 100%; padding: 12px; margin: 8px 0; display: "
          "inline-block; border: 1px solid #ccc; border-radius: 4px; "
          "box-sizing: border-box; font-size: 16px; }";
  html += "input[type=submit] { width: 100%; background-color: #4CAF50; color: "
          "white; padding: 14px 20px; margin: 8px 0; border: none; border-radius: "
          "4px; cursor: pointer; font-size: 16px; font-weight: bold; }";
  html += "input[type=submit]:hover { background-color: #45a049; }";
  html += ".label { font-weight: bold; color: #555; display: block; margin-top: 10px; }";
  html += "</style></head><body>";

  html += "<div class='card'>";
  html += "<h1>Enter your WiFi credentials</h1>";
  html += "<form method='POST' action='/save'>";
  html += "<label class='label'>WiFi Name (SSID)</label>";
  html += "<input type='text' name='ssid' placeholder='Enter SSID'>";
  html += "<label class='label'>Password</label>";
  html += "<input type='text' name='pass' placeholder='Enter Password'>";
  html += "<input type='submit' value='Connect'>";
  html += "</form></div>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

boolean handleCaptivePortal() {
  String host = server.hostHeader();
  String uri = server.uri();

  if (uri.indexOf("connecttest") != -1 || uri.indexOf("ncsi") != -1 ||
      uri.indexOf("generate_204") != -1 || uri.indexOf("redirect") != -1 ||
      uri.indexOf("success.txt") != -1 || uri.indexOf("hotspot-detect") != -1) {
    serveSetupPage();
    return true;
  }

  if (!isIp(host) && host != "myathan.local") {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
    server.client().stop();
    return true;
  }
  return false;
}

void handleRoot() {
  if (handleCaptivePortal()) return;
  serveSetupPage();
}

void handleSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  if (ssid.length() > 0) {
    String html = "<html><head><meta name='viewport' "
                  "content='width=device-width, initial-scale=1.0'></head>";
    html += "<body style='font-family:sans-serif; text-align:center; "
            "padding-top:100px; background:#f0f2f5;'>";
    html += "<div style='display:inline-block; background:white; padding:40px; "
            "border-radius:20px; box-shadow:0 10px 20px rgba(0,0,0,0.1);'>";
    html += "<h1 style='color:#27ae60; font-size:48px; margin-bottom:20px;'>Success!</h1>";
    html += "<p style='font-size:24px; color:#333;'>Connecting to "
            "WiFi...<br><b>You can now close this page!</b></p>";
    html += "</div></body></html>";
    server.send(200, "text/html", html);
    delay(1000);
    WiFi.persistent(true);
    WiFi.setAutoConnect(true);
    WiFi.begin(ssid.c_str(), pass.c_str());
    delay(500);
    ESP.restart();
  }
}

void handleNotFound() { handleRoot(); }

// =====================================================
// CONTROL PAGE (STA mode) - new UI with map
// =====================================================
const char CONTROL_PAGE[] PROGMEM = R"=====(<!DOCTYPE html>
<html lang='en'><head>
<meta name='viewport' content='width=device-width, initial-scale=1, user-scalable=no'>
<meta charset='utf-8'>
<title>MyAthan</title>
<link rel='stylesheet' href='https://unpkg.com/leaflet@1.9.4/dist/leaflet.css' crossorigin=''>
<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;background:linear-gradient(160deg,#0f6651 0%,#1a4d3a 100%);min-height:100vh;color:#1a1a1a;padding-bottom:24px}
.app{max-width:620px;margin:0 auto;padding:16px}
.header{display:flex;align-items:center;justify-content:space-between;padding:18px 6px 14px;color:#fff}
.header h1{margin:0;font-size:24px;font-weight:700;letter-spacing:.5px}
.header .sub{font-size:11px;opacity:.7;letter-spacing:2px;text-transform:uppercase}
.badge{font-size:12px;padding:6px 12px;border-radius:14px;font-weight:600;display:inline-flex;align-items:center;gap:6px}
.badge.ok{background:rgba(39,174,96,.95);color:#fff}
.badge.bad{background:rgba(231,76,60,.95);color:#fff}
.badge.warn{background:rgba(243,156,18,.95);color:#fff}
.dot{width:8px;height:8px;border-radius:50%;display:inline-block}
.dot.ok{background:#fff;box-shadow:0 0 0 0 rgba(255,255,255,.7);animation:pulse 2s infinite}
.dot.bad{background:#fff}
.dot.warn{background:#fff}
@keyframes pulse{0%{box-shadow:0 0 0 0 rgba(255,255,255,.5)}70%{box-shadow:0 0 0 8px rgba(255,255,255,0)}100%{box-shadow:0 0 0 0 rgba(255,255,255,0)}}
.card{background:#fff;border-radius:16px;padding:18px;margin-bottom:14px;box-shadow:0 6px 20px rgba(0,0,0,.15)}
.card h2{margin:0 0 14px;font-size:13px;color:#7c8a99;text-transform:uppercase;letter-spacing:1.5px;font-weight:700}
.row{display:flex;gap:10px;margin:10px 0}
.row>*{flex:1;min-width:0}
button{padding:14px;border:none;border-radius:10px;font-size:15px;font-weight:600;cursor:pointer;width:100%;transition:transform .1s,opacity .2s;font-family:inherit}
button:active{transform:scale(.97)}
button:disabled{opacity:.5;cursor:not-allowed}
.btn-play{background:#27ae60;color:#fff}
.btn-stop{background:#e74c3c;color:#fff}
.btn-save{background:#2c7be5;color:#fff;margin-top:10px}
.btn-auto{background:#7f8c8d;color:#fff;margin-top:8px}
.btn-sync{background:#34495e;color:#fff;margin-top:10px;font-size:13px;padding:10px}
.btn-restart{background:#f39c12;color:#fff}
input[type=text],input[type=number]{padding:12px;border:1px solid #dfe4ea;border-radius:10px;font-size:14px;width:100%;font-family:inherit}
select{padding:12px;border:1px solid #dfe4ea;border-radius:10px;font-size:14px;width:100%;font-family:inherit;background:#fff;-webkit-appearance:menulist}
input.tune{width:66px;flex:0 0 auto;padding:7px 6px;text-align:center;font-size:13px;border:1px solid #dfe4ea;border-radius:8px;font-family:inherit}
.ptright{display:flex;align-items:center;gap:10px}
input[type=range]{width:100%;margin:6px 0;accent-color:#0f6651}
.status-line{display:flex;justify-content:space-between;align-items:center;padding:9px 0;border-bottom:1px solid #f0f2f5;font-size:14px}
.status-line:last-child{border-bottom:0}
.status-line b{color:#7c8a99;font-weight:500}
.status-line span{color:#1a1a1a;font-weight:600;text-align:right;word-break:break-all}
#map{height:300px;border-radius:12px;border:1px solid #dfe4ea;margin:10px 0;overflow:hidden;background:#f0f2f5}
.ptime{display:flex;justify-content:space-between;align-items:center;padding:11px 14px;border-radius:10px;margin:6px 0;background:#f7f9fa;font-size:15px}
.ptime.next{background:linear-gradient(90deg,#e8f5e9,#f0fff4);border-left:4px solid #27ae60}
.ptime.next .pn{color:#0f6651}
.ptime.next .pt{color:#0f6651}
.ptime .pn{font-weight:600;color:#333}
.ptime .pt{font-family:'SF Mono','Menlo','Consolas',monospace;color:#1a4d3a;font-weight:700}
.ptime .nx{font-size:10px;font-weight:700;color:#27ae60;background:rgba(39,174,96,.15);padding:3px 8px;border-radius:6px;margin-left:8px}
.hint{font-size:13px;color:#7c8a99;margin:4px 0 10px}
.lbl{font-size:13px;color:#555;font-weight:500;display:block;margin:10px 0 4px}
.lbl b{color:#0f6651;font-weight:700;float:right}
.mode-pill{display:inline-block;padding:3px 10px;border-radius:10px;font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:.5px}
.mode-pill.auto{background:#e3f2fd;color:#1565c0}
.mode-pill.manual{background:#fff3e0;color:#e65100}
</style></head>
<body>
<div class='app'>
<div class='header'>
  <div>
    <div class='sub'>Prayer Reminder</div>
    <h1>MyAthan</h1>
  </div>
  <span id='wifi-badge' class='badge warn'><span class='dot warn'></span>Loading</span>
</div>

<div class='card'>
<h2>Player</h2>
<div class='status-line'><b>Current Status</b><span id='play-status'>...</span></div>
<div style='margin-top:12px'>
<div class='row' style='margin:0 0 14px'>
<button class='btn-play' onclick="cmd('/play')">&#9658; Play Athan</button>
<button class='btn-stop' onclick="cmd('/stop')">&#9632; Stop Athan</button>
</div>
<label class='lbl'>Volume <b id='vol-display'>--%</b></label>
<input type='range' id='vol-in' min='0' max='100' step='1' value='66'>
</div>
</div>

<div class='card'>
<h2>Prayer Times Today</h2>
<div id='prayers'>Loading...</div>
<div id='tune-actions' style='display:none;margin-top:6px'>
<p class='hint'>The box next to each prayer shifts that time by the given number of minutes (negative = earlier). It applies to both the shown time and the athan trigger.</p>
<button class='btn-save' onclick='saveTune()'>Save Time Adjustments</button>
</div>
</div>

<div class='card'>
<h2>Calculation Settings</h2>
<p class='hint'>Match these to your phone app so the device agrees with it. Quran Majeed uses <b>Umm al-Qura</b>. The high-latitude rule mainly changes Fajr &amp; Isha at northern latitudes (e.g. Germany).</p>
<label class='lbl'>Calculation Method</label>
<select id='method-in'>
<option value='4'>Umm al-Qura University, Makkah</option>
<option value='3'>Muslim World League</option>
<option value='2'>ISNA (North America)</option>
<option value='5'>Egyptian General Authority</option>
<option value='1'>Univ. of Islamic Sciences, Karachi</option>
<option value='7'>Univ. of Tehran (Geophysics)</option>
<option value='8'>Gulf Region</option>
<option value='9'>Kuwait</option>
<option value='10'>Qatar</option>
<option value='11'>Singapore (MUIS)</option>
<option value='12'>France (UOIF)</option>
<option value='13'>Diyanet, Turkey</option>
<option value='16'>Dubai</option>
<option value='17'>Malaysia (JAKIM)</option>
<option value='18'>Tunisia</option>
<option value='19'>Algeria</option>
<option value='20'>Indonesia (Kemenag)</option>
<option value='21'>Morocco</option>
<option value='22'>Lisbon, Portugal</option>
<option value='23'>Jordan</option>
<option value='15'>Moonsighting Committee</option>
<option value='14'>Russia (Spiritual Admin)</option>
<option value='0'>Shia Ithna-Ashari (Jafari)</option>
</select>
<label class='lbl'>Asr (Juristic) Method</label>
<select id='school-in'>
<option value='0'>Standard (Shafi / Maliki / Hanbali)</option>
<option value='1'>Hanafi</option>
</select>
<label class='lbl'>High-Latitude Rule</label>
<select id='latadj-in'>
<option value='0'>None / API default</option>
<option value='1'>Middle of the Night</option>
<option value='2'>One-Seventh of the Night</option>
<option value='3'>Angle Based</option>
</select>
<label class='lbl' style='margin-top:14px'><input type='checkbox' id='autotz-in' onchange='toggleTz()' style='width:auto;margin-right:8px;vertical-align:middle'>Automatic timezone (recommended)</label>
<p class='hint'>When on, the UTC offset including daylight saving is taken from your location automatically, so the athan always fires on time. Turn it off only to force a manual offset below.</p>
<button class='btn-save' onclick='saveSettings()'>Save Calculation Settings</button>
</div>

<div class='card'>
<h2>Current Location</h2>
<div class='status-line'><b>Mode</b><span id='mode'>--</span></div>
<div class='status-line'><b>Latitude</b><span id='lat'>--</span></div>
<div class='status-line'><b>Longitude</b><span id='lon'>--</span></div>
<div class='status-line'><b>UTC Offset</b><span id='offset'>--</span></div>
<div class='status-line'><b>Last Sync</b><span id='lsync'>Never</span></div>
<div class='status-line'><b>Sync Status</b><span id='sstatus'>--</span></div>
<button class='btn-sync' onclick='triggerSync()'>&#x21bb; Re-sync Now</button>
</div>

<div class='card'>
<h2>Network</h2>
<div class='status-line'><b>SSID</b><span id='ssid'>--</span></div>
<div class='status-line'><b>IP Address</b><span id='ip'>--</span></div>
<div class='status-line'><b>Signal</b><span id='rssi'>--</span></div>
</div>

<div class='card'>
<h2>Manual Location</h2>
<p class='hint'>Tap anywhere on the map to set your prayer location, or type coordinates directly. With <b>Automatic timezone</b> on (above), the UTC offset is set from your location for you &mdash; the slider below is only used when you turn that off.</p>
<div id='map'></div>
<div class='row'>
<div><label class='lbl'>Latitude</label><input type='number' id='lat-in' placeholder='e.g. 41.0082' step='0.0001'></div>
<div><label class='lbl'>Longitude</label><input type='number' id='lon-in' placeholder='e.g. 28.9784' step='0.0001'></div>
</div>
<label class='lbl'>UTC Offset <b id='offset-display'>UTC+00:00</b></label>
<input type='range' id='offset-in' min='-12' max='14' step='0.25' value='0'>
<button class='btn-save' onclick='saveLoc()'>Save Manual Location</button>
<button class='btn-auto' onclick='setAuto()'>Switch to Automatic Detection</button>
</div>

<div class='card'>
<h2>Device</h2>
<div class='status-line'><b>Firmware</b><span id='fw'>--</span></div>
<div class='status-line'><b>Update</b><span id='ota'>--</span></div>
<p class='hint'>Check GitHub for newer firmware and install it over the air. The device downloads, flashes and reboots (~1 min) &mdash; keep it powered.</p>
<button class='btn-sync' onclick='checkUpdate()'>&#x2b06; Check for Updates</button>
<p class='hint' style='margin-top:14px'>Restart the ESP8266. Prayer triggers will pause for a few seconds while it boots.</p>
<button class='btn-restart' onclick='doRestart()'>&#x21bb; Restart Device</button>
</div>

</div>

<script src='https://unpkg.com/leaflet@1.9.4/dist/leaflet.js' crossorigin=''></script>
<script>
var map = L.map('map', {zoomControl:true}).setView([20,10], 2);
L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',{attribution:'&copy; OpenStreetMap',maxZoom:18}).addTo(map);
var marker = null;
var mapInitialized = false;
var settingsLoaded = false;
var prayersBuilt = false;

function saveTune(){
  var parts = [];
  for (var i=0;i<5;i++){
    var v = parseInt(document.getElementById('tune-'+i).value, 10);
    if (isNaN(v)) v = 0;
    parts.push('t'+i+'='+v);
  }
  fetch('/api/tune',{
    method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body: parts.join('&')
  }).then(function(r){
    if (r.ok){ alert('Time adjustments saved. Re-fetching prayer times...'); setTimeout(refresh,2200); }
    else r.text().then(function(t){ alert('Error: ' + t); });
  });
}

function toggleTz(){
  document.getElementById('offset-in').disabled = document.getElementById('autotz-in').checked;
}

function saveSettings(){
  var body = 'method=' + document.getElementById('method-in').value
    + '&school=' + document.getElementById('school-in').value
    + '&latadj=' + document.getElementById('latadj-in').value
    + '&autotz=' + (document.getElementById('autotz-in').checked ? 1 : 0);
  fetch('/api/settings',{
    method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body: body
  }).then(function(r){
    if (r.ok){ alert('Calculation settings saved. Re-fetching prayer times...'); setTimeout(refresh,2200); }
    else r.text().then(function(t){ alert('Error: ' + t); });
  });
}

map.on('click', function(e){ setMarker(e.latlng.lat, e.latlng.lng, true); });

function setMarker(lat, lon, autoOffset){
  if (marker) marker.remove();
  marker = L.marker([lat,lon]).addTo(map);
  document.getElementById('lat-in').value = lat.toFixed(4);
  document.getElementById('lon-in').value = lon.toFixed(4);
  if (autoOffset){
    var oh = Math.round(lon/15);
    if (oh < -12) oh = -12; if (oh > 14) oh = 14;
    document.getElementById('offset-in').value = oh;
    updateOffsetDisplay();
  }
}

function fmtOffset(v){
  var s = v >= 0 ? '+' : '-';
  var a = Math.abs(v);
  var h = Math.floor(a);
  var m = Math.round((a-h)*60);
  return 'UTC' + s + String(h).padStart(2,'0') + ':' + String(m).padStart(2,'0');
}
function updateOffsetDisplay(){
  document.getElementById('offset-display').textContent = fmtOffset(parseFloat(document.getElementById('offset-in').value));
}
document.getElementById('offset-in').addEventListener('input', updateOffsetDisplay);

function cmd(u){ fetch(u).then(function(){ setTimeout(refresh,250); }); }
function triggerSync(){
  fetch('/api/sync',{method:'POST'}).then(function(){
    var b = document.getElementById('sstatus');
    b.textContent = 'Syncing...';
    setTimeout(refresh, 2500);
  });
}

var volSlider = document.getElementById('vol-in');
var volDisplay = document.getElementById('vol-display');
var volUserActive = false;
volSlider.addEventListener('input', function(){
  volUserActive = true;
  volDisplay.textContent = this.value + '%';
});
volSlider.addEventListener('change', function(){
  fetch('/api/volume', {
    method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body: 'vol=' + encodeURIComponent(this.value)
  }).finally(function(){
    setTimeout(function(){ volUserActive = false; }, 800);
  });
});

function doRestart(){
  if (!confirm('Restart the device now? It will be offline for ~5 seconds.')) return;
  fetch('/api/restart', {method:'POST'}).catch(function(){});
  var b = document.getElementById('wifi-badge');
  b.className = 'badge warn';
  b.innerHTML = '<span class="dot warn"></span>Restarting...';
  setTimeout(function(){ location.reload(); }, 8000);
}

function checkUpdate(){
  var o = document.getElementById('ota');
  if (o) o.textContent = 'Checking...';
  fetch('/api/ota',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'apply=0'})
    .then(function(r){ return r.text(); })
    .then(function(t){
      if (o) o.textContent = t;
      if (t.indexOf('Update available') === 0){
        if (confirm(t + '\n\nInstall now? The device will download, flash and reboot (~1 minute). Keep it powered.')){
          if (o) o.textContent = 'Updating... device will reboot';
          var b = document.getElementById('wifi-badge');
          b.className = 'badge warn';
          b.innerHTML = '<span class="dot warn"></span>Updating...';
          fetch('/api/ota',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'apply=1'}).catch(function(){});
          setTimeout(function(){ location.reload(); }, 60000);
        }
      } else {
        alert(t);
      }
    })
    .catch(function(){ if (o) o.textContent = 'Check failed'; alert('Update check failed'); });
}

function saveLoc(){
  var lat = document.getElementById('lat-in').value;
  var lon = document.getElementById('lon-in').value;
  var off = Math.round(parseFloat(document.getElementById('offset-in').value) * 3600);
  if (lat === '' || lon === ''){ alert('Please select a location on the map first.'); return; }
  fetch('/api/location', {
    method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body: 'lat='+encodeURIComponent(lat)+'&lon='+encodeURIComponent(lon)+'&offset='+encodeURIComponent(off)
  }).then(function(r){
    if (r.ok){ alert('Manual location saved. Refreshing prayer times...'); setTimeout(refresh,1800); }
    else r.text().then(function(t){ alert('Error: ' + t); });
  });
}

function setAuto(){
  if (!confirm('Switch to automatic location detection? The device will use its public IP to find your location.')) return;
  fetch('/api/auto', {method:'POST'}).then(function(r){
    if (r.ok){ alert('Switched to automatic. Re-detecting location...'); setTimeout(refresh,1800); }
  });
}

function refresh(){
  fetch('/api/status').then(function(r){ return r.json(); }).then(function(s){
    var b = document.getElementById('wifi-badge');
    if (s.connected){ b.className='badge ok'; b.innerHTML='<span class="dot ok"></span>Connected'; }
    else { b.className='badge bad'; b.innerHTML='<span class="dot bad"></span>Offline'; }

    document.getElementById('play-status').textContent = s.playing ? 'Playing' : 'Idle';
    if (!volUserActive && typeof s.volume === 'number') {
      var pct = Math.round((s.volume / 30) * 100);
      volSlider.value = pct;
      volDisplay.textContent = pct + '%';
    }
    document.getElementById('ssid').textContent = s.ssid || '--';
    document.getElementById('ip').textContent = s.ip || '--';
    document.getElementById('rssi').textContent = s.rssi ? (s.rssi + ' dBm') : '--';

    var ml = document.getElementById('mode');
    ml.innerHTML = s.manual
      ? '<span class="mode-pill manual">Manual</span>'
      : '<span class="mode-pill auto">Automatic</span>';
    document.getElementById('lat').textContent = (s.lat).toFixed(4);
    document.getElementById('lon').textContent = (s.lon).toFixed(4);
    document.getElementById('offset').textContent = fmtOffset(s.offset/3600);
    document.getElementById('lsync').textContent = s.lastSync || 'Never';
    document.getElementById('sstatus').textContent = s.syncStatus || '--';
    var fwEl = document.getElementById('fw'); if (fwEl) fwEl.textContent = s.fw || '--';
    var otaEl = document.getElementById('ota'); if (otaEl) otaEl.textContent = s.ota || '--';

    if (!settingsLoaded && typeof s.method === 'number'){
      document.getElementById('method-in').value = String(s.method);
      document.getElementById('school-in').value = String(s.school);
      document.getElementById('latadj-in').value = String(s.latadj);
      document.getElementById('autotz-in').checked = (s.autotz == 1);
      toggleTz();
      settingsLoaded = true;
    }

    var now = s.nowMin;
    var nextIdx = -1, minDiff = 99999;
    if (s.prayers && s.prayers.length){
      for (var i=0;i<s.prayers.length;i++){
        var pm = s.prayers[i].hour*60 + s.prayers[i].minute;
        var d = pm - now; if (d <= 0) d += 1440;
        if (d < minDiff){ minDiff = d; nextIdx = i; }
      }
      // Build the rows (with persistent tune inputs) only once, so the 3s
      // auto-refresh never wipes a value the user is typing.
      if (!prayersBuilt){
        var ph = '';
        for (var i=0;i<s.prayers.length;i++){
          ph += '<div class="ptime" id="prow-'+i+'">'
            + '<span class="pn" id="pn-'+i+'"></span>'
            + '<span class="ptright">'
            + '<span class="pt" id="pt-'+i+'"></span>'
            + '<input type="number" class="tune" id="tune-'+i+'" step="1" value="0" title="minutes offset">'
            + '</span></div>';
        }
        document.getElementById('prayers').innerHTML = ph;
        if (s.tune){
          for (var i=0;i<s.tune.length && i<5;i++)
            document.getElementById('tune-'+i).value = s.tune[i];
        }
        document.getElementById('tune-actions').style.display = 'block';
        prayersBuilt = true;
      }
      for (var i=0;i<s.prayers.length;i++){
        var p = s.prayers[i];
        var t = String(p.hour).padStart(2,'0') + ':' + String(p.minute).padStart(2,'0');
        document.getElementById('pt-'+i).textContent = t;
        document.getElementById('pn-'+i).innerHTML = p.name + (i===nextIdx?' <span class="nx">NEXT</span>':'');
        var row = document.getElementById('prow-'+i);
        if (i===nextIdx) row.classList.add('next'); else row.classList.remove('next');
      }
    } else if (!prayersBuilt){
      document.getElementById('prayers').innerHTML = '<div class="hint">No prayer data yet. Waiting for first sync...</div>';
    }

    if (!mapInitialized && (s.lat !== 0 || s.lon !== 0)){
      map.setView([s.lat, s.lon], 10);
      setMarker(s.lat, s.lon, false);
      document.getElementById('offset-in').value = (s.offset/3600);
      updateOffsetDisplay();
      mapInitialized = true;
    }
  }).catch(function(){
    var b = document.getElementById('wifi-badge');
    b.className='badge bad';
    b.innerHTML='<span class="dot bad"></span>No response';
  });
}

updateOffsetDisplay();
refresh();
setInterval(refresh, 3000);
setTimeout(function(){ map.invalidateSize(); }, 400);
</script>
</body></html>
)=====";

void serveControlPage() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.send_P(200, "text/html", CONTROL_PAGE);
}

// =====================================================
// API Handlers
// =====================================================
void handlePlay() {
  if (!isAthanActive) {
    Serial.println("Web: Manual Play");
    mp3.restart();
    isAthanActive = true;
    statusGracePeriod = millis() + 3000;
    digitalWrite(READY_PIN, HIGH);
    athanOffTime = 0;
  }
  server.send(200, "text/plain", "Playing");
}

void handleStop() {
  if (isAthanActive) {
    Serial.println("Web: Manual Stop");
    mp3.pause();
    isAthanActive = false;
    digitalWrite(READY_PIN, LOW);
    athanOffTime = 0;
  }
  server.send(200, "text/plain", "Stopped");
}

void handleStatus() {
  server.send(200, "text/plain", isAthanActive ? "Playing" : "Idle");
}

void handleApiStatus() {
  time_t now = time(nullptr);
  struct tm *ti = localtime(&now);
  int nowMin = (ti->tm_hour * 60) + ti->tm_min;

  String lastSyncStr = "Never";
  if (lastSyncEpoch > 0) {
    struct tm *lt = localtime(&lastSyncEpoch);
    char buf[24];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
             lt->tm_hour, lt->tm_min, lt->tm_sec);
    lastSyncStr = String(buf);
  }

  String json = "{";
  json += "\"connected\":";
  json += (WiFi.status() == WL_CONNECTED) ? "true" : "false";
  json += ",\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\"";
  json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  json += ",\"rssi\":" + String(WiFi.RSSI());
  json += ",\"playing\":";
  json += isAthanActive ? "true" : "false";
  json += ",\"volume\":" + String(currentVolume);
  json += ",\"manual\":";
  json += isManual ? "true" : "false";
  json += ",\"lat\":" + String(myLat, 4);
  json += ",\"lon\":" + String(myLon, 4);
  json += ",\"offset\":" + String(currentOffset);
  json += ",\"method\":" + String(calcMethod);
  json += ",\"school\":" + String(asrSchool);
  json += ",\"latadj\":" + String(latAdjMethod);
  json += ",\"autotz\":" + String(autoTz ? 1 : 0);
  json += ",\"tune\":[";
  for (int i = 0; i < 5; i++) {
    if (i > 0) json += ",";
    json += String(tuneOffset[i]);
  }
  json += "]";
  json += ",\"fw\":\"" FW_VERSION "\"";
  json += ",\"ota\":\"" + jsonEscape(otaStatus) + "\"";
  json += ",\"lastSync\":\"" + lastSyncStr + "\"";
  json += ",\"syncStatus\":\"" + jsonEscape(lastSyncStatus) + "\"";
  json += ",\"nowMin\":" + String(nowMin);
  json += ",\"prayers\":[";
  for (int i = 0; i < 5; i++) {
    if (i > 0) json += ",";
    json += "{\"name\":\"" + String(prayerNames[i]) +
            "\",\"hour\":" + String(prayers[i].hour) +
            ",\"minute\":" + String(prayers[i].minute) + "}";
  }
  json += "]}";

  server.sendHeader("Cache-Control", "no-cache, no-store");
  server.send(200, "application/json", json);
}

void handleSetLocation() {
  if (!server.hasArg("lat") || !server.hasArg("lon")) {
    server.send(400, "text/plain", "Missing lat/lon");
    return;
  }
  float lat = server.arg("lat").toFloat();
  float lon = server.arg("lon").toFloat();
  long off = server.hasArg("offset") ? server.arg("offset").toInt() : currentOffset;

  if (lat < -90 || lat > 90 || lon < -180 || lon > 180) {
    server.send(400, "text/plain", "Invalid coordinates");
    return;
  }
  if (off < -12 * 3600 || off > 14 * 3600) {
    server.send(400, "text/plain", "Invalid offset");
    return;
  }

  myLat = lat;
  myLon = lon;
  currentOffset = off;
  isManual = true;
  saveLocationToEEPROM();
  configTime(currentOffset, 0, "pool.ntp.org", "time.nist.gov");
  Serial.printf("[Manual] Lat=%.4f Lon=%.4f Offset=%ld\n", myLat, myLon, currentOffset);

  lastSyncStatus = "Pending re-sync...";
  forceSync = true;
  server.send(200, "text/plain", "OK");
}

void handleSetAuto() {
  isManual = false;
  saveLocationToEEPROM();
  Serial.println("[Manual] Switched to AUTO");
  lastSyncStatus = "Pending re-sync...";
  forceSync = true;
  server.send(200, "text/plain", "OK");
}

void handleSetSettings() {
  if (server.hasArg("method")) {
    int m = server.arg("method").toInt();
    if (m >= 0 && m <= 23) calcMethod = (byte)m;
  }
  if (server.hasArg("school")) {
    int s = server.arg("school").toInt();
    if (s == 0 || s == 1) asrSchool = (byte)s;
  }
  if (server.hasArg("latadj")) {
    int la = server.arg("latadj").toInt();
    if (la >= 0 && la <= 3) latAdjMethod = (byte)la;
  }
  if (server.hasArg("autotz")) {
    autoTz = (server.arg("autotz").toInt() == 1);
  }
  saveLocationToEEPROM();
  Serial.printf("[Settings] method=%d school=%d latAdj=%d autoTz=%d\n",
                calcMethod, asrSchool, latAdjMethod, autoTz);
  lastSyncStatus = "Pending re-sync...";
  forceSync = true;
  server.send(200, "text/plain", "OK");
}

void handleSetTune() {
  for (int i = 0; i < 5; i++) {
    String k = "t" + String(i);
    if (server.hasArg(k)) {
      int v = server.arg(k).toInt();
      if (v < -1440) v = -1440;
      if (v >  1440) v =  1440;
      tuneOffset[i] = v;
    }
  }
  saveLocationToEEPROM();
  Serial.printf("[Tune] %d,%d,%d,%d,%d\n",
                tuneOffset[0], tuneOffset[1], tuneOffset[2], tuneOffset[3], tuneOffset[4]);
  lastSyncStatus = "Pending re-sync...";
  forceSync = true;
  server.send(200, "text/plain", "OK");
}

void handleManualSync() {
  lastSyncStatus = "Pending re-sync...";
  forceSync = true;
  server.send(200, "text/plain", "Sync queued");
}

void handleSetVolume() {
  if (!server.hasArg("vol")) {
    server.send(400, "text/plain", "Missing vol");
    return;
  }
  int ui = server.arg("vol").toInt();
  if (ui < 0)   ui = 0;
  if (ui > 100) ui = 100;
  // Map 0-100 (UI) -> 1-30 (JQ6500). Clamp min to 1 so the device is never silent
  // unintentionally (matches the button-driven behavior in the loop).
  int v = (ui * 30 + 50) / 100; // rounded
  if (v < 1)  v = 1;
  if (v > 30) v = 30;
  currentVolume = (byte)v;
  mp3.setVolume(currentVolume);
  EEPROM.write(EE_VOL, currentVolume);
  EEPROM.commit();
  Serial.printf("Web: Volume %d%% -> %d/30\n", ui, currentVolume);
  server.send(200, "text/plain", String(currentVolume));
}

void handleRestart() {
  server.send(200, "text/plain", "Restarting...");
  server.client().flush();
  delay(500);
  Serial.println("Web: Restart requested");
  ESP.restart();
}

void handleOtaCheck() {
  bool apply = server.hasArg("apply") && server.arg("apply") == "1";
  Serial.printf("[OTA] Web-triggered check (apply=%d)\n", apply);
  String res = runOtaCheck(apply);
  // On a successful install the device reboots inside runOtaCheck and never
  // reaches this line; the browser fetch just errors out and reloads.
  server.send(200, "text/plain", res);
}

// =====================================================
// Location & Prayer Sync
// =====================================================
bool getLocationFromIP() {
  WiFiClient client;
  HTTPClient http;
  Serial.print("[Loc] Fetching IP-based location... ");
  if (!http.begin(client, "http://ip-api.com/csv/?fields=status,lat,lon,offset")) {
    Serial.println("HTTP begin failed");
    return false;
  }
  http.setTimeout(8000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Failed code=%d\n", code);
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();
  payload.trim();
  Serial.println(payload);

  // Format: "status,lat,lon,offset"  e.g. "success,52.5167,13.3833,7200"
  int p1 = payload.indexOf(',');
  if (p1 < 0) return false;
  String status = payload.substring(0, p1);
  if (status != "success") {
    Serial.printf("[Loc] Lookup failed: %s\n", status.c_str());
    return false;
  }
  int p2 = payload.indexOf(',', p1 + 1);
  if (p2 < 0) return false;
  int p3 = payload.indexOf(',', p2 + 1);
  if (p3 < 0) return false;

  float newLat = payload.substring(p1 + 1, p2).toFloat();
  float newLon = payload.substring(p2 + 1, p3).toFloat();
  long newOffset = payload.substring(p3 + 1).toInt();

  if (newLat == 0.0 && newLon == 0.0) {
    Serial.println("[Loc] Zero coords rejected");
    return false;
  }

  bool moved = (abs(newLat - myLat) > 0.05 || abs(newLon - myLon) > 0.05 || newOffset != currentOffset);
  myLat = newLat;
  myLon = newLon;
  currentOffset = newOffset;
  Serial.printf("[Loc] Lat=%.4f Lon=%.4f Offset=%lds %s\n",
                myLat, myLon, currentOffset, moved ? "(CHANGED)" : "(same)");
  // Always (re-)apply TZ so localtime() reflects current offset
  configTime(currentOffset, 0, "pool.ntp.org", "time.nist.gov");
  return true;
}

bool fetchPrayerTimes() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (myLat == 0.0 && myLon == 0.0) {
    Serial.println("[Prayers] Skipping - no valid coords");
    return false;
  }

  WiFiClientSecure sec;
  sec.setInsecure();
  sec.setBufferSizes(512, 512);
  HTTPClient http;

  String url = "https://api.aladhan.com/v1/timings?latitude=" + String(myLat, 4) +
               "&longitude=" + String(myLon, 4) +
               "&method=" + String(calcMethod) +
               "&school=" + String(asrSchool) +
               "&iso8601=true";
  // latitudeAdjustmentMethod is only valid for 1..3; 0 = leave it to the API default
  if (latAdjMethod >= 1 && latAdjMethod <= 3)
    url += "&latitudeAdjustmentMethod=" + String(latAdjMethod);
  Serial.printf("[Prayers] GET %s ... ", url.c_str());

  if (!http.begin(sec, url)) {
    Serial.println("HTTP begin failed");
    return false;
  }
  http.setTimeout(10000);
  http.setUserAgent("ESP8266-MyAthan");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Failed code=%d err=%s\n", code, http.errorToString(code).c_str());
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();
  Serial.println("OK");

  int found = 0;
  apiOffsetValid = false;
  for (int i = 0; i < 5; i++) {
    String key = "\"" + String(prayerNames[i]) + "\":\"";
    int start = payload.indexOf(key);
    if (start < 0) continue;
    start += key.length();
    // With iso8601=true the value is e.g. "2026-06-27T05:15:00+02:00"
    String val = payload.substring(start, start + 25);
    int tpos = val.indexOf('T');
    if (tpos < 0 || tpos + 6 > (int)val.length()) continue;
    prayers[i].name = prayerNames[i];
    prayers[i].hour = val.substring(tpos + 1, tpos + 3).toInt();
    prayers[i].minute = val.substring(tpos + 4, tpos + 6).toInt();
    prayers[i].played = false;

    // Apply the user's per-prayer fine-tune offset (minutes, may be negative),
    // wrapping around midnight so the result is always a valid 00:00..23:59 time.
    int adj = prayers[i].hour * 60 + prayers[i].minute + tuneOffset[i];
    adj = ((adj % 1440) + 1440) % 1440;
    prayers[i].hour = adj / 60;
    prayers[i].minute = adj % 60;

    int total = adj - 10;
    if (total < 0) total += 1440;
    prayers[i].readyHour = total / 60;
    prayers[i].readyMinute = total % 60;

    // Extract the DST-correct UTC offset once from the trailing "+HH:MM" / "-HH:MM"
    if (!apiOffsetValid) {
      int sgn = -1;
      for (int j = tpos + 6; j < (int)val.length(); j++) {
        if (val[j] == '+' || val[j] == '-') { sgn = j; break; }
      }
      if (sgn > 0 && sgn + 5 < (int)val.length()) {
        long oh = val.substring(sgn + 1, sgn + 3).toInt();
        long om = val.substring(sgn + 4, sgn + 6).toInt();
        apiOffset = oh * 3600 + om * 60;
        if (val[sgn] == '-') apiOffset = -apiOffset;
        apiOffsetValid = true;
      }
    }
    Serial.printf("  %-8s: %02d:%02d  (Light: %02d:%02d)\n",
                  prayers[i].name.c_str(), prayers[i].hour, prayers[i].minute,
                  prayers[i].readyHour, prayers[i].readyMinute);
    found++;
  }

  // Keep the device clock aligned with the prayer location (handles DST and
  // manual-mode timezone mistakes) so the athan fires at the right wall-clock time.
  if (found == 5 && autoTz && apiOffsetValid && apiOffset != currentOffset) {
    Serial.printf("[Prayers] TZ offset %lds -> %lds (from API)\n", currentOffset, apiOffset);
    currentOffset = apiOffset;
    configTime(currentOffset, 0, "pool.ntp.org", "time.nist.gov");
    saveLocationToEEPROM();
  }

  Serial.printf("[Prayers] Parsed %d/5  Heap=%d\n", found, ESP.getFreeHeap());
  return found == 5;
}

// True if dotted-numeric version `remote` is strictly newer than `local` (e.g. "2.2.3" > "2.2.2").
bool isNewer(const String &remote, const char *local) {
  int rp = 0; const char *lp = local;
  while (rp < (int)remote.length() || *lp) {
    long rn = 0, ln = 0;
    while (rp < (int)remote.length() && remote[rp] != '.') { if (remote[rp] >= '0' && remote[rp] <= '9') rn = rn * 10 + (remote[rp] - '0'); rp++; }
    while (*lp && *lp != '.') { if (*lp >= '0' && *lp <= '9') ln = ln * 10 + (*lp - '0'); lp++; }
    if (rn != ln) return rn > ln;
    if (rp < (int)remote.length()) rp++;   // skip '.'
    if (*lp) lp++;
  }
  return false;
}

// =====================================================
// OTA - pull firmware from GitHub Releases
// =====================================================
String runOtaCheck(bool applyUpdate) {
  if (WiFi.status() != WL_CONNECTED) { otaStatus = "No WiFi"; return otaStatus; }

  WiFiClientSecure client;
  client.setInsecure();          // GitHub is HTTPS-only; we don't pin a cert
  client.setTimeout(15000);

  // 1) Read the published version string (small file)
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  http.setUserAgent("ESP8266-MyAthan");
  if (!http.begin(client, OTA_VERSION_URL)) { otaStatus = "OTA begin failed"; return otaStatus; }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    otaStatus = "No release (HTTP " + String(code) + ")";
    Serial.printf("[OTA] version check HTTP %d\n", code);
    return otaStatus;
  }
  String remote = http.getString();
  http.end();
  remote.trim();
  if (remote.length() == 0) { otaStatus = "Empty version file"; return otaStatus; }
  Serial.printf("[OTA] installed=%s latest=%s\n", FW_VERSION, remote.c_str());

  if (!isNewer(remote, FW_VERSION)) { otaStatus = "Up to date (" FW_VERSION ")"; return otaStatus; }
  if (!applyUpdate) { otaStatus = "Update available: " + remote; return otaStatus; }

  // 2) GitHub serves asset downloads as a 302 redirect to objects.githubusercontent.com.
  // ESPhttpUpdate does not follow that reliably (it reports "Wrong HTTP Code"), so we
  // resolve the real download URL ourselves and hand the resolved URL to the updater.
  String binUrl = OTA_BIN_URL;
  {
    HTTPClient rh;
    rh.begin(client, OTA_BIN_URL);
    rh.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    const char *hk[] = { "Location" };
    rh.collectHeaders(hk, 1);
    int rc = rh.GET();
    if (rc >= 300 && rc < 400) {
      String loc = rh.header("Location");
      if (loc.length()) binUrl = loc;
    }
    rh.end();
    Serial.printf("[OTA] asset HTTP %d -> %s\n", rc, binUrl.c_str());
  }

  // 3) Download + flash the resolved image. Reboots automatically on success.
  otaStatus = "Updating to " + remote + "...";
  Serial.printf("[OTA] downloading (heap=%d)\n", ESP.getFreeHeap());
  ESPhttpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return ret = ESPhttpUpdate.update(client, binUrl);
  if (ret == HTTP_UPDATE_FAILED) {
    otaStatus = "Update failed: " + ESPhttpUpdate.getLastErrorString();
    Serial.printf("[OTA] FAILED (%d) %s\n",
                  ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
  } else if (ret == HTTP_UPDATE_NO_UPDATES) {
    otaStatus = "No update";
  }
  return otaStatus;  // on HTTP_UPDATE_OK the device has already rebooted
}

void doFullSync() {
  // Record attempt time first so a failure cannot trigger a tight retry loop
  lastFullSync = millis();
  if (lastFullSync == 0) lastFullSync = 1;

  if (WiFi.status() != WL_CONNECTED) {
    lastSyncStatus = "No WiFi";
    return;
  }
  Serial.println("--- doFullSync() ---");
  lastSyncStatus = "Syncing...";

  bool locOk = true;
  if (!isManual) {
    locOk = getLocationFromIP();
    // Note: on failure, myLat/myLon retain previous (EEPROM-loaded) values,
    // so fetchPrayerTimes below still has usable coords.
  } else {
    configTime(currentOffset, 0, "pool.ntp.org", "time.nist.gov");
  }

  bool prayerOk = fetchPrayerTimes();

  if (prayerOk) {
    // Prayers parsed - this is the success criterion (location may have been a no-op in manual mode)
    lastSyncStatus = isManual ? "OK (Manual)" : (locOk ? "OK" : "OK (stale loc)");
    time_t now = time(nullptr);
    if (now > 1700000000) lastSyncEpoch = now;
    saveLocationToEEPROM();
    hasFirstSync = true;

    if (firstSync) {
      for (int j = 0; j < 3; j++) {
        digitalWrite(READY_PIN, HIGH); delay(100);
        digitalWrite(READY_PIN, LOW);  delay(100);
      }
      firstSync = false;
    }
  } else {
    lastSyncStatus = !locOk ? "Location + Prayer API failed" : "Prayer API failed";
    Serial.printf("[Sync] %s\n", lastSyncStatus.c_str());
  }
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(9600);
  mp3Serial.begin(9600);

  delay(1500);
  Serial.flush();
  Serial.println("\n\n--- MyAthan v2.1 ---");

  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setAutoReconnect(true);
  WiFi.hostname("MyAthan");
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  delay(100);

  pinMode(READY_PIN, OUTPUT);
  digitalWrite(READY_PIN, LOW);

  EEPROM.begin(EE_SIZE);
  currentVolume = EEPROM.read(EE_VOL);
  if (currentVolume > 30 || currentVolume < 1)
    currentVolume = 20;
  mp3.setVolume(currentVolume);

  loadLocationFromEEPROM();
  // Apply saved TZ immediately so the clock is correct from the start
  configTime(currentOffset, 0, "pool.ntp.org", "time.nist.gov");

  pinMode(12, INPUT_PULLUP);
  pinMode(13, INPUT_PULLUP);

  Serial.print("Connecting to WiFi");
  WiFi.begin();
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 15) {
    digitalWrite(READY_PIN, HIGH);
    delay(100);
    digitalWrite(READY_PIN, LOW);
    delay(400);
    Serial.print(".");
    retries++;
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    isAPMode = true;
    Serial.println("WiFi Failed. Starting SoftAP + Station...");
    WiFi.mode(WIFI_AP_STA);
    delay(100);
    IPAddress apIP(192, 168, 4, 1);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(AP_SSID, NULL, 1, 0, 4);

    Serial.print("Access Point Started: ");
    Serial.println(AP_SSID);

    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", apIP);

    server.on("/", handleRoot);
    server.on("/save", handleSave);
    server.on("/generate_204", handleRoot);
    server.on("/ncsi.txt", handleRoot);
    server.on("/connecttest.txt", handleRoot);
    server.on("/success.txt", handleRoot);
    server.on("/redirect", handleRoot);
    server.on("/hotspot-detect.html", handleRoot);
    server.onNotFound(handleNotFound);
    server.begin();
    MDNS.begin("myathan");
  } else {
    Serial.print("Connected! IP: ");
    Serial.println(WiFi.localIP());
    digitalWrite(READY_PIN, HIGH);
    delay(2000);
    digitalWrite(READY_PIN, LOW);

    lastSeenSSID = WiFi.SSID();

    server.on("/", serveControlPage);
    server.on("/play", handlePlay);
    server.on("/stop", handleStop);
    server.on("/status", handleStatus);
    server.on("/api/status", handleApiStatus);
    server.on("/api/location", HTTP_POST, handleSetLocation);
    server.on("/api/auto", HTTP_POST, handleSetAuto);
    server.on("/api/settings", HTTP_POST, handleSetSettings);
    server.on("/api/tune", HTTP_POST, handleSetTune);
    server.on("/api/sync", HTTP_POST, handleManualSync);
    server.on("/api/volume", HTTP_POST, handleSetVolume);
    server.on("/api/restart", HTTP_POST, handleRestart);
    server.on("/api/ota", HTTP_POST, handleOtaCheck);
    server.begin();
    if (MDNS.begin("myathan")) {
      MDNS.addService("http", "tcp", 80);
      Serial.println("mDNS: http://myathan.local");
    }
  }
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  if (isAPMode) {
    dnsServer.processNextRequest();
    server.handleClient();

    static unsigned long lastBlink = 0;
    if (millis() - lastBlink > 200) {
      lastBlink = millis();
      digitalWrite(READY_PIN, !digitalRead(READY_PIN));
    }
  } else {
    server.handleClient();
    MDNS.update();
  }

  time_t now = time(nullptr);
  struct tm *timeinfo = localtime(&now);

  // === SYNC ORCHESTRATION (STA mode) ===
  if (!isAPMode && WiFi.status() == WL_CONNECTED && now > 1700000000) {
    bool timeIsValid = (timeinfo->tm_year > 100);

    // Detect WiFi network change -> very likely a location change
    String currentSSID = WiFi.SSID();
    if (currentSSID.length() > 0 && lastSeenSSID.length() > 0 &&
        currentSSID != lastSeenSSID) {
      Serial.printf("[WiFi] Network changed: %s -> %s\n",
                    lastSeenSSID.c_str(), currentSSID.c_str());
      lastSeenSSID = currentSSID;
      if (!isManual) forceSync = true;
    } else if (currentSSID.length() > 0 && lastSeenSSID.length() == 0) {
      lastSeenSSID = currentSSID;
    }

    // Sync schedule:
    //   forceSync flag           -> immediate
    //   no successful sync yet   -> retry every 30s
    //   already synced           -> refresh every 4 hours
    if (timeIsValid) {
      unsigned long sinceLast = millis() - lastFullSync;
      bool needsSync = forceSync ||
                       (lastFullSync == 0) ||
                       (!hasFirstSync && sinceLast > 30UL * 1000UL) ||
                       ( hasFirstSync && sinceLast > 4UL * 3600UL * 1000UL);
      if (needsSync) {
        forceSync = false;
        doFullSync();
      }
    }
  }

  // === DAILY PRAYER REFRESH ===
  static int lastDay = -1;
  if (timeinfo->tm_year > 100) {
    if (timeinfo->tm_mday != lastDay) {
      if (lastDay != -1) {
        // Reset triggers regardless of WiFi - yesterday's prayer times are a
        // better fallback than not firing at all if WiFi happens to be down.
        Serial.println("--- New Day ---");
        for (int i = 0; i < 5; i++) prayers[i].played = false;
        if (WiFi.status() == WL_CONNECTED && hasFirstSync) {
          if (fetchPrayerTimes()) {
            time_t n2 = time(nullptr);
            if (n2 > 1700000000) lastSyncEpoch = n2;
          }
        }
      }
      lastDay = timeinfo->tm_mday;
    }
  }

  // === MINUTE-BY-MINUTE TRIGGERS ===
  static int lastCheckMin = -1;
  if (timeinfo->tm_min != lastCheckMin && timeinfo->tm_year > 100) {
    lastCheckMin = timeinfo->tm_min;

    for (int i = 0; i < 5; i++) {
      if (timeinfo->tm_hour == prayers[i].readyHour &&
          timeinfo->tm_min == prayers[i].readyMinute) {
        Serial.println("Trigger: PRE-ATHAN ACTIVE");
        digitalWrite(READY_PIN, HIGH);
      }
      if (timeinfo->tm_hour == prayers[i].hour &&
          timeinfo->tm_min == prayers[i].minute && !prayers[i].played) {
        Serial.printf("Trigger: PLAYING %s\n", prayers[i].name.c_str());
        mp3.restart();
        isAthanActive = true;
        statusGracePeriod = millis() + 3000;
        prayers[i].played = true;
        digitalWrite(READY_PIN, HIGH);
      }
    }
  }

  // === DAILY AUTO OTA CHECK (00:30 - offset from the 00:00 prayer refresh) ===
  // Runs once per day, only when idle (no athan playing) and online. Installs +
  // reboots automatically if a newer firmware has been published to GitHub.
  static int lastOtaCheckDay = -1;
  if (timeinfo->tm_year > 100 && WiFi.status() == WL_CONNECTED && hasFirstSync &&
      !isAthanActive &&
      timeinfo->tm_hour == OTA_CHECK_HOUR && timeinfo->tm_min == OTA_CHECK_MIN &&
      timeinfo->tm_mday != lastOtaCheckDay) {
    lastOtaCheckDay = timeinfo->tm_mday;  // set first so a failed update won't retry-loop
    Serial.println("[OTA] Daily auto-check");
    runOtaCheck(true);
  }

  // === ATHAN DEACTIVATION ===
  if (isAthanActive && millis() > statusGracePeriod) {
    static unsigned long lastStat = 0;
    if (millis() - lastStat > 1000) {
      lastStat = millis();
      if (mp3.getStatus() != 1) {
        Serial.println("Athan Finished. Scheduling D8 OFF in 5s.");
        isAthanActive = false;
        athanOffTime = millis() + 5000;
      }
    }
  }
  if (athanOffTime > 0 && millis() > athanOffTime) {
    digitalWrite(READY_PIN, LOW);
    athanOffTime = 0;
    Serial.println("D8 deactivated.");
  }

  // === Volume & Reset Controls (unchanged state machine) ===
  bool up = (digitalRead(12) == LOW); // D6
  bool dn = (digitalRead(13) == LOW); // D7

  static uint32_t pressStart = 0;
  static bool dualFlag = false;
  static bool volFired = false;
  static uint32_t lastRepeat = 0;
  static int activeBtn = 0;
  static bool needsCommit = false;

  if (up || dn) {
    if (pressStart == 0) {
      pressStart = millis();
      dualFlag = false;
      volFired = false;
      activeBtn = up ? 1 : 2;
    }
    if (up && dn) dualFlag = true;

    if (dualFlag) {
      if (millis() - pressStart > 5000) {
        Serial.println("!!! NUCLEAR RESET: WIPING WIFI & CONFIG !!!");
        WiFi.disconnect(true);
        ESP.eraseConfig();
        for (int i = 0; i < EE_SIZE; i++) EEPROM.write(i, 255);
        EEPROM.commit();
        delay(500);
        ESP.restart();
      }
    } else {
      uint32_t held = millis() - pressStart;
      bool shouldChange = false;
      if (held > 200 && !volFired) { shouldChange = true; volFired = true; }
      if (held > 600 && millis() - lastRepeat > 150) shouldChange = true;

      if (shouldChange) {
        if (activeBtn == 1) { if (currentVolume < 30) currentVolume++; }
        else                { if (currentVolume > 1)  currentVolume--; }
        mp3.setVolume(currentVolume);
        needsCommit = true;
        lastRepeat = millis();
        Serial.printf("Vol: %d\n", currentVolume);
      }
    }
  } else {
    if (pressStart > 0) {
      uint32_t held = millis() - pressStart;
      if (dualFlag) {
        if (held > 50 && held < 3000 && !volFired) {
          if (isAthanActive) {
            Serial.println("Manual Stop");
            mp3.pause();
            isAthanActive = false;
            digitalWrite(READY_PIN, LOW);
            athanOffTime = 0;
          } else {
            Serial.println("Manual Play");
            mp3.restart();
            isAthanActive = true;
            statusGracePeriod = millis() + 3000;
            digitalWrite(READY_PIN, HIGH);
            athanOffTime = 0;
          }
        }
      } else {
        if (held > 50 && !volFired) {
          if (activeBtn == 1) { if (currentVolume < 30) currentVolume++; }
          else                { if (currentVolume > 1)  currentVolume--; }
          mp3.setVolume(currentVolume);
          needsCommit = true;
          Serial.printf("Vol: %d (Click)\n", currentVolume);
        }
      }
      if (needsCommit) {
        EEPROM.write(EE_VOL, currentVolume);
        if (EEPROM.commit())
          Serial.println("Volume Saved to EEPROM.");
        else
          Serial.println("EEPROM Commit Failed!");
        needsCommit = false;
      }
      pressStart = 0;
    }
  }

  // === PERSISTENT Reconnect Check (Every 60s) ===
  static unsigned long lastRecon = 0;
  if (millis() - lastRecon > 60000) {
    lastRecon = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Connection Lost/Not Found. Retrying...");
      WiFi.begin();
    }
  }
}
