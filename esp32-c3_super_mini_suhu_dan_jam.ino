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
#include <Preferences.h>      // Memori NVS Flash
#include <Update.h>           // Firmware OTA Manual Update
#include <HTTPClient.h>       // HTTP Client untuk Check Update
#include <WiFiClientSecure.h> // Secure Client HTTPS GitHub
#include <HTTPUpdate.h>       // Auto OTA via URL Direct (.bin)
#include <ArduinoJson.h>      // Library JSON Parser

// --- VERSI FIRMWARE ---
#define FIRMWARE_VERSION     "1.0.6"
#define FIRMWARE_VERSION_CODE 106 // Integer komparasi versi

// URL GitHub Raw Konfigurasi Update
const char* GITHUB_UPDATE_URL = "https://raw.githubusercontent.com/fahrulariza/esp32-weather_station_local/refs/heads/update/update.json";

// --- PIN ALOKASI ESP32-C3 SUPER MINI ---
#define SDA_PIN       6
#define SCL_PIN       5
#define LED_MERAH_PIN 7  // Active HIGH (HIGH = Nyala, LOW = Mati)
#define LED_BIRU_PIN  8  // Active LOW  (LOW = Nyala, HIGH = Mati)

// --- VARIABEL KONFIGURASI DINAMIS (NVS PREFERENCES) ---
String deviceName;
String wifiSsid1, wifiPass1;
String wifiSsid2, wifiPass2;
String apSsid, apPass;
String configUser, configPass;
String ntpServerLocal, ntpServerInet1, ntpServerInet2;
bool enableRedLed = true; // Konfigurasi On/Off LED Merah

const long  GMT_OFFSET_SEC      = 7 * 3600; // WIB (UTC+7)
const int   DAYLIGHT_OFFSET_SEC = 0;

// Interval Resync NTP Otomatis (12 Jam)
const unsigned long RESYNC_INTERVAL_MS = 12UL * 3600UL * 1000UL; 
unsigned long lastNTPResync = 0;

// Interval Auto Check Update GitHub (6 Jam)
const unsigned long CHECK_UPDATE_INTERVAL_MS = 6UL * 3600UL * 1000UL;
unsigned long lastUpdateCheck = 0;

// Status Online Update GitHub
bool isUpdateAvailable = false;
String newVersionStr = "";
int newVersionCode = 0;
String newBinUrl = "";
String changelogUrl = "";
String updateCheckStatusMsg = "";

// Status Error Update Rinci untuk Web UI
String lastUpdateError = "";

// Status Mode Flashing / Restarting
bool isSystemUpdatingOrRebooting = false;

// Global Objects
WebServer server(80);
WiFiUDP udpServer;
const unsigned int NTP_PORT = 123;

Adafruit_AHTX0 aht;
Adafruit_BMP085 bmp;
RTC_DS1307 rtc;
Preferences preferences;

// Status Hardware & Mode Network
bool ahtOk = false;
bool bmpOk = false;
bool rtcOk = false;
bool isApMode = false;

// --- HELPER KEDIPAN KONEKSI/TRANSAKSI DATA LED BIRU ---
inline void ledBiruOn() {
  digitalWrite(LED_BIRU_PIN, LOW); // Active LOW -> NYALA
}

inline void ledBiruOff() {
  digitalWrite(LED_BIRU_PIN, HIGH); // Active LOW -> MATI
}

// Fungsi kedip cepat saat flashing/reboot
void blinkBlueLedFast(int times = 10, int speedMs = 50) {
  for (int i = 0; i < times; i++) {
    ledBiruOn();
    delay(speedMs);
    ledBiruOff();
    delay(speedMs);
  }
}

// MACRO / HELPER UNTUK INDIKASI AKTIVITAS DATA TRANSMISI
class DataActivityPulse {
public:
  DataActivityPulse() {
    if (!isSystemUpdatingOrRebooting) {
      ledBiruOn(); // Nyalakan LED saat data mulai diproses/dikirim
    }
  }
  ~DataActivityPulse() {
    if (!isSystemUpdatingOrRebooting) {
      delay(15);   // Beri jeda minim 15ms agar kedipan terlihat jelas oleh mata
      ledBiruOff(); // Matikan LED setelah respon selesai dikirim
    }
  }
};

