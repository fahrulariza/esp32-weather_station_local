#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP085.h>
#include <RTClib.h>
#include <WiFiUdp.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include <Preferences.h>
#include <Update.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <esp_arduino_version.h>
#include <stdarg.h>

// --- VERSI FIRMWARE ---
#define FIRMWARE_VERSION     "1.1.4"
#define FIRMWARE_VERSION_CODE 114

const char* GITHUB_UPDATE_URL = "https://raw.githubusercontent.com/fahrulariza/esp32-weather_station_local/refs/heads/update/update.json";

// --- PIN ---
#define SDA_PIN       6
#define SCL_PIN       5
#define LED_MERAH_PIN 7
#define LED_BIRU_PIN  8

// --- WATCHDOG ---
#define WDT_TIMEOUT_SEC 60

// --- WIFI ---
#define WIFI_CHECK_INTERVAL_MS          30000UL
#define WIFI_RECONNECT_WAIT_MS          10000UL
#define WIFI_MAX_ATTEMPTS_BEFORE_SETUP  3

// --- LOG BUFFER ---
#define LOG_BUFFER_LINES   30
#define LOG_LINE_MAX_LEN   200

// --- KONFIGURASI DINAMIS ---
String deviceName;
String wifiSsid1, wifiPass1;
String wifiSsid2, wifiPass2;
String apSsid, apPass;
String configUser, configPass;
String ntpServerLocal, ntpServerInet1, ntpServerInet2;
bool enableRedLed = true;
bool verboseDebug = false;

const long  GMT_OFFSET_SEC      = 7 * 3600;
const int   DAYLIGHT_OFFSET_SEC = 0;

// [v1.1.4] Resync NTP dari 12 jam → 6 jam
const unsigned long RESYNC_INTERVAL_MS       = 6UL * 3600UL * 1000UL;
const unsigned long CHECK_UPDATE_INTERVAL_MS = 6UL * 3600UL * 1000UL;
unsigned long lastNTPResync  = 0;
unsigned long lastUpdateCheck = 0;

unsigned long lastWiFiCheck = 0;
int wifiReconnectAttempts = 0;
unsigned long totalWiFiReconnects = 0;

bool isUpdateAvailable = false;
String newVersionStr = "";
int newVersionCode = 0;
String newBinUrl = "";
String changelogUrl = "";
String updateCheckStatusMsg = "";
String lastUpdateError = "";
bool isSystemUpdatingOrRebooting = false;

WebServer server(80);
WiFiUDP udpServer;
const unsigned int NTP_PORT = 123;

Adafruit_AHTX0 aht;
Adafruit_BMP085 bmp;
RTC_DS1307 rtc;
Preferences preferences;

bool ahtOk = false;
bool bmpOk = false;
bool rtcOk = false;
bool isApMode = false;

// [v1.1.4] NTP Reference Tracking
uint32_t lastSyncEpochNTP = 0;
unsigned long lastSyncMillis = 0;
bool hasSyncedWithUpstream = false;

// ===================== LOG LEVEL SYSTEM =====================
enum LogLevel { LVL_DEBUG = 0, LVL_INFO = 1, LVL_WARN = 2, LVL_ERROR = 3 };
const char* levelStr[] = { "DBG", "INFO", "WARN", "ERR" };

char logBuf[LOG_BUFFER_LINES][LOG_LINE_MAX_LEN + 1];
unsigned long logSeq[LOG_BUFFER_LINES];
int logHead = 0;
int logSize = 0;
unsigned long logSeqCounter = 0;

void rawAppend(const char* line) {
  int len = strlen(line);
  if (len > LOG_LINE_MAX_LEN) len = LOG_LINE_MAX_LEN;
  memcpy(logBuf[logHead], line, len);
  logBuf[logHead][len] = '\0';
  logSeq[logHead] = ++logSeqCounter;
  logHead = (logHead + 1) % LOG_BUFFER_LINES;
  if (logSize < LOG_BUFFER_LINES) logSize++;
}

void getTimeStamp(char* buf, size_t sz) {
  DateTime now = rtcOk ? rtc.now() : DateTime(0UL);
  if (now.unixtime() > 100000) {
    snprintf(buf, sz, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  } else {
    unsigned long s = millis() / 1000;
    snprintf(buf, sz, "%02lu:%02lu:%02lu", s / 3600, (s % 3600) / 60, s % 60);
  }
}

class LogSerialClass : public Print {
public:
  char currentLine[LOG_LINE_MAX_LEN + 80];
  int curLen = 0;

  size_t write(uint8_t c) override {
    Serial.write(c);
    if (c == '\n') {
      currentLine[curLen] = '\0';
      rawAppend(currentLine);
      curLen = 0;
    } else if (c != '\r') {
      if (curLen < (int)sizeof(currentLine) - 1) currentLine[curLen++] = c;
    }
    return 1;
  }

  size_t write(const uint8_t *buffer, size_t size) override {
    Serial.write(buffer, size);
    for (size_t i = 0; i < size; i++) {
      char c = (char)buffer[i];
      if (c == '\n') {
        currentLine[curLen] = '\0';
        rawAppend(currentLine);
        curLen = 0;
      } else if (c != '\r') {
        if (curLen < (int)sizeof(currentLine) - 1) currentLine[curLen++] = c;
      }
    }
    return size;
  }

  void begin(unsigned long baud) { Serial.begin(baud); }
  void flush() { Serial.flush(); }

  size_t printf(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    size_t len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    return write((const uint8_t*)buf, len);
  }

  void logLine(LogLevel level, const char* fmt, ...) {
    char ts[10];
    getTimeStamp(ts, sizeof(ts));

    char msgBuf[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
    va_end(args);

    char full[LOG_LINE_MAX_LEN + 24];
    snprintf(full, sizeof(full), "[%s][%s] %s", ts, levelStr[level], msgBuf);

    Serial.println(full);
    rawAppend(full);
  }
};

LogSerialClass LogSerial;

#define LOGD(fmt, ...) do { if (verboseDebug) LogSerial.logLine(LVL_DEBUG, fmt, ##__VA_ARGS__); } while(0)
#define LOGI(fmt, ...) LogSerial.logLine(LVL_INFO,  fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) LogSerial.logLine(LVL_WARN,  fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) LogSerial.logLine(LVL_ERROR, fmt, ##__VA_ARGS__)

// ===================== WATCHDOG =====================
void wdtInit() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  esp_err_t err = esp_task_wdt_reconfigure(&wdt_config);
  if (err != ESP_OK) esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);
  LOGI("Watchdog aktif: %d detik (Core 3.x)", WDT_TIMEOUT_SEC);
#else
  esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
  esp_task_wdt_add(NULL);
  LOGI("Watchdog aktif: %d detik (Core 2.x)", WDT_TIMEOUT_SEC);
#endif
}

void wdtSuspend() { esp_task_wdt_delete(NULL); }
void wdtResume()  { esp_task_wdt_add(NULL); esp_task_wdt_reset(); }
inline void wdtFeed() { esp_task_wdt_reset(); }

// ===================== LED =====================
inline void ledBiruOn()  { digitalWrite(LED_BIRU_PIN, LOW); }
inline void ledBiruOff() { digitalWrite(LED_BIRU_PIN, HIGH); }

void blinkBlueLedFast(int times = 10, int speedMs = 30) {
  for (int i = 0; i < times; i++) {
    ledBiruOn();  delay(speedMs);
    ledBiruOff(); delay(speedMs);
    wdtFeed();
  }
}

class DataActivityPulse {
public:
  DataActivityPulse()  { if (!isSystemUpdatingOrRebooting) ledBiruOn(); }
  ~DataActivityPulse() {
    if (!isSystemUpdatingOrRebooting) { delay(15); ledBiruOff(); }
  }
};

