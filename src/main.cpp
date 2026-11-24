#include <Arduino.h>
#include <Wire.h>
#include <BH1750.h>
#include <RTClib.h>
#include "logic.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

// Compile-time WiFi defaults used only when no valid saved settings exist.
// Change these values before building if the device should start with different
// credentials. Do not commit real passwords to a shared repository.
#define STEM_DEFAULT_STA_SSID ""
#define STEM_DEFAULT_STA_PASSWORD ""
#define STEM_DEFAULT_AP_SSID "STEM_THPT_Trang_Dinh"
#define STEM_DEFAULT_AP_PASSWORD "STEM@thpttrangdinh12"

const unsigned long WIFI_TIMEOUT_MS = 15000;

struct WiFiSettings
{
  char staSsid[33];
  char staPassword[64];
  char apSsid[33];
  char apPassword[64];
};

WiFiSettings wifiSettings = {
    STEM_DEFAULT_STA_SSID,
    STEM_DEFAULT_STA_PASSWORD,
    STEM_DEFAULT_AP_SSID,
    STEM_DEFAULT_AP_PASSWORD};
bool restartPending = false;
unsigned long restartAt = 0;

bool validWiFiSettings(const WiFiSettings &settings)
{
  if (!memchr(settings.staSsid, 0, sizeof(settings.staSsid)) ||
      !memchr(settings.staPassword, 0, sizeof(settings.staPassword)) ||
      !memchr(settings.apSsid, 0, sizeof(settings.apSsid)) ||
      !memchr(settings.apPassword, 0, sizeof(settings.apPassword)))
    return false;
  // Empty station SSID disables station mode; its password is retained for
  // later use and validated just like the password of an enabled station.
  return stem::wifiSsid(settings.staSsid, true) &&
         stem::wifiPassword(settings.staPassword, true) &&
         stem::wifiSsid(settings.apSsid, false) &&
         stem::wifiPassword(settings.apPassword, false);
}

void loadWiFiSettings()
{
  Preferences preferences;
  WiFiSettings saved = {};
  if (preferences.begin("stem-wifi", true))
  {
    if (preferences.getBytesLength("settings") == sizeof(saved) &&
        preferences.getBytes("settings", &saved, sizeof(saved)) == sizeof(saved) &&
        validWiFiSettings(saved))
    {
      wifiSettings = saved;
    }
    preferences.end();
  }
}

String htmlEscape(const String &text)
{
  String escaped;
  for (size_t i = 0; i < text.length(); ++i)
  {
    switch (text[i])
    {
    case '&':
      escaped += "&amp;";
      break;
    case '<':
      escaped += "&lt;";
      break;
    case '>':
      escaped += "&gt;";
      break;
    case '\'':
      escaped += "&#39;";
      break;
    case '"':
      escaped += "&quot;";
      break;
    default:
      escaped += text[i];
      break;
    }
  }
  return escaped;
}
const int LAMP_PIN = 26;
const int WATER_PIN = 35;
const int SOIL_PIN = 34;
const int PUMP_PIN = 25;
const int STATUS_LED_PIN = 5; // Active-high system status LED
const int DAT_PIN = 21;       // SDA
const int SCL_PIN = 22;       // I2C
const float ON_THRESHOLD = 100;
const float OFF_THRESHOLD = 200;
const unsigned long DEBOUNCE_DELAY = 15000; // 15 seconds
const unsigned long ADC_PERIOD_MS = 1000;

stem::Config controlConfig;
stem::Pump pump;
float waterRaw = NAN, soilRaw = NAN, waterPercent = NAN, soilPercent = NAN;
unsigned long adcReadAt = 0, moistureSampleAt = 0;
bool moistureFresh = false, settingsValid = false;
Preferences controlPreferences;
const uint32_t CONTROL_MAGIC = 0x5354454d;

// ========== GLOBAL VARIABLES ==========
BH1750 lightsensor;
RTC_DS3231 rtc;

WebServer server(80);

float lux = 0;
float lux_trung_binh = 0;
bool trang_thai_den = false;
bool cam_bien_ok = false;
bool rtc_ok = false;