// --- FUNGSI PENERJEMAH ERROR OTA UNTUK USER ---
String getOTAErrorMessage(int httpErrCode, uint8_t updateErrCode = 0) {
  if (updateErrCode > 0) {
    switch (updateErrCode) {
      case UPDATE_ERROR_WRITE:
        return "Gagal menulis firmware ke memori Flash (Flash Write Error).";
      case UPDATE_ERROR_ERASE:
        return "Gagal mengosongkan sektor memori Flash (Flash Erase Error).";
      case UPDATE_ERROR_READ:
        return "Gagal membaca data dari memori Flash.";
      case UPDATE_ERROR_SPACE:
        return "Sisa ruang memori Flash tidak cukup untuk file .bin ini.";
      case UPDATE_ERROR_SIZE:
        return "Ukuran file firmware terlalu besar atau tidak valid.";
      case UPDATE_ERROR_STREAM:
        return "Aliran data (Stream) file .bin terputus di tengah jalan.";
      case UPDATE_ERROR_MD5:
        return "Verifikasi Hash MD5 gagal! File .bin korup atau terdistorsi.";
      case UPDATE_ERROR_MAGIC_BYTE:
        return "File bukan firmware asli ESP32 yang valid (Magic Byte Error).";
      case UPDATE_ERROR_ACTIVATE:
        return "Gagal mengaktifkan partisi booting baru.";
      case UPDATE_ERROR_NO_PARTITION:
        return "Partisi OTA pada chip ESP32 tidak ditemukan.";
      default:
        return "Error Flashing Kode #" + String(updateErrCode);
    }
  }

  switch (httpErrCode) {
    case HTTPC_ERROR_CONNECTION_REFUSED:
      return "Koneksi ditolak oleh server GitHub/Hosting.";
    case HTTPC_ERROR_SEND_HEADER_FAILED:
    case HTTPC_ERROR_SEND_PAYLOAD_FAILED:
      return "Gagal mengirim permintaan pengunduhan file .bin.";
    case HTTPC_ERROR_NOT_CONNECTED:
      return "Perangkat terputus dari jaringan WiFi saat mengunduh.";
    case HTTPC_ERROR_CONNECTION_LOST:
      return "Koneksi internet terputus di tengah proses unduh.";
    case HTTPC_ERROR_NO_STREAM:
      return "Server tidak memberikan respon stream data .bin.";
    case HTTPC_ERROR_NO_HTTP_SERVER:
      return "Server hosting firmware tidak ditemukan / Offline.";
    case HTTPC_ERROR_TOO_LESS_RAM:
      return "Sisa memori RAM ESP32 terlalu sedikit untuk melakukan update.";
    case HTTPC_ERROR_ENCODING:
      return "Format enkoding transfer HTTP tidak didukung.";
    case HTTPC_ERROR_STREAM_WRITE:
      return "Gagal menulis aliran data unduhan ke memori.";
    case HTTPC_ERROR_READ_TIMEOUT:
      return "Waktu unduh habis (Read Timeout). Koneksi internet lambat.";
    default:
      if (httpErrCode > 0) {
        return "HTTP Status Error " + String(httpErrCode) + " (Contoh: 404 File Tidak Ditemukan).";
      }
      return "Terjadi kesalahan unduh HTTP (Kode Error: " + String(httpErrCode) + ").";
  }
}

// --- FUNGSI LOAD & SAVE NVS PREFERENCES ---
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

// --- FUNGSI LOGIKA PENGECEKAN UPDATE GITHUB ---
void checkGitHubUpdate() {
  if (isApMode || WiFi.status() != WL_CONNECTED) {
    updateCheckStatusMsg = "Koneksi internet tidak tersedia.";
    return;
  }

  DataActivityPulse activity; // Kedipkan LED Biru saat unduh JSON GitHub

  Serial.println("\n🔍 Memeriksa pembaruan firmware dari GitHub...");
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (http.begin(client, GITHUB_UPDATE_URL)) {
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, payload);

      if (!error) {
        newVersionStr  = doc["version"].as<String>();
        newVersionCode = doc["versionCode"].as<int>();
        newBinUrl      = doc["zipUrl"].as<String>();
        changelogUrl   = doc["changelog"].as<String>();

        if (newVersionCode > FIRMWARE_VERSION_CODE) {
          isUpdateAvailable = true;
          updateCheckStatusMsg = "Versi baru ditemukan: " + newVersionStr;
        } else {
          isUpdateAvailable = false;
          updateCheckStatusMsg = "Firmware sudah menggunakan versi terbaru (v" FIRMWARE_VERSION ").";
        }
      } else {
        updateCheckStatusMsg = "Gagal memproses data JSON dari GitHub.";
      }
    } else {
      updateCheckStatusMsg = "Gagal terhubung ke GitHub. HTTP Code: " + String(httpCode);
    }
    http.end();
  } else {
    updateCheckStatusMsg = "Tidak dapat menginisialisasi koneksi HTTPS.";
  }
}