// ===================== HTTP REQUEST LOGGER =====================
void logHttpRequest(const char* handlerName) {
  if (!verboseDebug) return;
  IPAddress ip = server.client().remoteIP();
  String method = (server.method() == HTTP_GET)  ? "GET"  :
                  (server.method() == HTTP_POST) ? "POST" : "OTHER";
  String uri = server.uri();
  LOGD("HTTP %s %s -> %s from %s",
       method.c_str(), uri.c_str(), handlerName, ip.toString().c_str());
}

// ===================== ERROR OTA =====================
String getOTAErrorMessage(int httpErrCode, uint8_t updateErrCode = 0) {
  if (updateErrCode > 0) {
    switch (updateErrCode) {
      case UPDATE_ERROR_WRITE:        return "Gagal menulis ke Flash.";
      case UPDATE_ERROR_ERASE:        return "Gagal mengosongkan Flash.";
      case UPDATE_ERROR_READ:         return "Gagal membaca Flash.";
      case UPDATE_ERROR_SPACE:        return "Ruang Flash tidak cukup.";
      case UPDATE_ERROR_SIZE:         return "Ukuran file tidak valid.";
      case UPDATE_ERROR_STREAM:       return "Stream terputus.";
      case UPDATE_ERROR_MD5:          return "MD5 gagal.";
      case UPDATE_ERROR_MAGIC_BYTE:   return "Bukan firmware valid.";
      case UPDATE_ERROR_ACTIVATE:     return "Gagal aktifkan partisi.";
      case UPDATE_ERROR_NO_PARTITION: return "Partisi OTA tidak ada.";
      default:                        return "Error #" + String(updateErrCode);
    }
  }
  switch (httpErrCode) {
    case HTTPC_ERROR_CONNECTION_REFUSED:  return "Koneksi ditolak.";
    case HTTPC_ERROR_SEND_HEADER_FAILED:
    case HTTPC_ERROR_SEND_PAYLOAD_FAILED: return "Gagal kirim.";
    case HTTPC_ERROR_NOT_CONNECTED:       return "Terputus dari WiFi.";
    case HTTPC_ERROR_CONNECTION_LOST:     return "Koneksi hilang.";
    case HTTPC_ERROR_NO_STREAM:           return "Tidak ada stream.";
    case HTTPC_ERROR_NO_HTTP_SERVER:      return "Server offline.";
    case HTTPC_ERROR_TOO_LESS_RAM:        return "RAM tidak cukup.";
    case HTTPC_ERROR_ENCODING:            return "Encoding tidak didukung.";
    case HTTPC_ERROR_STREAM_WRITE:        return "Gagal tulis stream.";
    case HTTPC_ERROR_READ_TIMEOUT:        return "Read timeout.";
    default:
      if (httpErrCode > 0) return "HTTP Error " + String(httpErrCode);
      return "Error #" + String(httpErrCode);
  }
}

// ===================== NVS =====================
void loadSettings() {
  preferences.begin("sys_config", true);
  deviceName     = preferences.getString("dev_name", "ESP32-Suhu-Jam");
  wifiSsid1      = preferences.getString("ssid1", "Home-Firewall");
  wifiPass1      = preferences.getString("wpass1", "AkuTergodA1");
  wifiSsid2      = preferences.getString("ssid2", "Backup-WiFi");
  wifiPass2      = preferences.getString("wpass2", "12345678");
  apSsid         = preferences.getString("ap_ssid", "ESP32-Hotspot");
  apPass         = preferences.getString("ap_pass", "12345678");
  configUser     = preferences.getString("user", "admin");
  configPass     = preferences.getString("pass", "password123");
  ntpServerLocal = preferences.getString("ntp1", "192.168.23.1");
  ntpServerInet1 = preferences.getString("ntp2", "pool.ntp.org");
  ntpServerInet2 = preferences.getString("ntp3", "time.google.com");
  enableRedLed   = preferences.getBool("led_red", true);
  verboseDebug   = preferences.getBool("verbose", false);
  preferences.end();
}

void saveSettingsString(const char* key, String val) {
  preferences.begin("sys_config", false);
  preferences.putString(key, val);
  preferences.end();
}

void saveSettingsBool(const char* key, bool val) {
  preferences.begin("sys_config", false);
  preferences.putBool(key, val);
  preferences.end();
}

// ===================== GITHUB UPDATE =====================
void checkGitHubUpdate() {
  if (isApMode || WiFi.status() != WL_CONNECTED) {
    updateCheckStatusMsg = "Koneksi internet tidak tersedia.";
    LOGW("Update check skip: tidak ada internet");
    return;
  }
  DataActivityPulse activity;

  LOGI("Memeriksa update firmware dari GitHub...");
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10);

  HTTPClient http;
  if (http.begin(client, GITHUB_UPDATE_URL)) {
    http.setConnectTimeout(5000);
    http.setTimeout(10000);
    LOGD("GitHub connect timeout=5s, read timeout=10s");

    int httpCode = http.GET();
    LOGD("GitHub GET response: %d", httpCode);
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      LOGD("Payload length: %d bytes", payload.length());
      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, payload);
      if (!error) {
        newVersionStr  = doc["version"].as<String>();
        newVersionCode = doc["versionCode"].as<int>();
        newBinUrl      = doc["zipUrl"].as<String>();
        changelogUrl   = doc["changelog"].as<String>();
        LOGD("Parsed: version=%s, code=%d", newVersionStr.c_str(), newVersionCode);
        if (newVersionCode > FIRMWARE_VERSION_CODE) {
          isUpdateAvailable = true;
          updateCheckStatusMsg = "Versi baru: " + newVersionStr;
          LOGI("Update tersedia: %s (code %d)", newVersionStr.c_str(), newVersionCode);
        } else {
          isUpdateAvailable = false;
          updateCheckStatusMsg = "Versi terbaru: v" FIRMWARE_VERSION;
          LOGI("Firmware up-to-date");
        }
      } else {
        updateCheckStatusMsg = "Gagal parsing JSON.";
        LOGE("JSON parse error: %s", error.c_str());
      }
    } else {
      updateCheckStatusMsg = "Gagal akses GitHub: HTTP " + String(httpCode);
      LOGW("GitHub HTTP error: %d", httpCode);
    }
    http.end();
  } else {
    updateCheckStatusMsg = "Gagal koneksi HTTPS.";
    LOGE("HTTPS init gagal");
  }
}

void performOnlineOTA() {
  if (newBinUrl.length() == 0) return;
  isSystemUpdatingOrRebooting = true;
  lastUpdateError = "";
  LOGI("Memulai OTA Online dari: %s", newBinUrl.c_str());
  wdtSuspend();

  WiFiClientSecure client;
  client.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  blinkBlueLedFast(15, 40);
  t_httpUpdate_return ret = httpUpdate.update(client, newBinUrl);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      isSystemUpdatingOrRebooting = false;
      lastUpdateError = getOTAErrorMessage(httpUpdate.getLastError());
      LOGE("OTA gagal: %s", lastUpdateError.c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      isSystemUpdatingOrRebooting = false;
      lastUpdateError = "Tidak ada file pembaruan.";
      LOGW("OTA: tidak ada file");
      break;
    case HTTP_UPDATE_OK:
      lastUpdateError = "";
      LOGI("OTA sukses, restarting...");
      blinkBlueLedFast(20, 30);
      break;
  }
  wdtResume();
}