unsigned long lan_doc_cuoi = 0;
unsigned long diem_bat_dau = 0;
bool dang_cho = false;
bool trang_thai_cho_xu_ly = false;

// Moving average for light sensor
const int so_mau = 3;
float mang_lux[so_mau] = {0};
int vi_tri = 0;
float tong_lux = 0;

// WiFi AP mode
bool wifi_ap_mode = false;

// Status LED timing is independent of lamp control and WiFi servicing.
const unsigned long STATUS_LED_BLINK_MS = 500;
bool statusLedState = false;

void updateStatusLed()
{
  const unsigned long now = millis();
  const bool shouldBlink = WiFi.status() != WL_CONNECTED;
  const bool desiredState = shouldBlink ? ((now / STATUS_LED_BLINK_MS) % 2 == 0) : true;
  if (desiredState != statusLedState)
  {
    statusLedState = desiredState;
    digitalWrite(STATUS_LED_PIN, statusLedState ? HIGH : LOW);
  }
}

// ========== RTC FUNCTIONS ==========
void setupRTC()
{
  if (!rtc.begin())
  {
    Serial.println("Couldn't find RTC");
    rtc_ok = false;
    return;
  }

  if (rtc.lostPower())
  {
    Serial.println("RTC lost power, setting to compile time");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  rtc_ok = true;
  Serial.println("DS3231 RTC initialized");
}

DateTime getCurrentTime()
{
  if (rtc_ok)
  {
    return rtc.now();
  }
  // Fallback to millis if RTC not available
  unsigned long seconds = millis() / 1000;
  int hours = (seconds / 3600) % 24;
  int minutes = (seconds / 60) % 60;
  int secs = seconds % 60;

  // Create a fake DateTime (today's date from compile time, current time from millis)
  DateTime now(F(__DATE__), F(__TIME__));
  return DateTime(now.year(), now.month(), now.day(), hours, minutes, secs);
}

// Read both temperature registers in one checked I2C transaction.
bool readRtcTemperature(float &temperature)
{
  Wire.beginTransmission(0x68);
  Wire.write(0x11);
  if (Wire.endTransmission(false) != 0)
    return false;
  if (Wire.requestFrom(0x68, 2) != 2)
    return false;
  int msb = Wire.read();
  int lsb = Wire.read();
  if (msb < 0 || lsb < 0)
    return false;
  temperature = stem::ds3231Temperature(msb, lsb);
  return true;
}

void loadControlConfig()
{
  stem::Blob blob{};
  if (controlPreferences.begin("stem-control", true) && controlPreferences.getBytesLength("config") == sizeof(blob) && controlPreferences.getBytes("config", &blob, sizeof(blob)) == sizeof(blob) && stem::valid(blob))
  {
    controlConfig = blob.config;
    settingsValid = true;
  }
  else
  {
    settingsValid = false;
  }
  controlPreferences.end();
}
bool saveControlConfig(const stem::Config &config)
{
  stem::Blob blob;
  blob.config = config;
  blob.checksum = stem::checksum(blob);
  if (!controlPreferences.begin("stem-control", false))
    return false;
  bool ok = controlPreferences.putBytes("config", &blob, sizeof(blob)) == sizeof(blob);
  controlPreferences.end();
  return ok;
}
void sampleMoisture()
{
  const unsigned long now = millis();
  if (now - adcReadAt < ADC_PERIOD_MS)
    return;
  adcReadAt = now;
  waterRaw = analogRead(WATER_PIN);
  soilRaw = analogRead(SOIL_PIN);
  waterPercent = stem::percent(waterRaw, controlConfig.waterDry, controlConfig.waterWet);
  soilPercent = stem::percent(soilRaw, controlConfig.soilDry, controlConfig.soilWet);
  moistureSampleAt = now;
  moistureFresh = true;
}
void updatePump()
{
  bool enabled = settingsValid && stem::valid(controlConfig) && controlConfig.soilCal == 1 && controlConfig.soilAlert <= 95;
  bool before = pump.on;
  bool fresh = moistureFresh && !stem::elapsed(millis(), moistureSampleAt, 3000);
  pump.update(millis(), enabled, fresh, soilPercent, controlConfig.soilAlert);
  if (pump.on != before)
  {
    digitalWrite(PUMP_PIN, pump.on ? LOW : HIGH);
    Serial.println(pump.on ? "Pump ON" : "Pump OFF");
  }
}

void cam_bien()
{
  float lux_moi = lightsensor.readLightLevel();

  if (!isfinite(lux_moi) || lux_moi < 0)
  {
    Serial.println("Error reading BH1750!");
    cam_bien_ok = false;
    return;
  }

  // Moving average
  tong_lux = tong_lux - mang_lux[vi_tri] + lux_moi;
  mang_lux[vi_tri] = lux_moi;
  vi_tri = (vi_tri + 1) % so_mau;
  lux_trung_binh = tong_lux / so_mau;

  cam_bien_ok = true;
  lux = lux_moi;

  Serial.print("Instant light: ");
  Serial.print(lux_moi);
  Serial.print(" lux | Moving avg: ");
  Serial.print(lux_trung_binh);
  Serial.println(" lux");
}

void kiem_tra()
{
  if (!cam_bien_ok)
    return;

  unsigned long now = millis();
  bool mong_muon = trang_thai_den;

  if (!trang_thai_den && lux_trung_binh < ON_THRESHOLD)
  {
    mong_muon = true;
  }
  else if (trang_thai_den && lux_trung_binh > OFF_THRESHOLD)
  {
    mong_muon = false;
  }

  if (mong_muon == trang_thai_den)
  {
    dang_cho = false;
    return;
  }

  if (!dang_cho || mong_muon != trang_thai_cho_xu_ly)
  {
    trang_thai_cho_xu_ly = mong_muon;
    diem_bat_dau = now;
    dang_cho = true;
    return;
  }

  if (now - diem_bat_dau >= DEBOUNCE_DELAY)
  {
    trang_thai_den = trang_thai_cho_xu_ly;
    digitalWrite(LAMP_PIN, trang_thai_den ? LOW : HIGH);
    Serial.print("Lamp: ");
    Serial.println(trang_thai_den ? "ON" : "OFF");
    dang_cho = false;
  }
}

// ========== WEB SERVER FUNCTIONS ==========
void handleRoot()
{
  DateTime now = getCurrentTime();

  String html = "<html><head><title>STEM Monitor</title>";
  html += "<meta charset='utf-8'>";
  html += "<style>body{font-family:Arial;max-width:600px;margin:20px;}";
  html += ".card{background:#f5f5f5;padding:15px;margin:10px 0;border-radius:5px;}";
  html += ".value{font-size:24px;font-weight:bold;color:#0066cc;}";
  html += ".status-on{color:green;}";
  html += ".status-off{color:red;}";
  html += "</style></head><body>";
  html += "<h1>STEM Monitor</h1>";

  // Time card
  html += "<div class='card'><h2>Current Time</h2>";
  char time_str[20];
  sprintf(time_str, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  html += "<div class='value'>";
  html += time_str;
  html += "</div>";
  char date_str[20];
  sprintf(date_str, "%02d/%02d/%04d", now.day(), now.month(), now.year());
  html += "<div>";
  html += date_str;
  html += "</div></div>";

  // Light sensor card
  html += "<div class='card'><h2>Light Sensor</h2>";
  html += "<div>Instant: <span class='value'>";
  html += String(lux, 1);
  html += " lux</span></div>";
  html += "<div>Average: <span class='value'>";
  html += String(lux_trung_binh, 1);
  html += " lux</span></div>";
  html += "<div>Status: ";
  html += cam_bien_ok ? "<span style='color:green'>OK</span>" : "<span style='color:red'>ERROR</span>";
  html += "</div></div>";

  // Lamp card
  html += "<div class='card'><h2>Lamp Control</h2>";
  html += "<div>State: <span class='value ";
  html += trang_thai_den ? "status-on'" : "status-off'";
  html += ">";
  html += trang_thai_den ? "ON" : "OFF";
  html += "</span></div>";
  html += "<div>Threshold ON: < 100 lux</div>";
  html += "<div>Threshold OFF: > 200 lux</div>";
  html += "</div>";

  html += "<div class='card'><h2>Moisture / Pump Safety</h2>";
  html += "<div>Water ADC: " + String(waterRaw, 0) + " (" + String(waterPercent, 1) + "%)</div>";
  html += "<div>Soil ADC: " + String(soilRaw, 0) + " (" + String(soilPercent, 1) + "%)</div>";
  html += "<div>Calibration: " + String(controlConfig.soilCal == 1 ? "soil calibrated" : "NOT calibrated; pump locked OFF") + "</div>";
  html += "<div>Pump: <b>" + String(pump.on ? "ON" : "OFF") + "</b>; runtime max 30 s; cooldown 5 min; hysteresis 5 percentage points.</div>";
  html += "<p>ADC wiring faults or disconnected/floating probes are not reliably detectable. Use an independent low-water/dry-run cutoff.</p>";
  html += "<form method='POST' action='/moisture'><label>Soil dry ADC <input name='soil_dry' type='number' min='0' max='4095' value='" + String(controlConfig.soilDry) + "'></label><br><label>Soil wet ADC <input name='soil_wet' type='number' min='0' max='4095' value='" + String(controlConfig.soilWet) + "'></label><br><label>Water dry ADC <input name='water_dry' type='number' min='0' max='4095' value='" + String(controlConfig.waterDry) + "'></label><br><label>Water wet ADC <input name='water_wet' type='number' min='0' max='4095' value='" + String(controlConfig.waterWet) + "'></label><br><label>Start pump at soil % <input name='soil_alert' type='number' min='0' max='95' value='" + String(controlConfig.soilAlert) + "'></label><br><button>Save moisture settings</button></form></div>";

  html += "<div class='card'><h2>RTC Status</h2>";
  html += "<div>DS3231: ";
  html += rtc_ok ? "<span style='color:green'>Detected</span>" : "<span style='color:red'>Not found</span>";
  html += "</div><div>DS3231 chip temperature (not necessarily air temperature): ";
  float chipTemperature;
  if (readRtcTemperature(chipTemperature))
  {
    html += String(chipTemperature, 2);
    html += " &deg;C";
  }
  else
  {
    html += "Unavailable";
  }
  html += "</div></div>";

  // WiFi card
  html += "<div class='card'><h2>WiFi Configuration</h2>";
  html += "<div>Mode: ";
  html += wifi_ap_mode ? "Access Point" : (WiFi.status() == WL_CONNECTED ? "Station" : "Offline");
  html += "</div>";
  if (wifi_ap_mode)
  {
    html += "<div>SSID: ";
    html += htmlEscape(wifiSettings.apSsid);
    html += "</div><div>IP: ";
    html += WiFi.softAPIP().toString();
    html += "</div>";
  }
  else if (WiFi.status() == WL_CONNECTED)
  {
    html += "<div>SSID: ";
    html += htmlEscape(wifiSettings.staSsid);
    html += "</div><div>IP: ";
    html += WiFi.localIP().toString();
    html += "</div>";
  }
  html += "<form method='POST' action='/wifi'>";
  html += "<label>Station SSID (empty disables station): <input name='sta_ssid' maxlength='32' value='";
  html += htmlEscape(wifiSettings.staSsid);
  html += "'></label><br>";
  html += "<label>Station password: <input type='password' name='sta_password' maxlength='63' autocomplete='new-password'></label><br>";
  html += "<label><input type='checkbox' name='clear_sta_password' value='1'> Clear station password (open network)</label><br>";
  html += "<label>AP SSID: <input name='ap_ssid' maxlength='32' required value='";
  html += htmlEscape(wifiSettings.apSsid);
  html += "'></label><br>";
  html += "<label>AP password: <input type='password' name='ap_password' maxlength='63' autocomplete='new-password'></label><br>";
  html += "<p>Leave passwords blank to keep them. Empty station SSID disables station and retains its password; use Clear for an open network. Nonempty passwords must be 8–63 printable ASCII characters; the AP password is required. Saving restarts the device.</p>";
  html += "<button type='submit'>Save WiFi settings and restart</button></form></div>";

  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleWiFi()
{
  if (server.method() != HTTP_POST)
  {
    server.send(405, "text/plain", "POST required");
    return;
  }
  const char *fields[] = {"sta_ssid", "sta_password", "ap_ssid", "ap_password"};
  const size_t limits[] = {32, 63, 32, 63};
  for (size_t i = 0; i < 4; ++i)
  {
    if (!server.hasArg(fields[i]))
    {
      server.send(400, "text/plain", "Missing WiFi field");
      return;
    }
    String input = server.arg(fields[i]);
    if (input.length() > limits[i] || strlen(input.c_str()) != input.length())
    {
      server.send(400, "text/plain", "Invalid WiFi field length");
      return;
    }
  }
  if (server.hasArg("clear_sta_password") && server.arg("sta_password").length())
  {
    server.send(400, "text/plain", "Cannot set and clear the station password together");
    return;
  }
  WiFiSettings candidate = wifiSettings;
  String value;
  value = server.arg("sta_ssid");
  value.toCharArray(candidate.staSsid, sizeof(candidate.staSsid));
  if (server.hasArg("sta_password") && server.arg("sta_password").length())
    server.arg("sta_password").toCharArray(candidate.staPassword, sizeof(candidate.staPassword));
  if (server.hasArg("clear_sta_password"))
    candidate.staPassword[0] = '\0';
  value = server.arg("ap_ssid");
  value.toCharArray(candidate.apSsid, sizeof(candidate.apSsid));
  if (server.hasArg("ap_password") && server.arg("ap_password").length())
    server.arg("ap_password").toCharArray(candidate.apPassword, sizeof(candidate.apPassword));
  if (!validWiFiSettings(candidate))
  {
    server.send(400, "text/plain", "Invalid WiFi settings. Check SSID and password lengths/characters.");
    return;
  }
  Preferences preferences;
  if (!preferences.begin("stem-wifi", false) ||
      preferences.putBytes("settings", &candidate, sizeof(candidate)) != sizeof(candidate))
  {
    preferences.end();
    server.send(500, "text/plain", "Unable to save WiFi settings");
    return;
  }
  preferences.end();
  wifiSettings = candidate;
  server.send(200, "text/plain", "WiFi settings saved; restarting...");
  restartPending = true;
  restartAt = millis() + 1000;
}

void handleConfig()
{
  if (server.hasArg("on_threshold"))
  {
    float new_on = server.arg("on_threshold").toFloat();
    float new_off = server.arg("off_threshold").toFloat();
    unsigned long new_delay = server.arg("delay").toInt();

    // Update thresholds (you might want to make these global or use EEPROM)
    Serial.print("New ON threshold: ");
    Serial.println(new_on);
    Serial.print("New OFF threshold: ");
    Serial.println(new_off);
    Serial.print("New delay: ");
    Serial.println(new_delay);

    // Redirect back to root
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting...");
  }
  else
  {
    server.send(400, "text/plain", "Missing parameters");
  }
}

void handleMoisture()
{
  uint32_t sd, sw, wd, ww, sa;
  if (!server.hasArg("soil_dry") || !server.hasArg("soil_wet") || !server.hasArg("water_dry") || !server.hasArg("water_wet") || !server.hasArg("soil_alert") || !stem::number(server.arg("soil_dry").c_str(), 4095, sd) || !stem::number(server.arg("soil_wet").c_str(), 4095, sw) || !stem::number(server.arg("water_dry").c_str(), 4095, wd) || !stem::number(server.arg("water_wet").c_str(), 4095, ww) || !stem::number(server.arg("soil_alert").c_str(), 95, sa) || sd == sw || wd == ww)
  {
    server.send(400, "text/plain", "Invalid moisture settings");
    return;
  }
  stem::Config candidate = controlConfig;
  candidate.soilDry = sd;
  candidate.soilWet = sw;
  candidate.waterDry = wd;
  candidate.waterWet = ww;
  candidate.soilAlert = sa;
  candidate.soilCal = 1;
  candidate.waterCal = 1;
  if (!stem::valid(candidate) || !saveControlConfig(candidate))
  {
    server.send(400, "text/plain", "Settings rejected or storage unavailable");
    return;
  }
  controlConfig = candidate;
  settingsValid = true;
  moistureFresh = false;
  if (pump.on)
  {
    pump.on = false;
    pump.cooling = true;
    pump.stopped = millis();
    digitalWrite(PUMP_PIN, HIGH);
  }
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "Saved");
}

void handleNotFound()
{
  server.send(404, "text/plain", "Not found");
}

void setupWebServer()
{
  server.on("/", HTTP_GET, handleRoot);
  server.on("/config", HTTP_GET, handleConfig);
  server.on("/wifi", HTTP_POST, handleWiFi);
  server.on("/moisture", HTTP_POST, handleMoisture);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");
}

void setupWiFi()
{
  loadWiFiSettings();
  if (!validWiFiSettings(wifiSettings))
  {
    Serial.println("Invalid WiFi defaults; check source configuration");
    return;
  }
  WiFi.persistent(false);
  if (wifiSettings.staSsid[0])
  {
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSettings.staSsid, wifiSettings.staPassword);
    unsigned long started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_TIMEOUT_MS)
    {
      delay(100);
    }
  }
  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.print("Station IP address: ");
    Serial.println(WiFi.localIP());
  }
  else
  {
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    wifi_ap_mode = WiFi.softAP(wifiSettings.apSsid, wifiSettings.apPassword);
    if (wifi_ap_mode)
    {
      Serial.print("AP IP address: ");
      Serial.println(WiFi.softAPIP());
    }
    else
    {
      Serial.println("Failed to start access point");
    }
  }

  // Start mDNS
  if (!MDNS.begin("stem"))
  {
    Serial.println("Error setting up MDNS responder!");
  }
  else
  {
    Serial.println("mDNS responder started");
  }
}