// --- FUNGSI EKSEKUSI ONLINE OTA UPDATE ---
void performOnlineOTA() {
  if (newBinUrl.length() == 0) return;

  isSystemUpdatingOrRebooting = true;
  lastUpdateError = "";
  Serial.println("\n🚀 Memulai proses Flash OTA Online dari URL...");
  
  WiFiClientSecure client;
  client.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  blinkBlueLedFast(15, 40);
  
  t_httpUpdate_return ret = httpUpdate.update(client, newBinUrl);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      isSystemUpdatingOrRebooting = false;
      lastUpdateError = getOTAErrorMessage(httpUpdate.getLastError());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      isSystemUpdatingOrRebooting = false;
      lastUpdateError = "Tidak ada file pembaruan yang diproses oleh server.";
      break;
    case HTTP_UPDATE_OK:
      lastUpdateError = "";
      blinkBlueLedFast(20, 30);
      break;
  }
}

// --- FUNGSI KONEKSI WIFI BERTINGKAT ---
bool connectWiFiTarget(String ssid, String pass) {
  if (ssid.length() == 0) return false;

  Serial.printf("\n[WiFi] Memulai koneksi ke SSID: %s\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(deviceName.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());

  for (int attempt = 1; attempt <= 4; attempt++) {
    unsigned long startWait = millis();
    while (millis() - startWait < 30000) {
      if (WiFi.status() == WL_CONNECTED) {
        return true;
      }
      delay(500);
    }
  }
  return false;
}

void setupNetwork() {
  if (connectWiFiTarget(wifiSsid1, wifiPass1)) {
    isApMode = false;
    digitalWrite(LED_MERAH_PIN, LOW);
    return;
  }

  if (connectWiFiTarget(wifiSsid2, wifiPass2)) {
    isApMode = false;
    digitalWrite(LED_MERAH_PIN, LOW);
    return;
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid.c_str(), apPass.c_str());
  isApMode = true;
  
  if (enableRedLed) {
    digitalWrite(LED_MERAH_PIN, HIGH);
  } else {
    digitalWrite(LED_MERAH_PIN, LOW);
  }
}

// --- FUNGSI UPDATE RTC DARI NTP SERVER ---
void syncRTCFromNTP() {
  if (isApMode) return;
  DataActivityPulse activity; // Kedipkan LED Biru saat sync NTP Internet

  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, ntpServerLocal.c_str(), ntpServerInet1.c_str(), ntpServerInet2.c_str());

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 5000)) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint32_t microsToNextSecond = 1000000 - tv.tv_usec;
    if (microsToNextSecond < 1000000) delayMicroseconds(microsToNextSecond);

    time_t ntpSecs;
    time(&ntpSecs);
    ntpSecs += 1;

    if (rtcOk) {
      rtc.adjust(DateTime(ntpSecs));
    }
  }
}

// --- FUNGSI LOCAL NTP SERVER (UDP PORT 123) ---
void handleNTPServer() {
  int packetSize = udpServer.parsePacket();
  if (packetSize >= 48) {
    DataActivityPulse activity; // Kedipkan LED Biru saat merespon NTP Lokal

    byte requestBuffer[48];
    udpServer.read(requestBuffer, 48);

    DateTime now = rtcOk ? rtc.now() : DateTime(0UL);
    uint32_t epochSecs = now.unixtime();
    uint32_t ntpSecs = epochSecs + 2208988800UL;

    byte responseBuffer[48];
    memset(responseBuffer, 0, 48);

    responseBuffer[0] = 0x24;
    responseBuffer[1] = 1;
    responseBuffer[2] = 6;
    responseBuffer[3] = 0xEC;

    responseBuffer[7]  = 0x10;
    responseBuffer[11] = 0xA0;

    responseBuffer[12] = 'R'; responseBuffer[13] = 'T';
    responseBuffer[14] = 'C'; responseBuffer[15] = ' ';

    for (int i = 0; i < 8; i++) responseBuffer[24 + i] = requestBuffer[40 + i];

    responseBuffer[32] = (ntpSecs >> 24) & 0xFF; responseBuffer[33] = (ntpSecs >> 16) & 0xFF;
    responseBuffer[34] = (ntpSecs >> 8) & 0xFF;  responseBuffer[35] = ntpSecs & 0xFF;
    responseBuffer[40] = (ntpSecs >> 24) & 0xFF; responseBuffer[41] = (ntpSecs >> 16) & 0xFF;
    responseBuffer[42] = (ntpSecs >> 8) & 0xFF;  responseBuffer[43] = ntpSecs & 0xFF;

    udpServer.beginPacket(udpServer.remoteIP(), udpServer.remotePort());
    udpServer.write(responseBuffer, 48);
    udpServer.endPacket();
  }
}