// ===================== WIFI =====================
bool connectWiFiTarget(String ssid, String pass) {
  if (ssid.length() == 0) return false;
  LOGI("Koneksi WiFi ke SSID: %s", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(deviceName.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());

  for (int attempt = 1; attempt <= 4; attempt++) {
    unsigned long startWait = millis();
    while (millis() - startWait < 30000) {
      if (WiFi.status() == WL_CONNECTED) {
        LOGD("WiFi connected, RSSI=%d dBm, IP=%s",
             WiFi.RSSI(), WiFi.localIP().toString().c_str());
        return true;
      }
      delay(500);
      wdtFeed();
    }
    LOGW("WiFi percobaan %d/4 timeout", attempt);
  }
  return false;
}

void setupNetwork() {
  LOGI("Setup network...");
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  LOGD("Auto-reconnect: ON, persistent: ON");

  if (connectWiFiTarget(wifiSsid1, wifiPass1)) {
    isApMode = false;
    digitalWrite(LED_MERAH_PIN, LOW);
    LOGI("Terhubung ke WiFi Utama");
    return;
  }
  LOGW("WiFi utama gagal, coba cadangan...");
  if (connectWiFiTarget(wifiSsid2, wifiPass2)) {
    isApMode = false;
    digitalWrite(LED_MERAH_PIN, LOW);
    LOGI("Terhubung ke WiFi Cadangan");
    return;
  }
  LOGE("Kedua WiFi gagal, masuk AP Mode");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid.c_str(), apPass.c_str());
  isApMode = true;
  LOGI("AP Mode: SSID=%s, IP=%s", apSsid.c_str(), WiFi.softAPIP().toString().c_str());
  if (enableRedLed) digitalWrite(LED_MERAH_PIN, HIGH);
  else digitalWrite(LED_MERAH_PIN, LOW);
}

void checkWiFiConnection() {
  if (isApMode) return;
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiReconnectAttempts > 0) {
      LOGI("WiFi stabil kembali");
      wifiReconnectAttempts = 0;
    }
    return;
  }
  wifiReconnectAttempts++;
  LOGW("WiFi drop (percobaan %d/%d), reconnect...",
       wifiReconnectAttempts, WIFI_MAX_ATTEMPTS_BEFORE_SETUP);
  WiFi.disconnect();
  delay(100);
  WiFi.begin(wifiSsid1.c_str(), wifiPass1.c_str());

  unsigned long waitStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - waitStart < WIFI_RECONNECT_WAIT_MS) {
    delay(500);
    wdtFeed();
  }
  if (WiFi.status() == WL_CONNECTED) {
    LOGI("WiFi reconnect berhasil, IP=%s", WiFi.localIP().toString().c_str());
    wifiReconnectAttempts = 0;
    totalWiFiReconnects++;
    syncRTCFromNTP();
    return;
  }
  if (wifiReconnectAttempts >= WIFI_MAX_ATTEMPTS_BEFORE_SETUP) {
    LOGW("Gagal 3x, jalankan setupNetwork() ulang...");
    wifiReconnectAttempts = 0;
    setupNetwork();
    if (!isApMode) {
      syncRTCFromNTP();
      totalWiFiReconnects++;
    }
  }
}

