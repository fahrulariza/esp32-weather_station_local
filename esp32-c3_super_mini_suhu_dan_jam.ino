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
#include <Preferences.h>  // Memori NVS Flash
#include <Update.h>       // Firmware OTA Update

// --- VERSI FIRMWARE ---
#define FIRMWARE_VERSION "1.0.0"

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

const long  GMT_OFFSET_SEC      = 7 * 3600; // WIB (UTC+7)
const int   DAYLIGHT_OFFSET_SEC = 0;

// Interval Resync NTP Otomatis (12 Jam)
const unsigned long RESYNC_INTERVAL_MS = 12UL * 3600UL * 1000UL; 
unsigned long lastNTPResync = 0;

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

// --- FUNGSI LOAD & SAVE NVS PREFERENCES ---
void loadSettings() {
  preferences.begin("sys_config", true); // Mode Read-only
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
  preferences.end();
}

void saveSettingsString(const char* key, String val) {
  preferences.begin("sys_config", false);
  preferences.putString(key, val);
  preferences.end();
}

// --- FUNGSI KONEKSI WIFI BERTINGKAT (FAILOVER & AP MODE) ---
bool connectWiFiTarget(String ssid, String pass) {
  if (ssid.length() == 0) return false;

  Serial.printf("\n[WiFi] Memulai koneksi ke SSID: %s\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(deviceName.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());

  // Percobaan selama 2 menit (120 detik), interval cek per 30 detik
  for (int attempt = 1; attempt <= 4; attempt++) {
    Serial.printf("[WiFi] Percobaan ke-%d (Menunggu 30 detik)...\n", attempt);
    
    unsigned long startWait = millis();
    while (millis() - startWait < 30000) {
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] SUKSES Terhubung ke %s! IP: %s\n", ssid.c_str(), WiFi.localIP().toString().c_str());
        return true;
      }
      delay(500);
    }
  }
  
  Serial.printf("[WiFi] Gagal terhubung ke %s setelah 2 menit.\n", ssid.c_str());
  return false;
}

void setupNetwork() {
  // 1. Coba WiFi Target 1
  if (connectWiFiTarget(wifiSsid1, wifiPass1)) {
    isApMode = false;
    digitalWrite(LED_MERAH_PIN, LOW);
    return;
  }

  // 2. Coba WiFi Target 2
  if (connectWiFiTarget(wifiSsid2, wifiPass2)) {
    isApMode = false;
    digitalWrite(LED_MERAH_PIN, LOW);
    return;
  }

  // 3. Jika Kedua WiFi Gagal -> Nyalakan Access Point (Hotspot)
  Serial.println("\n[WiFi] WiFi 1 & 2 Gagal. Mengaktifkan Access Point Mode...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid.c_str(), apPass.c_str());
  isApMode = true;
  digitalWrite(LED_MERAH_PIN, HIGH); // LED Merah nyala saat AP Mode
  Serial.printf("[WiFi AP] Hotspot Aktif: %s | IP AP: %s\n", apSsid.c_str(), WiFi.softAPIP().toString().c_str());
}

// --- FUNGSI UPDATE RTC DARI NTP SERVER ---
void syncRTCFromNTP() {
  if (isApMode) return;

  Serial.println("\n🔄 Sync waktu dari NTP Server...");
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
      Serial.println("✅ Sync RTC dari NTP Berhasil!");
    }
  } else {
    Serial.println("⚠️️ Gagal mengambil waktu dari NTP!");
  }
}