// --- REST API ENDPOINT UTAMA (/) ---
void handleRoot() {
  DataActivityPulse activity; // Kedip LED Biru saat memproses API Suhu/Waktu

  sensors_event_t humidity, temp_aht;
  float t_aht = 0, hum = 0, t_bmp = 0, pres = 0, alt = 0;

  if (ahtOk) {
    aht.getEvent(&humidity, &temp_aht);
    t_aht = temp_aht.temperature;
    hum = humidity.relative_humidity;
  }

  if (bmpOk) {
    t_bmp = bmp.readTemperature();
    float p_pa = bmp.readPressure();
    pres = p_pa / 100.0;
    alt = 44330.0 * (1.0 - pow(p_pa / 101325.0, 0.1902949));
  }

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
  jsonPayload += "\"temperature_aht10\":" + String(t_aht, 1) + ",";
  jsonPayload += "\"humidity\":" + String(hum, 1) + ",";
  jsonPayload += "\"temperature_gy68\":" + String(t_bmp, 1) + ",";
  jsonPayload += "\"pressure_hpa\":" + String(pres, 2) + ",";
  jsonPayload += "\"altitude_m\":" + String(alt, 1);
  jsonPayload += "}";

  server.send(200, "application/json", jsonPayload);
}

// --- REST API ENDPOINT TIME (/api/time) ---
void handleTimeApi() {
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

// --- ENDPOINT UNTUK AUTO-DETECT REBOOT/PING (/api/ping) ---
void handlePingApi() {
  DataActivityPulse activity;
  server.send(200, "application/json", "{\"status\":\"ok\",\"version\":\"" FIRMWARE_VERSION "\"}");
}

// --- LOGOUT HANDLER ---
void handleLogout() {
  DataActivityPulse activity;
  server.sendHeader("WWW-Authenticate", "Basic realm=\"Login Required\"");
  server.send(401, "text/html", "<meta charset='utf-8'><div style='font-family:sans-serif;text-align:center;margin-top:50px;'><h2>Anda telah logout.</h2><p><a href='/config'>Klik di sini untuk login kembali</a></p></div>");
}

// --- HELPER TEMPLATE HTML REBOOT / UPDATE WAITING ---
String getRebootWaitingHtmlPage(String title, String statusMsg) {
  String html = "<!DOCTYPE html><html lang='id'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
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
  html += "      document.getElementById('status').innerText = 'Perangkat berhasil terhubung! Memuat halaman login...';";
  html += "      setTimeout(function(){ location.href = '/config'; }, 1500);";
  html += "    }";
  html += "  }).catch(function(e){});";
  html += "}, 2000);";
  html += "</script></div></body></html>";
  return html;
}

// --- HANDLER TRIGGER EXECUTE ONLINE UPDATE ---
void handleDoOnlineUpdate() {
  if (!server.authenticate(configUser.c_str(), configPass.c_str())) {
    return server.requestAuthentication();
  }

  if (!isUpdateAvailable || newBinUrl.length() == 0) {
    server.send(400, "text/html", "<h3>Tidak ada pembaruan firmware yang dapat diinstal.</h3><a href='/config'>Kembali</a>");
    return;
  }

  server.send(200, "text/html", getRebootWaitingHtmlPage("Proses Online OTA Update", "Sedang mengunduh & menginstal firmware baru... Perangkat akan merestart secara otomatis."));
  
  delay(1000);
  performOnlineOTA();
}

// --- HALAMAN PENGATURAN (/config) ---
void handleConfig() {
  if (!server.authenticate(configUser.c_str(), configPass.c_str())) {
    return server.requestAuthentication();
  }

  DataActivityPulse activity; // Kedipkan LED Biru saat memproses request Web Config

  String msg = "";
  bool msgError = false;

  // Handler Trigger Manual Check Update
  if (server.hasArg("check_update_now")) {
    checkGitHubUpdate();
    msg = updateCheckStatusMsg;
    msgError = !isUpdateAvailable;
  }

  // Submit Device Name
  if (server.hasArg("save_dev")) {
    deviceName = server.arg("dev_name");
    saveSettingsString("dev_name", deviceName);
    msg = "Nama Perangkat berhasil diperbarui!";
  }

  // Submit WiFi Target 1 & 2 & AP Hotspot & LED Merah
  if (server.hasArg("save_wifi")) {
    wifiSsid1 = server.arg("ssid1"); wifiPass1 = server.arg("wpass1");
    wifiSsid2 = server.arg("ssid2"); wifiPass2 = server.arg("wpass2");
    apSsid    = server.arg("ap_ssid"); apPass   = server.arg("ap_pass");
    
    enableRedLed = server.hasArg("enable_red_led");
    saveSettingsBool("led_red", enableRedLed);

    if (!enableRedLed || !isApMode) {
      digitalWrite(LED_MERAH_PIN, LOW);
    } else if (enableRedLed && isApMode) {
      digitalWrite(LED_MERAH_PIN, HIGH);
    }

    saveSettingsString("ssid1", wifiSsid1);   saveSettingsString("wpass1", wifiPass1);
    saveSettingsString("ssid2", wifiSsid2);   saveSettingsString("wpass2", wifiPass2);
    saveSettingsString("ap_ssid", apSsid);     saveSettingsString("ap_pass", apPass);
    msg = "Pengaturan Network & LED disimpan!";
  }

  // Submit Form NTP
  if (server.hasArg("save_ntp")) {
    ntpServerLocal = server.arg("ntp1");
    ntpServerInet1 = server.arg("ntp2");
    ntpServerInet2 = server.arg("ntp3");
    saveSettingsString("ntp1", ntpServerLocal);
    saveSettingsString("ntp2", ntpServerInet1);
    saveSettingsString("ntp3", ntpServerInet2);
    syncRTCFromNTP();
    msg = "Pengaturan NTP berhasil disimpan dan disinkronkan!";
  }

  // Submit Form Keamanan Login
  if (server.hasArg("save_auth")) {
    String oldPass = server.arg("old_pass");
    String newUser = server.arg("new_user");
    String newPass = server.arg("new_pass");

    if (oldPass == configPass) {
      if (newUser.length() > 0 && newPass.length() > 0) {
        configUser = newUser; configPass = newPass;
        saveSettingsString("user", newUser);
        saveSettingsString("pass", newPass);
        msg = "Kredensial login berhasil diperbarui!";
      } else {
        msg = "Username & Password baru tidak boleh kosong!"; msgError = true;
      }
    } else {
      msg = "Gagal! Password Lama tidak cocok."; msgError = true;
    }
  }

  // Kalkulasi Informasi Sistem
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t totalHeap = ESP.getHeapSize();
  uint32_t usedHeap = totalHeap - freeHeap;
  float heapUsagePct = ((float)usedHeap / (float)totalHeap) * 100.0;
  
  unsigned long uptimeSec = millis() / 1000;
  unsigned int days = uptimeSec / 86400;
  unsigned int hours = (uptimeSec % 86400) / 3600;
  unsigned int minutes = (uptimeSec % 3600) / 60;
  unsigned int seconds = uptimeSec % 60;

  // HTML Render
  String html = "<!DOCTYPE html><html lang='id'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>" + deviceName + " - Config</title>";
  html += "<style>body{font-family:Segoe UI,Tahoma,sans-serif;margin:20px;background:#f4f4f9;color:#333}";
  html += ".card{background:#fff;padding:22px;border-radius:10px;box-shadow:0 3px 8px rgba(0,0,0,0.12);max-width:480px;margin:auto}";
  html += "h2{color:#007bff;margin-top:0;font-size:22px;} h3{font-size:15px;margin-top:18px;margin-bottom:8px;color:#444;} label{font-weight:600;display:block;margin-top:10px;font-size:13px}";
  html += "input[type=text],input[type=password],input[type=file]{width:100%;padding:9px;margin-top:4px;box-sizing:border-box;border:1px solid #ccc;border-radius:5px;font-size:14px}";
  html += ".checkbox-container{display:flex;align-items:center;margin-top:12px;font-size:13px;font-weight:600;cursor:pointer;} .checkbox-container input{width:auto;margin-right:8px;}";
  html += "button,.btn{background:#007bff;color:#fff;border:none;padding:10px 15px;margin-top:14px;border-radius:5px;cursor:pointer;width:100%;font-weight:bold;font-size:14px;text-align:center;text-decoration:none;display:inline-block;box-sizing:border-box}";
  html += ".btn-danger{background:#dc3545}.btn-warning{background:#ffc107;color:#000}.btn-success{background:#28a745}.btn-outline{background:none;border:1px solid #007bff;color:#007bff}";
  html += ".ok{color:#28a745;font-weight:bold}.err{color:#dc3545;font-weight:bold}";
  html += ".alert{padding:12px;border-radius:5px;margin-bottom:15px;font-size:13px;font-weight:bold}";
  html += ".alert-success{background:#d4edda;color:#155724;border:1px solid #c3e6cb}";
  html += ".alert-danger{background:#f8d7da;color:#721c24;border:1px solid #f5c6cb}";
  html += ".alert-info{background:#d1ecf1;color:#0c5460;border:1px solid #bee5eb}";
  html += ".info-table{width:100%;border-collapse:collapse;margin-top:8px;font-size:13px}.info-table td{padding:5px 0;border-bottom:1px solid #eee}</style>";
  
  html += "<script>";
  html += "var timeout;";
  html += "function autoLogout(){";
  html += "  fetch('/logout').then(function(){";
  html += "    alert('Sesi berakhir karena tidak ada aktivitas selama 2 menit.');";
  html += "    location.href='/config';";
  html += "  });";
  html += "}";
  html += "function resetTimer(){";
  html += "  clearTimeout(timeout);";
  html += "  timeout = setTimeout(autoLogout, 120000);";
  html += "}";
  html += "window.onload=resetTimer;";
  html += "window.onmousemove=resetTimer;";
  html += "window.onmousedown=resetTimer;";
  html += "window.onclick=resetTimer;";
  html += "window.onkeydown=resetTimer;";
  html += "</script>";
  html += "</head><body>";
  
  html += "<div class='card'>";
  html += "<div style='display:flex;justify-content:space-between;align-items:center;'><h2>&#9881;&#65039; Config</h2><button onclick='fetch(\"/logout\").then(function(){location.href=\"/config\";});' style='margin:0;padding:6px 12px;width:auto;' class='btn btn-warning'>Logout</button></div>";

  if (lastUpdateError.length() > 0) {
    html += "<div class='alert alert-danger' style='margin-top:15px;'>";
    html += "⚠️ <b>Gagal Memasang Firmware!</b><br>";
    html += "Penyebab: <u>" + lastUpdateError + "</u><br>";
    html += "<span style='font-size:11px;font-weight:normal;'>Perangkat telah membatalkan proses update secara aman tanpa merusak firmware saat ini.</span>";
    html += "</div>";
  }

  if (msg.length() > 0) {
    html += "<div class='alert " + String(msgError ? "alert-danger" : "alert-success") + "' style='margin-top:15px;'>" + msg + "</div>";
  }

  if (isUpdateAvailable) {
    html += "<div class='alert alert-success' style='margin-top:15px;'>";
    html += "🚀 <b>Pembaruan Firmware Ditemukan!</b><br>";
    html += "Versi Terbaru: <b>" + newVersionStr + "</b><br>";
    if (changelogUrl.length() > 0) {
      html += "Catatan Rilis: <a href='" + changelogUrl + "' target='_blank' style='color:#155724;text-decoration:underline;'>Lihat Changelog GitHub</a><br>";
    }
    html += "<form method='POST' action='/do_online_update'>";
    html += "<button type='submit' class='btn btn-success' onclick='return confirm(\"Pasang firmware " + newVersionStr + " secara online?\")'>&#11015;&#65039; Update Online Sekarang</button>";
    html += "</form>";
    html += "</div>";
  }

  html += "<h3>&#128187; Informasi Sistem & Memori</h3>";
  html += "<table class='info-table'>";
  html += "<tr><td><b>Nama Device:</b></td><td>" + deviceName + "</td></tr>";
  html += "<tr><td><b>Versi Firmware:</b></td><td><span class='ok'>v" FIRMWARE_VERSION "</span> (Code: " + String(FIRMWARE_VERSION_CODE) + ")</td></tr>";
  html += "<tr><td><b>Status Update:</b></td><td>" + String(isUpdateAvailable ? "<span class='err'>Tersedia versi " + newVersionStr + "</span>" : "<span class='ok'>Up to date</span>") + "</td></tr>";
  html += "<tr><td><b>Mode Network:</b></td><td>" + String(isApMode ? "<span class='err'>AP Hotspot Mode</span>" : "<span class='ok'>STA Connected Mode</span>") + "</td></tr>";
  html += "<tr><td><b>IP Address:</b></td><td>" + (isApMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "</td></tr>";
  html += "<tr><td><b>LED Merah (Failover):</b></td><td>" + String(enableRedLed ? "<span class='ok'>Aktif</span>" : "<span class='err'>Matikan (Off)</span>") + "</td></tr>";
  html += "<tr><td><b>Sisa RAM (Free Heap):</b></td><td>" + String(freeHeap / 1024.0, 1) + " KB / " + String(totalHeap / 1024.0, 1) + " KB (" + String(heapUsagePct, 1) + "% terpakai)</td></tr>";
  html += "<tr><td><b>Min Free Heap Ever:</b></td><td>" + String(ESP.getMinFreeHeap() / 1024.0, 1) + " KB</td></tr>";
  html += "<tr><td><b>Uptime Perangkat:</b></td><td>" + String(days) + "d " + String(hours) + "h " + String(minutes) + "m " + String(seconds) + "s</td></tr>";
  html += "<tr><td><b>Frekuensi CPU / Flash:</b></td><td>" + String(ESP.getCpuFreqMHz()) + " MHz / " + String(ESP.getFlashChipSize() / (1024 * 1024)) + " MB</td></tr>";
  html += "</table>";

  html += "<form method='POST'>";
  html += "<button type='submit' name='check_update_now' value='1' class='btn btn-outline'>&#128260; Cek Update Baru dari GitHub</button>";
  html += "</form>";
  
  html += "<h3>&#128268; Status Hardware</h3>";
  html += "<p style='font-size:13px;margin:3px 0;'>AHT10 Sensor: " + String(ahtOk ? "<span class='ok'>[ONLINE]</span>" : "<span class='err'>[OFFLINE]</span>") + "</p>";
  html += "<p style='font-size:13px;margin:3px 0;'>GY-68 (BMP180): " + String(bmpOk ? "<span class='ok'>[ONLINE]</span>" : "<span class='err'>[OFFLINE]</span>") + "</p>";
  html += "<p style='font-size:13px;margin:3px 0;'>DS1307 RTC: " + String(rtcOk ? "<span class='ok'>[ONLINE]</span>" : "<span class='err'>[OFFLINE]</span>") + "</p>";
  html += "<hr>";

  html += "<h3>&#127991;&#65039; Nama Perangkat (Device Hostname)</h3>";
  html += "<form method='POST'>";
  html += "<label>Device Hostname:</label><input type='text' name='dev_name' value='" + deviceName + "' required>";
  html += "<button type='submit' name='save_dev' value='1'>Simpan Nama Device</button>";
  html += "</form>";
  html += "<hr>";

  html += "<h3>&#128246; Pengaturan Jaringan WiFi & Hardware</h3>";
  html += "<form method='POST'>";
  html += "<label><b>WiFi Target 1 (Utama):</b></label>";
  html += "<input type='text' name='ssid1' placeholder='SSID Target 1' value='" + wifiSsid1 + "' required>";
  html += "<input type='password' name='wpass1' placeholder='Password Target 1' value='" + wifiPass1 + "'>";
  
  html += "<label style='margin-top:12px;'><b>WiFi Target 2 (Cadangan):</b></label>";
  html += "<input type='text' name='ssid2' placeholder='SSID Target 2' value='" + wifiSsid2 + "'>";
  html += "<input type='password' name='wpass2' placeholder='Password Target 2' value='" + wifiPass2 + "'>";

  html += "<label style='margin-top:12px;'><b>Auto AP Hotspot (Fallback):</b></label>";
  html += "<input type='text' name='ap_ssid' placeholder='SSID Hotspot ESP32' value='" + apSsid + "' required>";
  html += "<input type='password' name='ap_pass' placeholder='Password Hotspot (Min 8 karakter)' value='" + apPass + "' required>";

  html += "<label class='checkbox-container'>";
  html += "<input type='checkbox' name='enable_red_led' value='1' " + String(enableRedLed ? "checked" : "") + ">";
  html += "Aktifkan LED Merah Indikator WiFi Failover";
  html += "</label>";

  html += "<button type='submit' name='save_wifi' value='1'>Simpan WiFi & Pengaturan LED</button>";
  html += "</form>";
  html += "<hr>";

  html += "<h3>&#128336; Pengaturan Server NTP</h3>";
  html += "<form method='POST'>";
  html += "<label>NTP Utama (Lokal):</label><input type='text' name='ntp1' value='" + ntpServerLocal + "'>";
  html += "<label>NTP Kedua (Internet):</label><input type='text' name='ntp2' value='" + ntpServerInet1 + "'>";
  html += "<label>NTP Ketiga (Internet):</label><input type='text' name='ntp3' value='" + ntpServerInet2 + "'>";
  html += "<button type='submit' name='save_ntp' value='1'>Simpan & Sync NTP</button>";
  html += "</form>";
  html += "<hr>";

  html += "<h3>&#128274; Ubah Kredensial Login Web UI</h3>";
  html += "<form method='POST'>";
  html += "<label>User Baru:</label><input type='text' name='new_user' value='" + configUser + "' required>";
  html += "<label>Password Baru:</label><input type='password' name='new_pass' placeholder='Password Baru' required>";
  html += "<label>Password Lama (Konfirmasi):</label><input type='password' name='old_pass' placeholder='Password Saat Ini' required>";
  html += "<button type='submit' name='save_auth' value='1'>Ubah User & Password</button>";
  html += "</form>";
  html += "<hr>";

  html += "<h3>&#128230; Manual Firmware Update (File .bin)</h3>";
  html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
  html += "<input type='file' name='update' accept='.bin' required>";
  html += "<button type='submit' class='btn'>Upload Local Firmware (.bin)</button>";
  html += "</form>";
  html += "<hr>";

  html += "<form method='POST' action='/restart'>";
  html += "<button type='submit' class='btn btn-danger' onclick='return confirm(\"Restart ESP32?\")'>Restart ESP32</button>";
  html += "</form>";

  html += "</div></body></html>";

  server.send(200, "text/html", html);
}

// --- HANDLER RESTART ESP32 ---
void handleRestart() {
  if (!server.authenticate(configUser.c_str(), configPass.c_str())) {
    return server.requestAuthentication();
  }
  
  isSystemUpdatingOrRebooting = true;
  server.send(200, "text/html", getRebootWaitingHtmlPage("Merekam Ulang ESP32", "Sedang memproses perintah restart..."));
  
  delay(500);
  blinkBlueLedFast(20, 40);
  ESP.restart();
}

// --- LOGIKA KONTROL LED INDIKATOR ---
void updateLEDIndicators() {
  if (isSystemUpdatingOrRebooting) return;

  // Kontrol LED Merah Failover
  if (!enableRedLed || (WiFi.status() == WL_CONNECTED && !isApMode)) {
    digitalWrite(LED_MERAH_PIN, LOW);
  } else if (enableRedLed && isApMode) {
    digitalWrite(LED_MERAH_PIN, HIGH);
  }

  // Heartbeat LED Biru (5 Detik Sekali saat Idle)
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

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(LED_MERAH_PIN, OUTPUT);
  pinMode(LED_BIRU_PIN, OUTPUT);
  digitalWrite(LED_MERAH_PIN, LOW);
  ledBiruOff();

  Wire.begin(SDA_PIN, SCL_PIN);

  ahtOk = aht.begin(&Wire);
  bmpOk = bmp.begin(BMP085_ULTRAHIGHRES, &Wire);
  rtcOk = rtc.begin(&Wire);

  if (rtcOk && !rtc.isrunning()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  loadSettings();
  setupNetwork();

  if (!isApMode) {
    syncRTCFromNTP();
    lastNTPResync = millis();

    checkGitHubUpdate();
    lastUpdateCheck = millis();
  }

  udpServer.begin(NTP_PORT);

  server.on("/", handleRoot);
  server.on("/api/data", handleRoot);
  server.on("/api/time", handleTimeApi);
  server.on("/api/ping", handlePingApi);
  server.on("/config", handleConfig);
  server.on("/logout", handleLogout);
  server.on("/restart", HTTP_POST, handleRestart);
  server.on("/do_online_update", HTTP_POST, handleDoOnlineUpdate);

  // Manual OTA Upload Handler (/update)
  server.on("/update", HTTP_POST, []() {
    if (!server.authenticate(configUser.c_str(), configPass.c_str())) {
      return server.requestAuthentication();
    }
    
    if (Update.hasError()) {
      isSystemUpdatingOrRebooting = false;
      lastUpdateError = getOTAErrorMessage(0, Update.getError());
      server.send(200, "text/html", "<h3>Manual Update Gagal!</h3><p>" + lastUpdateError + "</p><a href='/config'>Kembali ke Config</a>");
      return;
    }

    lastUpdateError = "";
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", getRebootWaitingHtmlPage("Manual OTA Flashing", "Proses pengunggahan firmware selesai! Merestart perangkat..."));
    
    blinkBlueLedFast(15, 30);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      isSystemUpdatingOrRebooting = true;
      lastUpdateError = "";
      ledBiruOn();
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { 
        lastUpdateError = getOTAErrorMessage(0, Update.getError());
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      // Toggle LED cepat setiap chunk byte firmware ditulis ke flash
      digitalWrite(LED_BIRU_PIN, !digitalRead(LED_BIRU_PIN)); 
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        lastUpdateError = getOTAErrorMessage(0, Update.getError());
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      ledBiruOff();
      if (!Update.end(true)) {
        lastUpdateError = getOTAErrorMessage(0, Update.getError());
      }
    }
  });

  server.begin();
  Serial.printf("🚀 Web Server API v%s (Code %d) Siap!\n", FIRMWARE_VERSION, FIRMWARE_VERSION_CODE);
}

void loop() {
  server.handleClient();
  handleNTPServer();
  updateLEDIndicators();

  if (!isApMode && WiFi.status() == WL_CONNECTED && (millis() - lastNTPResync >= RESYNC_INTERVAL_MS)) {
    lastNTPResync = millis();
    syncRTCFromNTP();
  }

  if (!isApMode && WiFi.status() == WL_CONNECTED && (millis() - lastUpdateCheck >= CHECK_UPDATE_INTERVAL_MS)) {
    lastUpdateCheck = millis();
    checkGitHubUpdate();
  }
}