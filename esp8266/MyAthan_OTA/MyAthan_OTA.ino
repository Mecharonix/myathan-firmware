/** MyAthan v2.2 - Location & UI Refresh + GitHub OTA
 *
 * WIRING (unchanged):
 * - MP3 Module TX      ->  ESP8266 D1 (GPIO 5)
 * - MP3 Module RX      ->  ESP8266 D2 (GPIO 4)
 * - LED / Relay (+)    ->  ESP8266 D8 (GPIO 15)
 * - Volume UP Button   ->  ESP8266 D6 (GPIO 12)
 * - Volume DOWN Button ->  ESP8266 D7 (GPIO 13)
 *
 * Build FQBN (must match USB flasher / 4M2M devices):
 *   esp8266:esp8266:nodemcuv2:eesz=4M2M,ssl=all
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
#include <user_interface.h>
#include "web_pages_gz.h"

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
#define FW_VERSION "2.2.10"
// 2.2.8 bridge: poll GitHub often so a follow-up 2.2.9 can land the same day.
// 2.2.9 should restore OTA_CHECK_HOUR/MIN (00:30) and drop this interval.
#define OTA_CHECK_INTERVAL_MS (15UL * 60UL * 1000UL)
#define OTA_MIN_HEAP 14000
// USB-flash the one test device with this set to 1. Every other unit still
// reads esp8266-latest (currently 2.2.7) and will not see esp8266-test.
// MUST be 0 before any fleet push of 2.2.8.
#define OTA_USE_TEST_CHANNEL 0
#if OTA_USE_TEST_CHANNEL
#define OTA_CHANNEL "esp8266-test"
#else
#define OTA_CHANNEL "esp8266-latest"
#endif
const char *OTA_VERSION_URL =
    "https://github.com/Mecharonix/myathan-firmware/releases/download/" OTA_CHANNEL "/version.txt";
const char *OTA_BIN_URL =
    "https://github.com/Mecharonix/myathan-firmware/releases/download/" OTA_CHANNEL "/firmware.bin";
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

// STA-only first so the radio can scan every channel. Then open the portal and
// keep retrying saved creds without a blocking WiFi scan while a phone is on
// the AP (scanNetworks drops SoftAP clients and kills Android's captive portal).
#define WIFI_PORTAL_DELAY_MS   45000
#define WIFI_RETRY_INTERVAL_MS 20000

// --- State ---
bool isAPMode = false;
bool hasSavedWifi = false;
char savedSsid[33];
char savedPass[65];
unsigned long lastWifiRetry = 0;
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

void sendGzipPage(const uint8_t *data, size_t len) {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, "text/html", (PGM_P)data, len);
}
void serveSetupPage() { sendGzipPage(SETUP_HTML_GZ, SETUP_HTML_GZ_LEN); }
void serveControlPage() { sendGzipPage(CONTROL_HTML_GZ, CONTROL_HTML_GZ_LEN); }

boolean handleCaptivePortal() {
  String host = server.hostHeader();
  host.toLowerCase();
  String uri = server.uri();
  uri.toLowerCase();

  bool setupHost = isIp(host) || host.startsWith("192.168.4.1") ||
                   host == "myathan.local";
  bool probe = uri.indexOf("generate_204") != -1 || uri.indexOf("gen_204") != -1 ||
               uri.indexOf("connecttest") != -1 || uri.indexOf("ncsi") != -1 ||
               uri.indexOf("success.txt") != -1 || uri.indexOf("hotspot-detect") != -1 ||
               uri.indexOf("canonical") != -1 || uri.indexOf("fwlink") != -1 ||
               uri.indexOf("redirect") != -1 || uri.indexOf("wifistub") != -1 ||
               uri.indexOf("wpad") != -1;

  if (setupHost && uri == "/") return false;

  if (!setupHost || probe) {
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
    return true;
  }
  return false;
}

void handleRoot() {
  // After a late STA connect we stay on the same HTTP server; / was registered
  // for the portal first, so dispatch here instead of adding a second handler.
  if (!isAPMode) {
    serveControlPage();
    return;
  }
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
// API Handlers
// =====================================================
void applySavedVolume() {
  if (currentVolume < 1) currentVolume = 1;
  if (currentVolume > 30) currentVolume = 30;
  mp3.setVolume(currentVolume);
}

// Play track 1 without asking the JQ6500 what volume it thinks it has.
// Library restart() does getVolume(); that serial reply is often 0/garbage, so
// each athan would mute and restore a lower level ("gets quieter every day").
void startAthan() {
  applySavedVolume();
  mp3.next();
  mp3.pause();
  applySavedVolume();
  mp3.prev();
}

void handlePlay() {
  if (!isAthanActive) {
    Serial.println("Web: Manual Play");
    startAthan();
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
  json += ",\"otaChannel\":\"" OTA_CHANNEL "\"";
  json += ",\"ota\":\"" + jsonEscape(otaStatus) + "\"";
  json += ",\"sketch\":" + String(ESP.getSketchSize());
  json += ",\"freeOta\":" + String(ESP.getFreeSketchSpace());
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
  if (isAPMode || WiFi.status() != WL_CONNECTED) { otaStatus = "No WiFi"; return otaStatus; }
  if (isAthanActive) { otaStatus = "Busy (athan)"; return otaStatus; }
  if (ESP.getFreeHeap() < (int)OTA_MIN_HEAP) {
    otaStatus = "Low heap, retry later";
    Serial.printf("[OTA] skip heap=%d\n", ESP.getFreeHeap());
    return otaStatus;
  }
  Serial.printf("[OTA] sketch=%u free=%u heap=%d apply=%d\n",
                ESP.getSketchSize(), ESP.getFreeSketchSpace(),
                ESP.getFreeHeap(), applyUpdate);

  // Each HTTPS step runs in its own scope so its ~16KB BearSSL buffer is released
  // before the next one - the ESP8266 has only ~24KB free, not enough for two at once.

  // 1) Read the published version string.
  String remote;
  {
    WiFiClientSecure vc;
    vc.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(15000);
    http.setUserAgent("ESP8266-MyAthan");
    if (!http.begin(vc, OTA_VERSION_URL)) { otaStatus = "OTA begin failed"; return otaStatus; }
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
      http.end();
      otaStatus = "No release (HTTP " + String(code) + ")";
      Serial.printf("[OTA] version check HTTP %d\n", code);
      return otaStatus;
    }
    remote = http.getString();
    http.end();
  }
  remote.trim();
  if (remote.length() == 0) { otaStatus = "Empty version file"; return otaStatus; }
  Serial.printf("[OTA] installed=%s latest=%s\n", FW_VERSION, remote.c_str());

  if (!isNewer(remote, FW_VERSION)) { otaStatus = "Up to date (" FW_VERSION ")"; return otaStatus; }
  if (!applyUpdate) { otaStatus = "Update available: " + remote; return otaStatus; }

  // 2) GitHub redirects asset downloads (github.com -> objects.githubusercontent.com).
  // The updater path doesn't follow that, so resolve the real URL ourselves. Own scope
  // (and freed buffer) gives enough heap to actually capture the long Location header.
  String binUrl;
  {
    WiFiClientSecure rc;
    rc.setInsecure();
    HTTPClient rh;
    rh.setTimeout(15000);
    rh.setUserAgent("ESP8266-MyAthan");
    rh.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    const char *hk[] = { "Location" };
    rh.begin(rc, OTA_BIN_URL);
    rh.collectHeaders(hk, 1);
    int code = rh.GET();
    if (code >= 300 && code < 400) binUrl = rh.header("Location");
    rh.end();
    Serial.printf("[OTA] resolve HTTP %d  loc_len=%d  heap=%d\n", code, binUrl.length(), ESP.getFreeHeap());
  }
  if (binUrl.length() == 0) { otaStatus = "Update failed: redirect not resolved"; return otaStatus; }

  // 3) Download + flash from the resolved direct URL (returns 200, no redirect).
  otaStatus = "Updating to " + remote + "...";
  Serial.printf("[OTA] downloading (heap=%d)\n", ESP.getFreeHeap());
  WiFiClientSecure dc;
  dc.setInsecure();
  ESPhttpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return ret = ESPhttpUpdate.update(dc, binUrl);
  if (ret == HTTP_UPDATE_FAILED) {
    otaStatus = "Update failed: " + ESPhttpUpdate.getLastErrorString();
    Serial.printf("[OTA] FAILED (%d) %s sketch=%u free=%u heap=%d\n",
                  ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str(),
                  ESP.getSketchSize(), ESP.getFreeSketchSpace(), ESP.getFreeHeap());
    yield();
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

void registerControlRoutes() {
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
}

void advertiseMdns() {
  MDNS.end();
  if (MDNS.begin("myathan")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("[mDNS] http://myathan.local");
  } else {
    Serial.println("[mDNS] begin failed");
  }
}

void rememberSavedWifi() {
  strncpy(savedSsid, WiFi.SSID().c_str(), sizeof(savedSsid) - 1);
  savedSsid[sizeof(savedSsid) - 1] = 0;
  strncpy(savedPass, WiFi.psk().c_str(), sizeof(savedPass) - 1);
  savedPass[sizeof(savedPass) - 1] = 0;
  if (savedSsid[0] == 0) {
    struct station_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    wifi_station_get_config_default(&cfg);
    strncpy(savedSsid, reinterpret_cast<char *>(cfg.ssid), sizeof(savedSsid) - 1);
    strncpy(savedPass, reinterpret_cast<char *>(cfg.password), sizeof(savedPass) - 1);
    savedSsid[sizeof(savedSsid) - 1] = 0;
    savedPass[sizeof(savedPass) - 1] = 0;
  }
  hasSavedWifi = savedSsid[0] != 0;
  if (hasSavedWifi)
    Serial.printf("[WiFi] Saved SSID %s\n", savedSsid);
}

int scanChannelForSavedWifi() {
  if (!hasSavedWifi) return 1;
  int ch = 1;
  int n = WiFi.scanNetworks(false, true);
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == savedSsid) {
      ch = WiFi.channel(i);
      break;
    }
  }
  WiFi.scanDelete();
  return ch;
}

void retrySavedWifi() {
  if (!hasSavedWifi || WiFi.status() == WL_CONNECTED) return;
  WiFi.persistent(false);
  // A blocking scan here knocks phones off MyAthan Setup, so Android never
  // opens the login page. Only retune the AP channel when nobody is associated.
  if (WiFi.softAPgetStationNum() == 0) {
    int ch = scanChannelForSavedWifi();
    Serial.printf("[WiFi] Retry %s on ch %d\n", savedSsid, ch);
    WiFi.softAP(AP_SSID, NULL, ch, 0, 4);
  } else {
    Serial.printf("[WiFi] Retry %s (phone on portal)\n", savedSsid);
  }
  WiFi.begin(savedSsid, savedPass);
}

void leavePortalForSavedWifi() {
  Serial.print("[WiFi] Saved network is up. IP ");
  Serial.println(WiFi.localIP());
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  isAPMode = false;
  digitalWrite(READY_PIN, LOW);
  lastSeenSSID = WiFi.SSID();
  registerControlRoutes();
  advertiseMdns();
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(9600);
  mp3Serial.begin(9600);

  delay(1500);
  Serial.flush();
  Serial.printf("\n\n--- MyAthan v%s ---\n", FW_VERSION);

  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setAutoReconnect(false);
  WiFi.hostname("myathan");
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  delay(100);

  pinMode(READY_PIN, OUTPUT);
  digitalWrite(READY_PIN, LOW);

  EEPROM.begin(EE_SIZE);
  currentVolume = EEPROM.read(EE_VOL);
  if (currentVolume > 30 || currentVolume < 1)
    currentVolume = 20;
  applySavedVolume();

  loadLocationFromEEPROM();
  // Apply saved TZ immediately so the clock is correct from the start
  configTime(currentOffset, 0, "pool.ntp.org", "time.nist.gov");

  pinMode(12, INPUT_PULLUP);
  pinMode(13, INPUT_PULLUP);

  Serial.print("Connecting to WiFi");
  WiFi.persistent(true);
  WiFi.begin();
  unsigned long waitStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - waitStart < WIFI_PORTAL_DELAY_MS) {
    digitalWrite(READY_PIN, HIGH);
    delay(100);
    digitalWrite(READY_PIN, LOW);
    delay(400);
    Serial.print(".");
  }
  Serial.println();

  rememberSavedWifi();

  if (WiFi.status() != WL_CONNECTED) {
    isAPMode = true;
    Serial.println("WiFi Failed. Starting SoftAP + Station...");
    int ch = scanChannelForSavedWifi();
    WiFi.mode(WIFI_AP_STA);
    delay(100);
    IPAddress apIP(192, 168, 4, 1);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(AP_SSID, NULL, ch, 0, 4);

    Serial.printf("Access Point Started: %s ch %d\n", AP_SSID, ch);

    if (hasSavedWifi) {
      WiFi.persistent(false);
      WiFi.begin(savedSsid, savedPass);
      lastWifiRetry = millis();
    }

    dnsServer.setTTL(30);
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", apIP);

    server.on("/", handleRoot);
    server.on("/save", handleSave);
    server.on("/generate_204", handleRoot);
    server.on("/gen_204", handleRoot);
    server.on("/ncsi.txt", handleRoot);
    server.on("/connecttest.txt", handleRoot);
    server.on("/success.txt", handleRoot);
    server.on("/redirect", handleRoot);
    server.on("/hotspot-detect.html", handleRoot);
    server.on("/canonical.html", handleRoot);
    server.on("/fwlink", handleRoot);
    server.onNotFound(handleNotFound);
    server.begin();
    advertiseMdns();
  } else {
    Serial.print("Connected! IP: ");
    Serial.println(WiFi.localIP());
    digitalWrite(READY_PIN, HIGH);
    delay(2000);
    digitalWrite(READY_PIN, LOW);

    lastSeenSSID = WiFi.SSID();
    WiFi.setAutoReconnect(true);

    server.on("/", serveControlPage);
    registerControlRoutes();
    server.begin();
    advertiseMdns();
  }
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  MDNS.update();
  if (isAPMode) {
    dnsServer.processNextRequest();
    server.handleClient();

    if (hasSavedWifi) {
      if (WiFi.status() == WL_CONNECTED) {
        leavePortalForSavedWifi();
      } else if (millis() - lastWifiRetry >= WIFI_RETRY_INTERVAL_MS) {
        lastWifiRetry = millis();
        retrySavedWifi();
      }
    }

    static unsigned long lastBlink = 0;
    if (isAPMode && millis() - lastBlink > 200) {
      lastBlink = millis();
      digitalWrite(READY_PIN, !digitalRead(READY_PIN));
    }
  } else {
    server.handleClient();
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
        startAthan();
        isAthanActive = true;
        statusGracePeriod = millis() + 3000;
        prayers[i].played = true;
        digitalWrite(READY_PIN, HIGH);
      }
    }
  }

  // === 2.2.8 BRIDGE: OTA every 15 minutes (2.2.9 restores daily 00:30) ===
  static unsigned long lastOtaPoll = 0;
  if (!isAPMode && WiFi.status() == WL_CONNECTED && !isAthanActive &&
      millis() - lastOtaPoll >= OTA_CHECK_INTERVAL_MS) {
    lastOtaPoll = millis();  // set first so a failed update won't retry-loop
    Serial.println("[OTA] Periodic auto-check");
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
        applySavedVolume();
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
            startAthan();
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

}