// --- FUNGSI LOCAL NTP SERVER (UDP PORT 123) ---
void handleNTPServer() {
  int packetSize = udpServer.parsePacket();
  if (packetSize >= 48) {
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
  jsonPayload += "\"status\":\"success\",";
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

// --- LOGOUT HANDLER ---
void handleLogout() {
  server.sendHeader("WWW-Authenticate", "Basic realm=\"Login Required\"");
  server.send(401, "text/html", "<meta charset='utf-8'><div style='font-family:sans-serif;text-align:center;margin-top:50px;'><h2>Anda telah logout.</h2><p><a href='/config'>Klik di sini untuk login kembali</a></p></div>");
}

// --- HALAMAN PENGATURAN (/config) ---
void handleConfig() {
  if (!server.authenticate(configUser.c_str(), configPass.c_str())) {
    return server.requestAuthentication();
  }

  String msg = "";
  bool msgError = false;

  // 1. Submit Device Name
  if (server.hasArg("save_dev")) {
    deviceName = server.arg("dev_name");
    saveSettingsString("dev_name", deviceName);
    msg = "Nama Perangkat berhasil diperbarui!";
  }

  // 2. Submit WiFi Target 1 & 2 & AP Hotspot
  if (server.hasArg("save_wifi")) {
    wifiSsid1 = server.arg("ssid1"); wifiPass1 = server.arg("wpass1");
    wifiSsid2 = server.arg("ssid2"); wifiPass2 = server.arg("wpass2");
    apSsid    = server.arg("ap_ssid"); apPass   = server.arg("ap_pass");

    saveSettingsString("ssid1", wifiSsid1);   saveSettingsString("wpass1", wifiPass1);
    saveSettingsString("ssid2", wifiSsid2);   saveSettingsString("wpass2", wifiPass2);
    saveSettingsString("ap_ssid", apSsid);     saveSettingsString("ap_pass", apPass);
    msg = "Pengaturan Network disimpan! Berlaku setelah ESP32 di-restart.";
  }

  // 3. Submit Form NTP
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

  // 4. Submit Form Keamanan Login
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
  html += "button,.btn{background:#007bff;color:#fff;border:none;padding:10px 15px;margin-top:14px;border-radius:5px;cursor:pointer;width:100%;font-weight:bold;font-size:14px}";
  html += ".btn-danger{background:#dc3545}.btn-warning{background:#ffc107;color:#000}.ok{color:#28a745;font-weight:bold}.err{color:#dc3545;font-weight:bold}";
  html += ".alert{padding:10px;border-radius:5px;margin-bottom:15px;font-size:13px;font-weight:bold}";
  html += ".alert-success{background:#d4edda;color:#155724;border:1px solid #c3e6cb}";
  html += ".alert-danger{background:#f8d7da;color:#721c24;border:1px solid #f5c6cb}";
  html += ".info-table{width:100%;border-collapse:collapse;margin-top:8px;font-size:13px}.info-table td{padding:5px 0;border-bottom:1px solid #eee}</style>";
  
  // Script JavaScript Auto Logout 2 Menit Inaktivitas -> Redirect ke /config
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

  if (msg.length() > 0) {
    html += "<div class='alert " + String(msgError ? "alert-danger" : "alert-success") + "' style='margin-top:15px;'>" + msg + "</div>";
  }

  // Informasi Memori & Sistem
  html += "<h3>&#128187; Informasi Sistem & Memori</h3>";
  html += "<table class='info-table'>";
  html += "<tr><td><b>Nama Device:</b></td><td>" + deviceName + "</td></tr>";
  html += "<tr><td><b>Versi Firmware:</b></td><td><span class='ok'>v" FIRMWARE_VERSION "</span></td></tr>";
  html += "<tr><td><b>Mode Network:</b></td><td>" + String(isApMode ? "<span class='err'>AP Hotspot Mode</span>" : "<span class='ok'>STA Connected Mode</span>") + "</td></tr>";
  html += "<tr><td><b>IP Address:</b></td><td>" + (isApMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "</td></tr>";
  html += "<tr><td><b>Sisa RAM (Free Heap):</b></td><td>" + String(freeHeap / 1024.0, 1) + " KB / " + String(totalHeap / 1024.0, 1) + " KB (" + String(heapUsagePct, 1) + "% terpakai)</td></tr>";
  html += "<tr><td><b>Min Free Heap Ever:</b></td><td>" + String(ESP.getMinFreeHeap() / 1024.0, 1) + " KB</td></tr>";
  html += "<tr><td><b>Uptime Perangkat:</b></td><td>" + String(days) + "d " + String(hours) + "h " + String(minutes) + "m " + String(seconds) + "s</td></tr>";
  html += "<tr><td><b>Frekuensi CPU / Flash:</b></td><td>" + String(ESP.getCpuFreqMHz()) + " MHz / " + String(ESP.getFlashChipSize() / (1024 * 1024)) + " MB</td></tr>";
  html += "</table>";
  
  // Status Hardware
  html += "<h3>&#128268; Status Hardware</h3>";
  html += "<p style='font-size:13px;margin:3px 0;'>AHT10 Sensor: " + String(ahtOk ? "<span class='ok'>[ONLINE]</span>" : "<span class='err'>[OFFLINE]</span>") + "</p>";
  html += "<p style='font-size:13px;margin:3px 0;'>GY-68 (BMP180): " + String(bmpOk ? "<span class='ok'>[ONLINE]</span>" : "<span class='err'>[OFFLINE]</span>") + "</p>";
  html += "<p style='font-size:13px;margin:3px 0;'>DS1307 RTC: " + String(rtcOk ? "<span class='ok'>[ONLINE]</span>" : "<span class='err'>[OFFLINE]</span>") + "</p>";
  html += "<hr>";

  // Form Device Name
  html += "<h3>&#127991;&#65039; Nama Perangkat (Device Hostname)</h3>";
  html += "<form method='POST'>";
  html += "<label>Device Hostname:</label><input type='text' name='dev_name' value='" + deviceName + "' required>";
  html += "<button type='submit' name='save_dev' value='1'>Simpan Nama Device</button>";
  html += "</form>";
  html += "<hr>";

  // Form Pengaturan WiFi 1, WiFi 2, AP Mode
  html += "<h3>&#128246; Pengaturan Jaringan WiFi</h3>";
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

  html += "<button type='submit' name='save_wifi' value='1'>Simpan Semua WiFi</button>";
  html += "</form>";
  html += "<hr>";

  // Form Edit NTP
  html += "<h3>&#128336; Pengaturan Server NTP</h3>";
  html += "<form method='POST'>";
  html += "<label>NTP Utama (Lokal):</label><input type='text' name='ntp1' value='" + ntpServerLocal + "'>";
  html += "<label>NTP Kedua (Internet):</label><input type='text' name='ntp2' value='" + ntpServerInet1 + "'>";
  html += "<label>NTP Ketiga (Internet):</label><input type='text' name='ntp3' value='" + ntpServerInet2 + "'>";
  html += "<button type='submit' name='save_ntp' value='1'>Simpan & Sync NTP</button>";
  html += "</form>";
  html += "<hr>";

  // Form Ubah User & Password Config
  html += "<h3>&#128274; Ubah Kredensial Login Web UI</h3>";
  html += "<form method='POST'>";
  html += "<label>User Baru:</label><input type='text' name='new_user' value='" + configUser + "' required>";
  html += "<label>Password Baru:</label><input type='password' name='new_pass' placeholder='Password Baru' required>";
  html += "<label>Password Lama (Konfirmasi):</label><input type='password' name='old_pass' placeholder='Password Saat Ini' required>";
  html += "<button type='submit' name='save_auth' value='1'>Ubah User & Password</button>";
  html += "</form>";
  html += "<hr>";

  // Form OTA Upload Firmware
  html += "<h3>&#128230; Firmware Update (OTA)</h3>";
  html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
  html += "<input type='file' name='update' accept='.bin' required>";
  html += "<button type='submit' class='btn'>Upload Firmware (.bin)</button>";
  html += "</form>";
  html += "<hr>";

  // Action Restart
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
  server.send(200, "text/html", "<meta charset='utf-8'><h3>Merekali ESP32-C3... Halaman akan dimuat ulang dalam 10 detik.</h3><script>setTimeout(function(){location.href='/config';}, 10000);</script>");
  delay(1000);
  ESP.restart();
}

// --- LOGIKA KONTROL LED INDIKATOR ---
void updateLEDIndicators() {
  if (WiFi.status() == WL_CONNECTED && !isApMode) {
    digitalWrite(LED_MERAH_PIN, LOW); // Mati jika terhubung WiFi Router
  } else {
    digitalWrite(LED_MERAH_PIN, HIGH); // Nyala jika AP Mode / Terputus
  }

  static unsigned long lastHeartbeat = 0;
  static bool ledState = false;
  static unsigned long ledTurnOnTime = 0;
  unsigned long currentMillis = millis();

  if (!ledState && (currentMillis - lastHeartbeat >= 5000)) {
    lastHeartbeat = currentMillis;
    ledState = true;
    digitalWrite(LED_BIRU_PIN, LOW); // Active LOW -> NYALA
    ledTurnOnTime = currentMillis;
  }

  if (ledState && (currentMillis - ledTurnOnTime >= 100)) {
    ledState = false;
    digitalWrite(LED_BIRU_PIN, HIGH); // Active LOW -> MATI
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(LED_MERAH_PIN, OUTPUT);
  pinMode(LED_BIRU_PIN, OUTPUT);
  digitalWrite(LED_MERAH_PIN, HIGH);
  digitalWrite(LED_BIRU_PIN, HIGH); // Active LOW -> OFF

  Wire.begin(SDA_PIN, SCL_PIN);

  // Cek Hardware Sensor & RTC
  ahtOk = aht.begin(&Wire);
  bmpOk = bmp.begin(BMP085_ULTRAHIGHRES, &Wire);
  rtcOk = rtc.begin(&Wire);

  if (rtcOk && !rtc.isrunning()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  // Load Pengaturan dari Flash (NVS)
  loadSettings();

  // Koneksi Network Failover (Target 1 -> Target 2 -> Auto AP Mode)
  setupNetwork();

  if (!isApMode) {
    syncRTCFromNTP();
    lastNTPResync = millis();
  }

  udpServer.begin(NTP_PORT);

  // Endpoint Routing
  server.on("/", handleRoot);
  server.on("/api/data", handleRoot);
  server.on("/api/time", handleTimeApi);
  server.on("/config", handleConfig);
  server.on("/logout", handleLogout);
  server.on("/restart", HTTP_POST, handleRestart);

  // OTA Upload Handler (/update)
  server.on("/update", HTTP_POST, []() {
    if (!server.authenticate(configUser.c_str(), configPass.c_str())) {
      return server.requestAuthentication();
    }
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", (Update.hasError()) ? "<meta charset='utf-8'><h3>Update Gagal!</h3>" : "<meta charset='utf-8'><h3>Update Sukses! ESP32 Restarting...</h3><script>setTimeout(function(){location.href='/config';}, 10000);</script>");
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Update Firmware: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { 
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        Serial.printf("Update Selesai: %u Byte\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });

  server.begin();
  Serial.printf("🚀 Web Server API & System Config v%s Siap!\n", FIRMWARE_VERSION);
}

void loop() {
  server.handleClient();
  handleNTPServer();
  updateLEDIndicators();

  // Auto-Resync NTP Tiap 12 Jam (hanya dalam STA Mode)
  if (!isApMode && WiFi.status() == WL_CONNECTED && (millis() - lastNTPResync >= RESYNC_INTERVAL_MS)) {
    lastNTPResync = millis();
    syncRTCFromNTP();
  }
}