// ========== SETUP ==========
void setup()
{
  // Preload LOW before enabling the output: active-high LED starts off.
  digitalWrite(STATUS_LED_PIN, LOW);
  pinMode(STATUS_LED_PIN, OUTPUT);

  Serial.begin(115200);
  delay(100);

  Serial.println("\nSTEM System Starting...");

  // Initialize I2C
  Wire.begin(DAT_PIN, SCL_PIN);
  Serial.println("I2C initialized");

  // Initialize relay outputs HIGH before enabling outputs: active-LOW means OFF.
  digitalWrite(PUMP_PIN, HIGH);
  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(LAMP_PIN, HIGH);
  pinMode(LAMP_PIN, OUTPUT);
  trang_thai_den = false;
  pump.on = false;
  loadControlConfig();

  // Initialize BH1750 light sensor
  cam_bien_ok = lightsensor.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
  if (!cam_bien_ok)
  {
    Serial.println("BH1750 not found!");
  }
  else
  {
    Serial.println("BH1750 found!");
  }

  // Initialize DS3231 RTC
  setupRTC();

  // Initialize WiFi
  setupWiFi();

  // Initialize web server
  setupWebServer();

  Serial.println("Setup complete!");
}

// ========== MAIN LOOP ==========
void loop()
{
  updatePump();
  sampleMoisture();
  updatePump();
  // Handle sensor reading
  if (!cam_bien_ok)
  {
    if (millis() - lan_doc_cuoi >= 5000)
    {
      lan_doc_cuoi = millis();
      cam_bien_ok = lightsensor.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
      if (cam_bien_ok)
      {
        Serial.println("BH1750 reconnected");
        // Reset moving average
        for (int i = 0; i < so_mau; i++)
        {
          mang_lux[i] = 0;
        }
        tong_lux = 0;
        vi_tri = 0;
      }
    }
  }
  else if (millis() - lan_doc_cuoi >= 2000)
  {
    lan_doc_cuoi = millis();
    cam_bien();
  }

  kiem_tra();

  server.handleClient();
  updateStatusLed();

  if (restartPending && static_cast<long>(millis() - restartAt) >= 0)
  {
    ESP.restart();
  }

  delay(10);
}