// ===================== NTP & RTC =====================
void syncRTCFromNTP() {
  if (isApMode) return;
  DataActivityPulse activity;
  LOGI("NTP sync dimulai...");
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC,
             ntpServerLocal.c_str(), ntpServerInet1.c_str(), ntpServerInet2.c_str());
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 5000)) {
    int year = timeinfo.tm_year + 1900;
    LOGD("NTP dapat: %04d-%02d-%02d %02d:%02d:%02d",
         year, timeinfo.tm_mon + 1, timeinfo.tm_mday,
         timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    if (year < 2024 || year > 2099) {
      LOGW("NTP tahun tidak valid: %d, skip", year);
      return;
    }
    if (timeinfo.tm_mon < 0 || timeinfo.tm_mon > 11) {
      LOGW("NTP bulan tidak valid, skip");
      return;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint32_t microsToNextSecond = 1000000 - tv.tv_usec;
    if (microsToNextSecond < 1000000) delayMicroseconds(microsToNextSecond);
    time_t ntpSecs;
    time(&ntpSecs);
    ntpSecs += 1;
    if (rtcOk) {
      rtc.adjust(DateTime(ntpSecs));

      // [v1.1.4] Simpan reference untuk NTP server
      lastSyncEpochNTP = ntpSecs - 1;
      lastSyncMillis = millis();
      hasSyncedWithUpstream = true;

      LOGI("RTC disinkronkan: %04d-%02d-%02d %02d:%02d:%02d",
           year, timeinfo.tm_mon + 1, timeinfo.tm_mday,
           timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
      LOGD("Reference epoch tersimpan: %lu", lastSyncEpochNTP);
    }
  } else {
    LOGW("NTP timeout, RTC tidak diperbarui");
  }
}

// ===================== NTP SERVER (v1.1.4) =====================
void handleNTPServer() {
  int packetSize = udpServer.parsePacket();
  if (packetSize >= 48) {
    DataActivityPulse activity;
    IPAddress remoteIP = udpServer.remoteIP();
    LOGD("NTP request dari %s", remoteIP.toString().c_str());

    byte requestBuffer[48];
    udpServer.read(requestBuffer, 48);

    // --- Hitung waktu server dengan presisi sub-detik ---
    DateTime now = rtcOk ? rtc.now() : DateTime(0UL);
    uint32_t epochSecs = now.unixtime();

    // Interpolasi sub-detik via millis() sejak sync terakhir
    unsigned long subMs = 0;
    if (hasSyncedWithUpstream && millis() > lastSyncMillis) {
      unsigned long elapsed = millis() - lastSyncMillis;
      uint32_t extraSecs = elapsed / 1000;
      subMs = elapsed % 1000;
      epochSecs = lastSyncEpochNTP + extraSecs;
    }

    // Konversi ke NTP epoch (1900 + 2208988800)
    uint32_t ntpSecs = epochSecs + 2208988800UL;
    uint32_t ntpFrac = (uint32_t)((subMs * 4294967296ULL) / 1000ULL);

    // Reference timestamp = waktu sync terakhir dari upstream
    uint32_t refNtpSecs;
    if (hasSyncedWithUpstream) {
      refNtpSecs = lastSyncEpochNTP + 2208988800UL;
    } else {
      refNtpSecs = ntpSecs;
    }

    byte responseBuffer[48];
    memset(responseBuffer, 0, 48);

    // Byte 0: LI=0, VN=4, Mode=4 (server)
    responseBuffer[0] = 0x24;

    // Byte 1: Stratum — 2 jika sudah sync upstream, 16 jika belum
    responseBuffer[1] = hasSyncedWithUpstream ? 2 : 16;

    // Byte 2: Poll interval (6 = 64s)
    responseBuffer[2] = 6;

    // Byte 3: Precision (-20 = 1 us)
    responseBuffer[3] = 0xEC;

    // Byte 4-7: Root Delay (0)
    // Byte 8-11: Root Dispersion
    responseBuffer[11] = 0xA0;

    // Byte 12-15: Reference ID ("RTC ")
    responseBuffer[12] = 'R';
    responseBuffer[13] = 'T';
    responseBuffer[14] = 'C';
    responseBuffer[15] = ' ';

    // Byte 16-23: Reference Timestamp
    responseBuffer[16] = (refNtpSecs >> 24) & 0xFF;
    responseBuffer[17] = (refNtpSecs >> 16) & 0xFF;
    responseBuffer[18] = (refNtpSecs >> 8) & 0xFF;
    responseBuffer[19] = refNtpSecs & 0xFF;

    // Byte 24-31: Origin Timestamp (echo dari client)
    for (int i = 0; i < 8; i++) responseBuffer[24 + i] = requestBuffer[40 + i];

    // Byte 32-39: Receive Timestamp (dengan fraksi sub-detik)
    responseBuffer[32] = (ntpSecs >> 24) & 0xFF;
    responseBuffer[33] = (ntpSecs >> 16) & 0xFF;
    responseBuffer[34] = (ntpSecs >> 8) & 0xFF;
    responseBuffer[35] = ntpSecs & 0xFF;
    responseBuffer[36] = (ntpFrac >> 24) & 0xFF;
    responseBuffer[37] = (ntpFrac >> 16) & 0xFF;
    responseBuffer[38] = (ntpFrac >> 8) & 0xFF;
    responseBuffer[39] = ntpFrac & 0xFF;

    // Byte 40-47: Transmit Timestamp
    responseBuffer[40] = (ntpSecs >> 24) & 0xFF;
    responseBuffer[41] = (ntpSecs >> 16) & 0xFF;
    responseBuffer[42] = (ntpSecs >> 8) & 0xFF;
    responseBuffer[43] = ntpSecs & 0xFF;
    responseBuffer[44] = (ntpFrac >> 24) & 0xFF;
    responseBuffer[45] = (ntpFrac >> 16) & 0xFF;
    responseBuffer[46] = (ntpFrac >> 8) & 0xFF;
    responseBuffer[47] = ntpFrac & 0xFF;

    udpServer.beginPacket(udpServer.remoteIP(), udpServer.remotePort());
    udpServer.write(responseBuffer, 48);
    udpServer.endPacket();

    LOGD("NTP response -> %s (stratum %d, ref=%lu)",
         remoteIP.toString().c_str(),
         hasSyncedWithUpstream ? 2 : 16,
         refNtpSecs);
  }
}

// ===================== REST API =====================
void handleRoot() {
  logHttpRequest("handleRoot");
  DataActivityPulse activity;

  sensors_event_t humidity, temp_aht;
  float t_aht = 0, hum = 0, t_bmp = 0, pres = 0, alt = 0;

  if (ahtOk) {
    aht.getEvent(&humidity, &temp_aht);
    t_aht = temp_aht.temperature;
    hum = humidity.relative_humidity;
    LOGD("AHT10: %.2fC, %.1f%%", t_aht, hum);
  }
  if (bmpOk) {
    t_bmp = bmp.readTemperature();
    float p_pa = bmp.readPressure();
    pres = p_pa / 100.0;
    alt = 44330.0 * (1.0 - pow(p_pa / 101325.0, 0.1902949));
    LOGD("GY68: %.2fC, %.2fhPa, %.1fm", t_bmp, pres, alt);
  }

  float t_avg = 0.0;
  if (ahtOk && bmpOk)       t_avg = (t_aht + t_bmp) / 2.0;
  else if (ahtOk)           t_avg = t_aht;
  else if (bmpOk)           t_avg = t_bmp;
  LOGD("Suhu rata-rata: %.1fC", t_avg);

  DateTime now = rtcOk ? rtc.now() : DateTime(0UL);
  char timeStr[25];
  snprintf(timeStr, sizeof(timeStr), "%04d-%02d-%02d %02d:%02d:%02d",
           now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());

  String jsonPayload = "{";
  jsonPayload += "\"device_name\":\"" + deviceName + "\",";
  jsonPayload += "\"firmware_version\":\"" FIRMWARE_VERSION "\",";
  jsonPayload += "\"firmware_version_code\":" + String(FIRMWARE_VERSION_CODE) + ",";
  jsonPayload += "\"update_available\":" + String(isUpdateAvailable ? "true" : "false") + ",";
  jsonPayload += "\"status\":\"online\",";
  jsonPayload += "\"datetime\":\"" + String(timeStr) + "\",";
  jsonPayload += "\"epoch_timestamp\":" + String(now.unixtime()) + ",";
  jsonPayload += "\"temperature\":" + String(t_avg, 1) + ",";
  jsonPayload += "\"temperature_aht10\":" + String(t_aht, 1) + ",";
  jsonPayload += "\"humidity\":" + String(hum, 1) + ",";
  jsonPayload += "\"temperature_gy68\":" + String(t_bmp, 1) + ",";
  jsonPayload += "\"pressure_hpa\":" + String(pres, 2) + ",";
  jsonPayload += "\"altitude_m\":" + String(alt, 1) + ",";
  jsonPayload += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
  jsonPayload += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  jsonPayload += "\"max_alloc_heap\":" + String(ESP.getMaxAllocHeap()) + ",";
  jsonPayload += "\"uptime_s\":" + String(millis() / 1000) + ",";
  jsonPayload += "\"wifi_reconnects\":" + String(totalWiFiReconnects) + ",";
  jsonPayload += "\"ntp_synced\":" + String(hasSyncedWithUpstream ? "true" : "false");
  jsonPayload += "}";
  server.send(200, "application/json", jsonPayload);
}

void handleTimeApi() {
  logHttpRequest("handleTimeApi");
  DataActivityPulse activity;
  DateTime now = rtcOk ? rtc.now() : DateTime(0UL);
  char timeStr[25];
  snprintf(timeStr, sizeof(timeStr), "%04d-%02d-%02d %02d:%02d:%02d",
           now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
  String jsonPayload = "{";
  jsonPayload += "\"datetime\":\"" + String(timeStr) + "\",";
  jsonPayload += "\"epoch\":" + String(now.unixtime()) + ",";
  jsonPayload += "\"year\":" + String(now.year()) + ",";
  jsonPayload += "\"month\":" + String(now.month()) + ",";
  jsonPayload += "\"day\":" + String(now.day()) + ",";
  jsonPayload += "\"hour\":" + String(now.hour()) + ",";
  jsonPayload += "\"minute\":" + String(now.minute()) + ",";
  jsonPayload += "\"second\":" + String(now.second());
  jsonPayload += "}";
  server.send(200, "application/json", jsonPayload);
}

void handlePingApi() {
  logHttpRequest("handlePingApi");
  DataActivityPulse activity;
  server.send(200, "application/json",
    "{\"status\":\"ok\",\"version\":\"" FIRMWARE_VERSION "\"}");
}

void handleLogout() {
  logHttpRequest("handleLogout");
  DataActivityPulse activity;
  LOGI("Logout dari %s", server.client().remoteIP().toString().c_str());
  server.sendHeader("WWW-Authenticate", "Basic realm=\"Login Required\"");
  server.send(401, "text/html",
    "<meta charset='utf-8'><div style='font-family:sans-serif;text-align:center;margin-top:50px;'>"
    "<h2>Anda telah logout.</h2>"
    "<p><a href='/config'>Klik di sini untuk login kembali</a></p></div>");
}

// ===================== WEB SERIAL MONITOR =====================
void handleSerialPage() {
  logHttpRequest("handleSerialPage");
  String html = "<!DOCTYPE html><html lang='id'><head>";
  html += "<meta charset='utf-8'><title>ESP32 Serial Monitor</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<style>";
  html += "body{background:#1e1e1e;color:#d4d4d4;font-family:Consolas,'Courier New',monospace;margin:0;padding:10px;font-size:13px;}";
  html += "h1{color:#4ec9b0;font-size:14px;margin:0 0 8px 0;font-weight:600;}";
  html += "#log{background:#000;padding:12px;border-radius:5px;height:calc(100vh - 140px);overflow-y:auto;white-space:pre-wrap;word-break:break-word;line-height:1.35;font-size:12.5px;}";
  html += "button{background:#007bff;color:#fff;border:none;padding:6px 12px;border-radius:3px;cursor:pointer;margin-right:5px;font-size:12px;font-weight:600;}";
  html += "button:hover{background:#0056b3;}";
  html += "button.off{background:#6c757d;}";
  html += "#status{color:#888;font-size:11px;margin-top:5px;}";
  html += ".lvl-DBG{color:#888;}";
  html += ".lvl-INFO{color:#4ec9b0;}";
  html += ".lvl-WARN{color:#dcdcaa;}";
  html += ".lvl-ERR{color:#f48771;font-weight:bold;}";
  html += "</style></head><body>";
  html += "<h1>&#128225; ESP32 Serial Monitor &mdash; " + deviceName + "</h1>";
  html += "<div style='margin-bottom:8px;'>";
  html += "<button id='asBtn' onclick='toggleAS()'>Auto-scroll: ON</button>";
  html += "<button id='pauseBtn' onclick='togglePause()'>Pause</button>";
  html += "<button onclick='clearLocal()'>Clear Local</button>";
  html += "<button onclick='reloadAll()'>Reload All</button>";
  html += "<button onclick='downloadLog()'>Download Log</button>";
  html += "<button onclick='location.href=\"/config\"'>Config</button>";
  html += "</div>";
  html += "<div id='log'></div>";
  html += "<div id='status'>Memuat...</div>";
  html += "<script>";
  html += "var allLines=[];";
  html += "var lastSeq=0;";
  html += "var as=true,paused=false;";
  html += "var MAX_BROWSER_LINES=5000;";
  html += "function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}";
  html += "function lvlClass(s){";
  html += "  if(s.indexOf('[DBG]')>=0) return 'lvl-DBG';";
  html += "  if(s.indexOf('[WARN]')>=0) return 'lvl-WARN';";
  html += "  if(s.indexOf('[ERR]')>=0) return 'lvl-ERR';";
  html += "  return 'lvl-INFO';";
  html += "}";
  html += "function render(){";
  html += "  if(paused) return;";
  html += "  var el=document.getElementById('log');";
  html += "  var h='';";
  html += "  for(var i=0;i<allLines.length;i++){";
  html += "    h+='<div class=\"'+lvlClass(allLines[i])+'\">'+esc(allLines[i])+'</div>';";
  html += "  }";
  html += "  el.innerHTML=h;";
  html += "  if(as) el.scrollTop=el.scrollHeight;";
  html += "  document.getElementById('status').innerText=allLines.length+' baris (browser) | seq '+lastSeq+' | '+new Date().toLocaleTimeString();";
  html += "}";
  html += "function fetchLogs(){";
  html += "  fetch('/logs?since='+lastSeq+'&t='+Date.now())";
  html += "    .then(function(r){return r.json();})";
  html += "    .then(function(d){";
  html += "      if(d.gap) allLines=[];";
  html += "      if(d.lines && d.lines.length){";
  html += "        for(var i=0;i<d.lines.length;i++) allLines.push(d.lines[i].text);";
  html += "        if(allLines.length>MAX_BROWSER_LINES) allLines=allLines.slice(-MAX_BROWSER_LINES);";
  html += "        lastSeq=d.latest;";
  html += "        render();";
  html += "      } else {";
  html += "        lastSeq=d.latest;";
  html += "        document.getElementById('status').innerText=allLines.length+' baris | seq '+lastSeq+' | idle '+new Date().toLocaleTimeString();";
  html += "      }";
  html += "    })";
  html += "    .catch(function(e){";
  html += "      document.getElementById('status').innerText='Koneksi error: '+e;";
  html += "    });";
  html += "}";
  html += "function toggleAS(){as=!as;var b=document.getElementById('asBtn');b.innerText='Auto-scroll: '+(as?'ON':'OFF');b.className=as?'':'off';}";
  html += "function togglePause(){paused=!paused;var b=document.getElementById('pauseBtn');b.innerText=paused?'Resume':'Pause';b.className=paused?'off':'';if(!paused)render();}";
  html += "function clearLocal(){allLines=[];render();}";
  html += "function reloadAll(){allLines=[];lastSeq=0;fetchLogs();}";
  html += "function downloadLog(){";
  html += "  var blob=new Blob([allLines.join('\\n')],{type:'text/plain'});";
  html += "  var url=URL.createObjectURL(blob);";
  html += "  var a=document.createElement('a');";
  html += "  a.href=url;a.download='esp32_log_'+new Date().toISOString().replace(/[:.]/g,'-')+'.txt';";
  html += "  a.click();URL.revokeObjectURL(url);";
  html += "}";
  html += "setInterval(fetchLogs,1000);";
  html += "fetchLogs();";
  html += "</script>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleLogsJson() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");

  unsigned long since = 0;
  if (server.hasArg("since")) {
    since = strtoul(server.arg("since").c_str(), NULL, 10);
  }

  unsigned long oldest = 0;
  if (logSize > 0) {
    int start = logHead - logSize;
    if (start < 0) start += LOG_BUFFER_LINES;
    oldest = logSeq[start];
  }
  bool gap = (since > 0 && oldest > 0 && since < oldest - 1);

  String json = "{";
  json += "\"latest\":" + String(logSeqCounter) + ",";
  json += "\"oldest\":" + String(oldest) + ",";
  json += "\"gap\":" + String(gap ? "true" : "false") + ",";
  json += "\"lines\":[";

  int start = logHead - logSize;
  if (start < 0) start += LOG_BUFFER_LINES;
  bool first = true;

  for (int i = 0; i < logSize; i++) {
    int idx = (start + i) % LOG_BUFFER_LINES;
    unsigned long s = logSeq[idx];
    if (s <= since) continue;
    if (!first) json += ",";
    first = false;
    json += "{\"seq\":" + String(s) + ",\"text\":\"";
    const char* p = logBuf[idx];
    while (*p) {
      char c = *p++;
      if (c == '"' || c == '\\')      { json += "\\"; json += c; }
      else if (c == '\n')              json += "\\n";
      else if (c == '\r')              continue;
      else if (c == '\t')              json += "\\t";
      else                             json += c;
    }
    json += "\"}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

// ===================== REBOOT PAGE =====================
String getRebootWaitingHtmlPage(String title, String statusMsg) {
  String html = "<!DOCTYPE html><html lang='id'><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>" + title + "</title>";
  html += "<style>body{font-family:Segoe UI,sans-serif;background:#f4f4f9;text-align:center;padding:50px 20px;}";
  html += ".card{background:#fff;padding:30px;border-radius:10px;box-shadow:0 3px 10px rgba(0,0,0,0.1);max-width:420px;margin:auto;}";
  html += ".spinner{border:5px solid #f3f3f3;border-top:5px solid #007bff;border-radius:50%;width:45px;height:45px;animation:spin 1s linear infinite;margin:20px auto;}";
  html += "@keyframes spin{0%{transform:rotate(0deg);}100%{transform:rotate(360deg);}}</style>";
  html += "</head><body><div class='card'>";
  html += "<h2>" + title + "</h2>";
  html += "<p>" + statusMsg + "</p>";
  html += "<div class='spinner'></div>";
  html += "<p id='status' style='font-size:13px;color:#666;'>Menunggu perangkat online kembali...</p>";
  html += "<script>";
  html += "var checkInterval = setInterval(function(){";
  html += "  fetch('/api/ping', {method:'GET', cache:'no-store'}).then(function(res){";
  html += "    if(res.ok){";
  html += "      clearInterval(checkInterval);";
  html += "      document.getElementById('status').innerText = 'Perangkat berhasil terhubung!';";
  html += "      setTimeout(function(){ location.href = '/config'; }, 1500);";
  html += "    }";
  html += "  }).catch(function(e){});";
  html += "}, 2000);";
  html += "</script></div></body></html>";
  return html;
}

void handleDoOnlineUpdate() {
  logHttpRequest("handleDoOnlineUpdate");
  if (!server.authenticate(configUser.c_str(), configPass.c_str())) {
    LOGW("Auth gagal untuk OTA trigger");
    return server.requestAuthentication();
  }
  if (!isUpdateAvailable || newBinUrl.length() == 0) {
    server.send(400, "text/html",
      "<h3>Tidak ada pembaruan firmware.</h3><a href='/config'>Kembali</a>");
    return;
  }
  LOGI("OTA trigger dari Web UI");
  server.send(200, "text/html",
    getRebootWaitingHtmlPage("Proses Online OTA Update",
      "Sedang mengunduh & menginstal firmware baru..."));
  delay(1000);
  performOnlineOTA();
}

// ===================== CONFIG =====================
void handleConfig() {
  logHttpRequest("handleConfig");
  if (!server.authenticate(configUser.c_str(), configPass.c_str())) {
    LOGW("Config: auth gagal dari %s", server.client().remoteIP().toString().c_str());
    return server.requestAuthentication();
  }
  DataActivityPulse activity;

  String msg = "";
  bool msgError = false;

  if (server.hasArg("check_update_now")) {
    LOGI("Manual cek update dari Web UI");
    checkGitHubUpdate();
    msg = updateCheckStatusMsg;
    msgError = !isUpdateAvailable;
  }
  if (server.hasArg("save_dev")) {
    deviceName = server.arg("dev_name");
    saveSettingsString("dev_name", deviceName);
    msg = "Nama Perangkat diperbarui!";
    LOGI("Device name diubah: %s", deviceName.c_str());
  }
  if (server.hasArg("save_wifi")) {
    wifiSsid1 = server.arg("ssid1"); wifiPass1 = server.arg("wpass1");
    wifiSsid2 = server.arg("ssid2"); wifiPass2 = server.arg("wpass2");
    apSsid    = server.arg("ap_ssid"); apPass = server.arg("ap_pass");
    enableRedLed = server.hasArg("enable_red_led");
    saveSettingsBool("led_red", enableRedLed);
    if (!enableRedLed || !isApMode) digitalWrite(LED_MERAH_PIN, LOW);
    else if (enableRedLed && isApMode) digitalWrite(LED_MERAH_PIN, HIGH);
    saveSettingsString("ssid1", wifiSsid1); saveSettingsString("wpass1", wifiPass1);
    saveSettingsString("ssid2", wifiSsid2); saveSettingsString("wpass2", wifiPass2);
    saveSettingsString("ap_ssid", apSsid);   saveSettingsString("ap_pass", apPass);
    msg = "Pengaturan Network & LED disimpan!";
    LOGI("Pengaturan WiFi disimpan");
  }
  if (server.hasArg("save_debug")) {
    verboseDebug = server.hasArg("verbose");
    saveSettingsBool("verbose", verboseDebug);
    msg = String("Mode Debug: ") + (verboseDebug ? "AKTIF" : "NONAKTIF");
    LOGI("Mode debug diubah: %s", verboseDebug ? "ON" : "OFF");
  }
  if (server.hasArg("save_ntp")) {
    ntpServerLocal = server.arg("ntp1");
    ntpServerInet1 = server.arg("ntp2");
    ntpServerInet2 = server.arg("ntp3");
    saveSettingsString("ntp1", ntpServerLocal);
    saveSettingsString("ntp2", ntpServerInet1);
    saveSettingsString("ntp3", ntpServerInet2);
    LOGI("Pengaturan NTP disimpan, sync ulang...");
    syncRTCFromNTP();
    msg = "Pengaturan NTP disimpan & disinkronkan!";
  }
  if (server.hasArg("save_auth")) {
    String oldPass = server.arg("old_pass");
    String newUser = server.arg("new_user");
    String newPass = server.arg("new_pass");
    if (oldPass == configPass) {
      if (newUser.length() > 0 && newPass.length() > 0) {
        configUser = newUser; configPass = newPass;
        saveSettingsString("user", newUser);
        saveSettingsString("pass", newPass);
        msg = "Kredensial diperbarui!";
        LOGI("Kredensial login diubah");
      } else {
        msg = "Username/Password tidak boleh kosong!"; msgError = true;
        LOGW("Ubah kredensial: field kosong");
      }
    } else {
      msg = "Password Lama tidak cocok."; msgError = true;
      LOGW("Ubah kredensial: password lama salah");
    }
  }

  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t totalHeap = ESP.getHeapSize();
  uint32_t maxAlloc = ESP.getMaxAllocHeap();
  uint32_t usedHeap = totalHeap - freeHeap;
  float heapUsagePct = ((float)usedHeap / (float)totalHeap) * 100.0;
  unsigned long uptimeSec = millis() / 1000;
  unsigned int days = uptimeSec / 86400;
  unsigned int hours = (uptimeSec % 86400) / 3600;
  unsigned int minutes = (uptimeSec % 3600) / 60;
  unsigned int seconds = uptimeSec % 60;

  String html = "<!DOCTYPE html><html lang='id'><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>" + deviceName + " - Config</title>";
  html += "<style>body{font-family:Segoe UI,Tahoma,sans-serif;margin:20px;background:#f4f4f9;color:#333}";
  html += ".card{background:#fff;padding:22px;border-radius:10px;box-shadow:0 3px 8px rgba(0,0,0,0.12);max-width:480px;margin:auto}";
  html += "h2{color:#007bff;margin-top:0;font-size:22px;} h3{font-size:15px;margin-top:18px;margin-bottom:8px;color:#444;}";
  html += "label{font-weight:600;display:block;margin-top:10px;font-size:13px}";
  html += "input[type=text],input[type=password],input[type=file]{width:100%;padding:9px;margin-top:4px;box-sizing:border-box;border:1px solid #ccc;border-radius:5px;font-size:14px}";
  html += ".checkbox-container{display:flex;align-items:center;margin-top:12px;font-size:13px;font-weight:600;cursor:pointer;}";
  html += ".checkbox-container input{width:auto;margin-right:8px;}";
  html += "button,.btn{background:#007bff;color:#fff;border:none;padding:10px 15px;margin-top:14px;border-radius:5px;cursor:pointer;width:100%;font-weight:bold;font-size:14px;text-align:center;text-decoration:none;display:inline-block;box-sizing:border-box}";
  html += ".btn-danger{background:#dc3545}.btn-warning{background:#ffc107;color:#000}.btn-success{background:#28a745}.btn-outline{background:none;border:1px solid #007bff;color:#007bff}";
  html += ".btn-serial{background:#212529;color:#4ec9b0;border:1px solid #4ec9b0;}";
  html += ".ok{color:#28a745;font-weight:bold}.err{color:#dc3545;font-weight:bold}";
  html += ".alert{padding:12px;border-radius:5px;margin-bottom:15px;font-size:13px;font-weight:bold}";
  html += ".alert-success{background:#d4edda;color:#155724;border:1px solid #c3e6cb}";
  html += ".alert-danger{background:#f8d7da;color:#721c24;border:1px solid #f5c6cb}";
  html += ".alert-info{background:#d1ecf1;color:#0c5460;border:1px solid #bee5eb}";
  html += ".info-table{width:100%;border-collapse:collapse;margin-top:8px;font-size:13px}";
  html += ".info-table td{padding:5px 0;border-bottom:1px solid #eee}</style>";

  html += "<script>";
  html += "var timeout;";
  html += "function autoLogout(){fetch('/logout').then(function(){alert('Sesi berakhir (idle 2 menit).');location.href='/config';});}";
  html += "function resetTimer(){clearTimeout(timeout);timeout=setTimeout(autoLogout,120000);}";
  html += "window.onload=resetTimer;window.onmousemove=resetTimer;window.onmousedown=resetTimer;window.onclick=resetTimer;window.onkeydown=resetTimer;";
  html += "</script>";
  html += "</head><body>";

  html += "<div class='card'>";
  html += "<div style='display:flex;justify-content:space-between;align-items:center;'>";
  html += "<h2>&#9881;&#65039; Config</h2>";
  html += "<button onclick='fetch(\"/logout\").then(function(){location.href=\"/config\";});' style='margin:0;padding:6px 12px;width:auto;' class='btn btn-warning'>Logout</button>";
  html += "</div>";

  html += "<a href='/serial' class='btn btn-serial' style='margin-top:12px;'>&#128225; Buka Serial Monitor (Live Log)</a>";

  if (lastUpdateError.length() > 0) {
    html += "<div class='alert alert-danger' style='margin-top:15px;'>";
    html += "⚠️ <b>Gagal Memasang Firmware!</b><br>";
    html += "Penyebab: <u>" + lastUpdateError + "</u><br></div>";
  }
  if (msg.length() > 0) {
    html += "<div class='alert " + String(msgError ? "alert-danger" : "alert-success") + "' style='margin-top:15px;'>" + msg + "</div>";
  }
  if (isUpdateAvailable) {
    html += "<div class='alert alert-success' style='margin-top:15px;'>";
    html += "🚀 <b>Pembaruan Firmware Ditemukan!</b><br>";
    html += "Versi Terbaru: <b>" + newVersionStr + "</b><br>";
    if (changelogUrl.length() > 0) {
      html += "Catatan Rilis: <a href='" + changelogUrl + "' target='_blank' style='color:#155724;text-decoration:underline;'>Lihat Changelog</a><br>";
    }
    html += "<form method='POST' action='/do_online_update'>";
    html += "<button type='submit' class='btn btn-success' onclick='return confirm(\"Pasang firmware " + newVersionStr + "?\")'>&#11015;&#65039; Update Online Sekarang</button>";
    html += "</form></div>";
  }

  html += "<h3>&#128187; Informasi Sistem & Memori</h3>";
  html += "<table class='info-table'>";
  html += "<tr><td><b>Nama Device:</b></td><td>" + deviceName + "</td></tr>";
  html += "<tr><td><b>Versi Firmware:</b></td><td><span class='ok'>v" FIRMWARE_VERSION "</span> (Code: " + String(FIRMWARE_VERSION_CODE) + ")</td></tr>";
  html += "<tr><td><b>Mode Debug:</b></td><td>" + String(verboseDebug ? "<span class='ok'>AKTIF (verbose)</span>" : "<span class='err'>NONAKTIF</span>") + "</td></tr>";
  html += "<tr><td><b>Status NTP:</b></td><td>" + String(hasSyncedWithUpstream ? "<span class='ok'>Synced (stratum 2)</span>" : "<span class='err'>Belum sync (stratum 16)</span>") + "</td></tr>";
  html += "<tr><td><b>Status Update:</b></td><td>" + String(isUpdateAvailable ? "<span class='err'>Tersedia " + newVersionStr + "</span>" : "<span class='ok'>Up to date</span>") + "</td></tr>";
  html += "<tr><td><b>Mode Network:</b></td><td>" + String(isApMode ? "<span class='err'>AP Hotspot</span>" : "<span class='ok'>STA Connected</span>") + "</td></tr>";
  html += "<tr><td><b>IP Address:</b></td><td>" + (isApMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "</td></tr>";
  html += "<tr><td><b>WiFi RSSI:</b></td><td>" + String(WiFi.RSSI()) + " dBm</td></tr>";
  html += "<tr><td><b>Total Reconnect:</b></td><td>" + String(totalWiFiReconnects) + " kali</td></tr>";
  html += "<tr><td><b>LED Merah:</b></td><td>" + String(enableRedLed ? "<span class='ok'>Aktif</span>" : "<span class='err'>Off</span>") + "</td></tr>";
  html += "<tr><td><b>Free Heap:</b></td><td>" + String(freeHeap / 1024.0, 1) + " KB / " + String(totalHeap / 1024.0, 1) + " KB (" + String(heapUsagePct, 1) + "%)</td></tr>";
  html += "<tr><td><b>Max Alloc Heap:</b></td><td>" + String(maxAlloc / 1024.0, 1) + " KB</td></tr>";
  html += "<tr><td><b>Min Free Heap:</b></td><td>" + String(ESP.getMinFreeHeap() / 1024.0, 1) + " KB</td></tr>";
  html += "<tr><td><b>Uptime:</b></td><td>" + String(days) + "d " + String(hours) + "h " + String(minutes) + "m " + String(seconds) + "s</td></tr>";
  html += "<tr><td><b>CPU / Flash:</b></td><td>" + String(ESP.getCpuFreqMHz()) + " MHz / " + String(ESP.getFlashChipSize() / (1024 * 1024)) + " MB</td></tr>";
  html += "<tr><td><b>Log Buffer:</b></td><td>" + String(logSize) + " / " + String(LOG_BUFFER_LINES) + " baris | seq " + String(logSeqCounter) + "</td></tr>";
  html += "<tr><td><b>NTP Resync Interval:</b></td><td>6 jam</td></tr>";
  html += "</table>";

  html += "<form method='POST'>";
  html += "<button type='submit' name='check_update_now' value='1' class='btn btn-outline'>&#128260; Cek Update GitHub</button>";
  html += "</form>";

  html += "<h3>&#128268; Status Hardware</h3>";
  html += "<p style='font-size:13px;margin:3px 0;'>AHT10: " + String(ahtOk ? "<span class='ok'>[ONLINE]</span>" : "<span class='err'>[OFFLINE]</span>") + "</p>";
  html += "<p style='font-size:13px;margin:3px 0;'>GY-68 (BMP180): " + String(bmpOk ? "<span class='ok'>[ONLINE]</span>" : "<span class='err'>[OFFLINE]</span>") + "</p>";
  html += "<p style='font-size:13px;margin:3px 0;'>DS1307 RTC: " + String(rtcOk ? "<span class='ok'>[ONLINE]</span>" : "<span class='err'>[OFFLINE]</span>") + "</p>";
  html += "<hr>";

  html += "<h3>&#128269; Mode Debug Verbose</h3>";
  html += "<p style='font-size:12px;color:#666;margin:5px 0;'>Tampilkan detail: HTTP request, sensor read, state change. Berguna untuk debugging.</p>";
  html += "<form method='POST'>";
  html += "<label class='checkbox-container'>";
  html += "<input type='checkbox' name='verbose' value='1' " + String(verboseDebug ? "checked" : "") + ">";
  html += "Aktifkan Mode Debug Verbose</label>";
  html += "<button type='submit' name='save_debug' value='1' class='btn btn-outline'>Simpan Mode Debug</button>";
  html += "</form>";
  html += "<hr>";

  html += "<h3>&#127991;&#65039; Nama Perangkat</h3>";
  html += "<form method='POST'>";
  html += "<label>Device Hostname:</label><input type='text' name='dev_name' value='" + deviceName + "' required>";
  html += "<button type='submit' name='save_dev' value='1'>Simpan Nama</button>";
  html += "</form>";
  html += "<hr>";

  html += "<h3>&#128246; Pengaturan WiFi</h3>";
  html += "<form method='POST'>";
  html += "<label><b>WiFi Target 1 (Utama):</b></label>";
  html += "<input type='text' name='ssid1' value='" + wifiSsid1 + "' required>";
  html += "<input type='password' name='wpass1' value='" + wifiPass1 + "'>";
  html += "<label style='margin-top:12px;'><b>WiFi Target 2 (Cadangan):</b></label>";
  html += "<input type='text' name='ssid2' value='" + wifiSsid2 + "'>";
  html += "<input type='password' name='wpass2' value='" + wifiPass2 + "'>";
  html += "<label style='margin-top:12px;'><b>Auto AP Hotspot:</b></label>";
  html += "<input type='text' name='ap_ssid' value='" + apSsid + "' required>";
  html += "<input type='password' name='ap_pass' value='" + apPass + "' required>";
  html += "<label class='checkbox-container'>";
  html += "<input type='checkbox' name='enable_red_led' value='1' " + String(enableRedLed ? "checked" : "") + ">";
  html += "Aktifkan LED Merah Failover</label>";
  html += "<button type='submit' name='save_wifi' value='1'>Simpan WiFi & LED</button>";
  html += "</form>";
  html += "<hr>";

  html += "<h3>&#128336; Pengaturan NTP</h3>";
  html += "<form method='POST'>";
  html += "<label>NTP Lokal:</label><input type='text' name='ntp1' value='" + ntpServerLocal + "'>";
  html += "<label>NTP Internet 1:</label><input type='text' name='ntp2' value='" + ntpServerInet1 + "'>";
  html += "<label>NTP Internet 2:</label><input type='text' name='ntp3' value='" + ntpServerInet2 + "'>";
  html += "<button type='submit' name='save_ntp' value='1'>Simpan & Sync NTP</button>";
  html += "</form>";
  html += "<hr>";

  html += "<h3>&#128274; Ubah Kredensial</h3>";
  html += "<form method='POST'>";
  html += "<label>User Baru:</label><input type='text' name='new_user' value='" + configUser + "' required>";
  html += "<label>Password Baru:</label><input type='password' name='new_pass' required>";
  html += "<label>Password Lama:</label><input type='password' name='old_pass' required>";
  html += "<button type='submit' name='save_auth' value='1'>Ubah User & Password</button>";
  html += "</form>";
  html += "<hr>";

  html += "<h3>&#128230; Manual Firmware Update (.bin)</h3>";
  html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
  html += "<input type='file' name='update' accept='.bin' required>";
  html += "<button type='submit' class='btn'>Upload Firmware</button>";
  html += "</form>";
  html += "<hr>";

  html += "<form method='POST' action='/restart'>";
  html += "<button type='submit' class='btn btn-danger' onclick='return confirm(\"Restart ESP32?\")'>Restart ESP32</button>";
  html += "</form>";

  html += "</div></body></html>";
  server.send(200, "text/html", html);
}

void handleRestart() {
  logHttpRequest("handleRestart");
  if (!server.authenticate(configUser.c_str(), configPass.c_str())) {
    return server.requestAuthentication();
  }
  isSystemUpdatingOrRebooting = true;
  LOGI("Restart diminta dari Web UI");
  server.send(200, "text/html",
    getRebootWaitingHtmlPage("Restart ESP32", "Sedang memproses perintah restart..."));
  delay(500);
  blinkBlueLedFast(20, 30);
  ESP.restart();
}

void updateLEDIndicators() {
  if (isSystemUpdatingOrRebooting) return;
  if (!enableRedLed || (WiFi.status() == WL_CONNECTED && !isApMode)) {
    digitalWrite(LED_MERAH_PIN, LOW);
  } else if (enableRedLed && isApMode) {
    digitalWrite(LED_MERAH_PIN, HIGH);
  }
  static unsigned long lastHeartbeat = 0;
  static bool ledState = false;
  static unsigned long ledTurnOnTime = 0;
  unsigned long currentMillis = millis();
  if (!ledState && (currentMillis - lastHeartbeat >= 5000)) {
    lastHeartbeat = currentMillis;
    ledState = true;
    ledBiruOn();
    ledTurnOnTime = currentMillis;
  }
  if (ledState && (currentMillis - ledTurnOnTime >= 100)) {
    ledState = false;
    ledBiruOff();
  }
}

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(LED_MERAH_PIN, OUTPUT);
  pinMode(LED_BIRU_PIN, OUTPUT);
  digitalWrite(LED_MERAH_PIN, LOW);
  ledBiruOff();

  LogSerial.printf("\n=== ESP32 Weather Station v%s (Code %d) ===\n",
                   FIRMWARE_VERSION, FIRMWARE_VERSION_CODE);
  LogSerial.printf("Core Version: %d.%d.%d\n",
                   ESP_ARDUINO_VERSION_MAJOR,
                   ESP_ARDUINO_VERSION_MINOR,
                   ESP_ARDUINO_VERSION_PATCH);

  LOGI("=== BOOT START ===");
  LOGI("Firmware v%s (code %d)", FIRMWARE_VERSION, FIRMWARE_VERSION_CODE);
  LOGI("NTP resync interval: 6 jam");

  LOGI("Init watchdog...");
  wdtInit();

  LOGI("Init I2C bus (SDA=%d, SCL=%d)", SDA_PIN, SCL_PIN);
  Wire.begin(SDA_PIN, SCL_PIN);

  LOGI("Init sensor AHT10...");
  ahtOk = aht.begin(&Wire);
  LOGI("  AHT10: %s", ahtOk ? "OK" : "FAIL");

  LOGI("Init sensor GY-68 (BMP180)...");
  bmpOk = bmp.begin(BMP085_ULTRAHIGHRES, &Wire);
  LOGI("  GY-68: %s", bmpOk ? "OK" : "FAIL");

  LOGI("Init RTC DS1307...");
  rtcOk = rtc.begin(&Wire);
  LOGI("  RTC: %s", rtcOk ? "OK" : "FAIL");

  if (rtcOk && !rtc.isrunning()) {
    LOGW("RTC belum jalan, set dari compile time");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  LOGI("Load settings dari NVS...");
  loadSettings();
  LOGI("  Device name: %s", deviceName.c_str());
  LOGD("  WiFi1: %s", wifiSsid1.c_str());
  LOGD("  WiFi2: %s", wifiSsid2.c_str());
  LOGD("  NTP: %s / %s / %s",
       ntpServerLocal.c_str(), ntpServerInet1.c_str(), ntpServerInet2.c_str());
  LOGD("  Verbose debug: %s", verboseDebug ? "ON" : "OFF");

  LOGI("Setup network...");
  setupNetwork();

  if (!isApMode) {
    LOGI("Sync RTC dari NTP...");
    syncRTCFromNTP();
    lastNTPResync = millis();
    LOGI("Cek update firmware dari GitHub...");
    checkGitHubUpdate();
    lastUpdateCheck = millis();
  }

  LOGI("Start UDP NTP server (port %d)...", NTP_PORT);
  udpServer.begin(NTP_PORT);

  LOGI("Daftarkan route HTTP...");
  server.on("/", handleRoot);
  server.on("/api/data", handleRoot);
  server.on("/api/time", handleTimeApi);
  server.on("/api/ping", handlePingApi);
  server.on("/config", handleConfig);
  server.on("/logout", handleLogout);
  server.on("/restart", HTTP_POST, handleRestart);
  server.on("/do_online_update", HTTP_POST, handleDoOnlineUpdate);
  server.on("/serial", handleSerialPage);
  server.on("/logs", handleLogsJson);

  server.on("/update", HTTP_POST, []() {
    logHttpRequest("handleUpdate");
    if (!server.authenticate(configUser.c_str(), configPass.c_str())) {
      return server.requestAuthentication();
    }
    if (Update.hasError()) {
      isSystemUpdatingOrRebooting = false;
      lastUpdateError = getOTAErrorMessage(0, Update.getError());
      LOGE("Manual OTA gagal: %s", lastUpdateError.c_str());
      server.send(200, "text/html",
        "<h3>Manual Update Gagal!</h3><p>" + lastUpdateError + "</p><a href='/config'>Kembali</a>");
      wdtResume();
      return;
    }
    lastUpdateError = "";
    server.sendHeader("Connection", "close");
    server.send(200, "text/html",
      getRebootWaitingHtmlPage("Manual OTA Flashing",
        "Upload selesai! Merestart perangkat..."));
    blinkBlueLedFast(15, 30);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      isSystemUpdatingOrRebooting = true;
      lastUpdateError = "";
      ledBiruOn();
      wdtSuspend();
      LOGI("Manual OTA upload dimulai: %s", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        lastUpdateError = getOTAErrorMessage(0, Update.getError());
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      digitalWrite(LED_BIRU_PIN, !digitalRead(LED_BIRU_PIN));
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        lastUpdateError = getOTAErrorMessage(0, Update.getError());
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      ledBiruOff();
      if (!Update.end(true)) {
        lastUpdateError = getOTAErrorMessage(0, Update.getError());
        LOGE("Manual OTA error: %s", lastUpdateError.c_str());
      } else {
        LOGI("Manual OTA sukses, restarting...");
      }
    }
  });

  LOGI("Start HTTP server...");
  server.begin();
  LOGI("Web Server siap!");
  LOGI("Serial Monitor: http://%s/serial",
       (isApMode ? WiFi.softAPIP().toString().c_str() : WiFi.localIP().toString().c_str()));
  LOGI("Config Page:    http://%s/config",
       (isApMode ? WiFi.softAPIP().toString().c_str() : WiFi.localIP().toString().c_str()));
  LOGI("=== BOOT SELESAI ===");
}

// ===================== LOOP =====================
void loop() {
  wdtFeed();
  server.handleClient();
  handleNTPServer();
  updateLEDIndicators();

  if (millis() - lastWiFiCheck >= WIFI_CHECK_INTERVAL_MS) {
    lastWiFiCheck = millis();
    checkWiFiConnection();
  }
  if (!isApMode && WiFi.status() == WL_CONNECTED &&
      (millis() - lastNTPResync >= RESYNC_INTERVAL_MS)) {
    lastNTPResync = millis();
    LOGI("Resync NTP berkala (6 jam)");
    syncRTCFromNTP();
  }
  if (!isApMode && WiFi.status() == WL_CONNECTED &&
      (millis() - lastUpdateCheck >= CHECK_UPDATE_INTERVAL_MS)) {
    lastUpdateCheck = millis();
    LOGI("Auto cek update (6 jam)");
    checkGitHubUpdate();
  }
}