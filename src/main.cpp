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
const unsigned long DEBOUNCE_DELAY = 5000; // 15 seconds
const unsigned long ADC_PERIOD_MS = 1000;

stem::Config controlConfig;
stem::Pump pump;
stem::Lamp lamp;
float waterRaw = NAN, soilRaw = NAN, waterPercent = NAN, soilPercent = NAN;
unsigned long adcReadAt = 0, moistureSampleAt = 0;
bool moistureFresh = false, settingsValid = false;
Preferences controlPreferences;
const uint32_t CONTROL_MAGIC = 0x5354454d;

// ========== GLOBAL VARIABLES ==========
BH1750 lightsensor;
bool lightInitialized = false;

WebServer server(80);

float lux = NAN;
float lux_trung_binh = NAN;
bool trang_thai_den = false;
bool cam_bien_ok = false;
bool rtc_ok = false;

unsigned long lan_doc_cuoi = 0;
unsigned long diem_bat_dau = 0;
bool dang_cho = false;
bool trang_thai_cho_xu_ly = false;

// Moving average for light sensor; only populated samples contribute.
const int so_mau = 3;
stem::Average<so_mau> luxAverage;

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
uint8_t bcd(unsigned n) { return uint8_t((n / 10) * 16 + n % 10); }
bool unbcd(uint8_t value, unsigned &out)
{
  if ((value & 15) > 9 || (value >> 4) > 9)
    return false;
  out = (value >> 4) * 10 + (value & 15);
  return true;
}
bool rtcRead(uint8_t reg, uint8_t *data, size_t count)
{
  Wire.beginTransmission(0x68);
  if (Wire.write(reg) != 1 || Wire.endTransmission(false) != 0)
    return false;
  if (Wire.requestFrom(0x68, int(count)) != count)
    return false;
  for (size_t i = 0; i < count; ++i)
  {
    int value = Wire.read();
    if (value < 0)
      return false;
    data[i] = uint8_t(value);
  }
  return true;
}
bool rtcWrite(uint8_t reg, const uint8_t *data, size_t count)
{
  Wire.beginTransmission(0x68);
  bool ok = Wire.write(reg) == 1 && Wire.write(data, count) == count;
  return Wire.endTransmission() == 0 && ok;
}
bool readRtcTime(DateTime &result)
{
  uint8_t status, r[7];
  if (!rtcRead(0x0f, &status, 1) || (status & 0x80) ||
      !rtcRead(0, r, sizeof(r)) || !rtcRead(0x0f, &status, 1) || (status & 0x80))
    return false;
  unsigned s, m, h, day, month, year;
  // Reject unsupported 12-hour mode and century; validate all BCD fields.
  if ((r[0] & 0x80) || (r[1] & 0x80) || (r[2] & 0xc0) ||
      (r[4] & 0xc0) || (r[5] & 0xe0) ||
      !unbcd(r[0], s) || !unbcd(r[1], m) || !unbcd(r[2], h) ||
      !unbcd(r[4], day) || !unbcd(r[5], month) || !unbcd(r[6], year) ||
      !stem::dateValid(2000 + year, month, day, h, m, s))
    return false;
  result = DateTime(2000 + year, month, day, h, m, s);
  return true;
}
bool readRtcTemperature(float &temperature)
{
  uint8_t data[2];
  if (!rtcRead(0x11, data, sizeof(data)))
    return false;
  temperature = stem::ds3231Temperature(data[0], data[1]);
  return true;
}
DateTime rtcTime(2000, 1, 1, 0, 0, 0);
float rtcTemperature = NAN;
unsigned long rtcReadAt = 0;
bool rtcSampled = false;
bool rtcNeedsSet = false; // A partial or unverifiable write cannot establish trustworthy time.
void refreshRTC(bool force = false)
{
  if (!force && rtcSampled && millis() - rtcReadAt < 1000)
    return;
  rtcSampled = true;
  rtcReadAt = millis();
  DateTime time(2000, 1, 1, 0, 0, 0);
  float temperature;
  rtc_ok = !rtcNeedsSet && readRtcTime(time) && readRtcTemperature(temperature);
  if (rtc_ok)
  {
    rtcTime = time;
    rtcTemperature = temperature;
  }
  else
    rtcTemperature = NAN;
}
void setupRTC() { refreshRTC(true); }

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
  bool enabled = settingsValid && stem::valid(controlConfig) && controlConfig.soilAlert <= 95;
  bool before = pump.on;
  bool fresh = moistureFresh && !stem::elapsed(millis(), moistureSampleAt, 3000);
  pump.update(millis(), enabled, fresh, soilPercent, controlConfig.soilAlert, controlConfig);
  if (pump.on != before)
  {
    digitalWrite(PUMP_PIN, pump.on ? LOW : HIGH);
    Serial.println(pump.on ? "Bơm BẬT" : "Bơm TẮT");
  }
}

void cam_bien()
{
  float lux_moi = lightsensor.readLightLevel();

  if (!isfinite(lux_moi) || lux_moi < 0)
  {
    Serial.println("Lỗi đọc BH1750!");
    cam_bien_ok = false;
    lightInitialized = false;
    lux = lux_trung_binh = NAN;
    luxAverage.clear();
    dang_cho = false;
    return;
  }

  lux_trung_binh = luxAverage.add(lux_moi);

  cam_bien_ok = true;
  lux = lux_moi;

  Serial.print("Ánh sáng tức thời: ");
  Serial.print(lux_moi);
  Serial.print(" lux | Trung bình trượt: ");
  Serial.print(lux_trung_binh);
  Serial.println(" lux");
}

void kiem_tra()
{
  if (!cam_bien_ok)
    return;

  unsigned long now = millis();
  bool fresh = moistureFresh && isfinite(lux_trung_binh);

  if (controlConfig.modeAuto) {
    bool newLampState = lamp.update(now, fresh, lux_trung_binh, controlConfig);
    if (newLampState != trang_thai_den) {
      trang_thai_den = newLampState;
      digitalWrite(LAMP_PIN, trang_thai_den ? LOW : HIGH);
      Serial.print("Đèn: ");
      Serial.println(trang_thai_den ? "BẬT" : "TẮT");
    }
  } else {
    if (lamp.manualState != trang_thai_den) {
      trang_thai_den = lamp.manualState;
      digitalWrite(LAMP_PIN, trang_thai_den ? LOW : HIGH);
      Serial.print("Đèn (thủ công): ");
      Serial.println(trang_thai_den ? "BẬT" : "TẮT");
    }
  }

  dang_cho = false;
}

// ========== WEB SERVER FUNCTIONS ==========
String jsonNumber(float x, int digits = 2) { return isfinite(x) ? String(x, digits) : "null"; }
void sendJson(int code, const String &body)
{
  if ((server.uri() == "/command" || server.uri() == "/mode") && code == 200)
  {
    server.sendHeader("Location", "/");
    server.send(303, "text/plain; charset=utf-8", "Đã gửi lệnh");
  }
  else server.send(code, "application/json; charset=utf-8", body);
}
void apiStatus()
{
  refreshRTC();
  DateTime t = rtcTime;
  char ts[32];
  snprintf(ts, sizeof(ts), "%04u-%02u-%02uT%02u:%02u:%02u", t.year(), t.month(), t.day(), t.hour(), t.minute(), t.second());
  String mode = wifi_ap_mode ? "AP" : (WiFi.status() == WL_CONNECTED ? "STA" : "offline");
  String ip = wifi_ap_mode ? WiFi.softAPIP().toString() : (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "");
  String j = "{\"health\":{\"light\":" + String(cam_bien_ok ? "true" : "false") + ",\"rtc\":" + (rtc_ok ? "true" : "false") + "}";
  j += ",\"light\":{\"lux\":" + (cam_bien_ok ? jsonNumber(lux) : "null") + ",\"average_lux\":" + (cam_bien_ok ? jsonNumber(lux_trung_binh) : "null") + "}";
  j += ",\"water\":{\"adc\":" + jsonNumber(waterRaw, 0) + ",\"percent\":" + jsonNumber(waterPercent) + "}";
  j += ",\"soil\":{\"adc\":" + jsonNumber(soilRaw, 0) + ",\"percent\":" + jsonNumber(soilPercent) + "}";
  j += ",\"pump\":" + String(pump.on ? "true" : "false") + ",\"lamp\":" + (digitalRead(LAMP_PIN) == LOW ? "true" : "false");
  j += ",\"mode\":\"" + String(controlConfig.modeAuto ? "auto" : "manual") + "\"";
  j += ",\"rtc_time\":" + String(rtc_ok ? "\"" : "") + (rtc_ok ? ts : "null") + (rtc_ok ? "\"" : "");
  j += ",\"ds3231_temperature_c\":" + (rtc_ok ? jsonNumber(rtcTemperature) : "null") + ",\"wifi\":{\"mode\":\"" + mode + "\",\"ip\":\"" + ip + "\"}}";
  sendJson(200, j);
}
void apiTime()
{
  unsigned v[6];
  String input = server.arg("time");
  if (server.args() != 1 || !server.hasArg("time") || input.length() != 19 ||
      strlen(input.c_str()) != input.length() || !stem::datetime(input.c_str(), v))
  {
    sendJson(400, "{\"ok\":false,\"reason\":\"Thời gian không hợp lệ; dùng YYYY-MM-DDTHH:MM:SS\"}");
    return;
  }
  rtc_ok = false;
  rtcTemperature = NAN;
  DateTime requested(v[0], v[1], v[2], v[3], v[4], v[5]);
  uint8_t registers[] = {bcd(v[5]), bcd(v[4]), bcd(v[3]), uint8_t(requested.dayOfTheWeek() + 1), bcd(v[2]), bcd(v[1]), bcd(v[0] - 2000)};
  uint8_t status;
  bool ok = rtcWrite(0, registers, sizeof(registers)) && rtcRead(0x0f, &status, 1);
  if (ok)
  {
    status &= 0x7f;
    ok = rtcWrite(0x0f, &status, 1);
  }
  rtcNeedsSet = !ok;
  refreshRTC(true);
  ok = ok && rtc_ok && rtcTime.unixtime() >= requested.unixtime() &&
       rtcTime.unixtime() - requested.unixtime() <= 1;
  if (!ok)
  {
    rtcNeedsSet = true;
    rtc_ok = false;
    rtcTemperature = NAN;
    sendJson(503, "{\"ok\":false,\"reason\":\"Không thể ghi hoặc xác minh DS3231\"}");
    return;
  }
  sendJson(200, "{\"ok\":true}");
}
void apiConfig()
{
  String j = "{\"lamp\":{\"on_threshold_lux\":100,\"off_threshold_lux\":200,\"debounce_ms\":15000},\"moisture\":{";
  j += "\"soil_dry\":" + String(controlConfig.soilDry) + ",\"soil_wet\":" + String(controlConfig.soilWet) + ",\"water_dry\":" + String(controlConfig.waterDry) + ",\"water_wet\":" + String(controlConfig.waterWet) + ",\"soil_alert\":" + String(controlConfig.soilAlert);
  j += ",\"soil_calibrated\":" + String(settingsValid && controlConfig.soilCal ? "true" : "false") + ",\"water_calibrated\":" + (settingsValid && controlConfig.waterCal ? "true" : "false") + "},\"pump_safety\":{\"max_runtime_s\":30,\"cooldown_s\":300,\"hysteresis_percent\":5}}";
  sendJson(200, j);
}
void apiMode()
{
  if (server.args() != 1 || !server.hasArg("mode"))
  {
    sendJson(400, "{\"ok\":false,\"reason\":\"Cần đúng một tham số mode\"}");
    return;
  }
  String mode = server.arg("mode");
  if (mode != "auto" && mode != "manual")
  {
    sendJson(400, "{\"ok\":false,\"reason\":\"mode phải là auto hoặc manual\"}");
    return;
  }
  controlConfig.modeAuto = mode == "auto";
  lamp.manualState = trang_thai_den;
  lamp.state = trang_thai_den;
  lamp.pending = false;
  pump.manualState = pump.on;
  if (!saveControlConfig(controlConfig))
  {
    sendJson(500, "{\"ok\":false,\"reason\":\"Không thể lưu chế độ\"}");
    return;
  }
  sendJson(200, "{\"ok\":true,\"mode\":\"" + mode + "\"}");
}

void apiCommand()
{
  if (server.args() != 1 || !server.hasArg("command"))
  {
    sendJson(400, "{\"ok\":false,\"reason\":\"Cần đúng một tham số command\"}");
    return;
  }
  String c = server.arg("command");
  if (c == "lamp_on" || c == "lamp_off")
  {
    if (controlConfig.modeAuto) {
      sendJson(400, "{\"ok\":false,\"reason\":\"Không thể điều khiển thủ công khi ở chế độ auto\"}");
      return;
    }
    lamp.manualState = c == "lamp_on";
    lamp.state = lamp.manualState;
    lamp.pending = false;
    trang_thai_den = lamp.manualState;
    dang_cho = false;
    digitalWrite(LAMP_PIN, trang_thai_den ? LOW : HIGH);
    sendJson(200, trang_thai_den ? "{\"ok\":true,\"lamp\":true}" : "{\"ok\":true,\"lamp\":false}");
    return;
  }
  if (c == "pump_off")
  {
    if (controlConfig.modeAuto) {
      sendJson(400, "{\"ok\":false,\"reason\":\"Không thể điều khiển thủ công khi ở chế độ auto\"}");
      return;
    }
    pump.manualState = false;
    if (pump.on)
    {
      pump.on = false;
      pump.cooling = true;
      pump.stopped = millis();
      digitalWrite(PUMP_PIN, HIGH);
    }
    sendJson(200, "{\"ok\":true,\"pump\":false}");
    return;
  }
  if (c == "pump_on")
  {
    if (controlConfig.modeAuto) {
      sendJson(400, "{\"ok\":false,\"reason\":\"Không thể điều khiển thủ công khi ở chế độ auto\"}");
      return;
    }
    pump.manualState = true;
    pump.cooling = false;
    if (!pump.on)
    {
      pump.on = true;
      pump.started = millis();
      digitalWrite(PUMP_PIN, LOW);
    }
    sendJson(200, "{\"ok\":true,\"pump\":true}");
    return;
  }
  sendJson(400, "{\"ok\":false,\"reason\":\"Lệnh không hợp lệ\"}");
}

void handleRoot()
{
  refreshRTC();
  DateTime now = rtcTime;

  String html = "<html><head><title>Giám sát STEM</title>";
  html += "<meta charset='utf-8'>";
  html += "<style>body{font-family:Arial;max-width:600px;margin:20px;}";
  html += ".card{background:#f5f5f5;padding:15px;margin:10px 0;border-radius:5px;}";
  html += ".value{font-size:24px;font-weight:bold;color:#0066cc;}";
  html += ".status-on{color:green;}";
  html += ".status-off{color:red;}";
  html += "</style></head><body>";
  html += "<h1>Giám sát STEM</h1>";

  // Time card
  html += "<div class='card'><h2>Thời gian hiện tại</h2>";
  char time_str[20];
  sprintf(time_str, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  html += "<div class='value' id='status-time'>";
  html += rtc_ok ? time_str : "Không khả dụng";
  html += "</div>";
  char date_str[20];
  sprintf(date_str, "%02d/%02d/%04d", now.day(), now.month(), now.year());
  html += "<div id='status-date'>";
  html += rtc_ok ? date_str : "Không khả dụng";
  html += "</div></div>";

  // Light sensor card
  html += "<div class='card'><h2>Cảm biến ánh sáng</h2>";
  html += "<div>Tức thời: <span class='value' id='status-lux'>";
  html += cam_bien_ok ? String(lux, 1) + " lux" : "Không khả dụng";
  html += "</span></div>";
  html += "<div>Trung bình: <span class='value' id='status-average'>";
  html += cam_bien_ok ? String(lux_trung_binh, 1) + " lux" : "Không khả dụng";
  html += "</span></div>";
  html += "<div>Trạng thái: <span id='status-light'>";
  html += cam_bien_ok ? "Bình thường" : "LỖI";
  html += "</span></div></div>";

  // Lamp card
  html += "<div class='card'><h2>Điều khiển đèn</h2>";
  html += "<div>Lệnh GPIO relay (không xác nhận tải): <span class='value ";
  html += digitalRead(LAMP_PIN) == LOW ? "status-on' id='status-lamp'>" : "status-off' id='status-lamp'>";
  html += digitalRead(LAMP_PIN) == LOW ? "BẬT" : "TẮT";
  html += "</span></div>";
  html += "<div>Ngưỡng BẬT: < 100 lux</div>";
  html += "<div>Ngưỡng TẮT: > 200 lux</div>";
  html += "<div><form method='POST' action='/command'><button type='submit' name='command' value='lamp_on' " + String(controlConfig.modeAuto ? "disabled" : "") + ">BẬT ĐÈN</button> <button type='submit' name='command' value='lamp_off' " + String(controlConfig.modeAuto ? "disabled" : "") + ">TẮT ĐÈN</button></form></div>";
  html += "</div>";

  // Mode control card
  html += "<div class='card'><h2>Chế độ điều khiển</h2>";
  html += "<div>Chế độ hiện tại: <span id='status-mode'>" + String(controlConfig.modeAuto ? "TỰ ĐỘNG" : "THỦ CÔNG") + "</span></div>";
  html += "<div><form method='POST' action='/mode'><button type='submit' name='mode' value='auto'>CHUYỂN TỰ ĐỘNG</button> <button type='submit' name='mode' value='manual'>CHUYỂN THỦ CÔNG</button></form></div>";
  html += "<div>Tự động: sử dụng ngưỡng cảm biến. Thủ công: sử dụng lệnh cuối cùng.</div>";
  html += "</div>";

  html += "<div class='card'><h2>Độ ẩm / An toàn bơm</h2>";
  html += "<div>ADC nước: <span id='status-water'>" + (isfinite(waterRaw) ? String(waterRaw, 0) : String("Không khả dụng")) + " (" + (isfinite(waterPercent) ? String(waterPercent, 1) + "%" : String("Không khả dụng")) + ")</span></div>";
  html += "<div>ADC đất: <span id='status-soil'>" + (isfinite(soilRaw) ? String(soilRaw, 0) : String("Không khả dụng")) + " (" + (isfinite(soilPercent) ? String(soilPercent, 1) + "%" : String("Không khả dụng")) + ")</span></div>";
  html += "<div>Hiệu chuẩn: " + String(settingsValid && controlConfig.soilCal == 1 ? "đất đã hiệu chuẩn" : "CHƯA hiệu chuẩn; số đo có thể không chính xác") + "</div>";
  html += "<div>Bơm: <b id='status-pump'>" + String(pump.on ? "BẬT" : "TẮT") + "</b>; chạy tối đa 30 giây. Chế độ tự động: nghỉ 5 phút; trễ 5 điểm phần trăm.</div>";
  html += "<div><form method='POST' action='/command'><button type='submit' name='command' value='pump_on' " + String(controlConfig.modeAuto ? "disabled" : "") + ">BẬT BƠM</button> <button type='submit' name='command' value='pump_off' " + String(controlConfig.modeAuto ? "disabled" : "") + ">TẮT BƠM</button></form></div>";
  html += "<p>Không thể phát hiện đáng tin cậy lỗi dây ADC hoặc đầu dò bị ngắt/thả nổi. Hãy dùng bộ ngắt độc lập khi cạn nước/chạy khô.</p>";
  html += "<form method='POST' action='/moisture'><label>ADC đất khô <input name='soil_dry' type='number' min='0' max='4095' value='" + String(controlConfig.soilDry) + "'></label><br><label>ADC đất ướt <input name='soil_wet' type='number' min='0' max='4095' value='" + String(controlConfig.soilWet) + "'></label><br><label>ADC nước khô <input name='water_dry' type='number' min='0' max='4095' value='" + String(controlConfig.waterDry) + "'></label><br><label>ADC nước ướt <input name='water_wet' type='number' min='0' max='4095' value='" + String(controlConfig.waterWet) + "'></label><br><label>Bắt đầu bơm khi đất % <input name='soil_alert' type='number' min='0' max='95' value='" + String(controlConfig.soilAlert) + "'></label><br><button>Lưu cài đặt độ ẩm</button></form></div>";

  html += "<div class='card'><h2>Trạng thái RTC</h2>";
  html += "<div>DS3231: <span id='status-rtc'>";
  html += rtc_ok ? "Thời gian hợp lệ" : "Thời gian không hợp lệ hoặc không khả dụng";
  html += "</span></div><div>Nhiệt độ chip DS3231 (không nhất thiết là nhiệt độ không khí): <span id='status-temperature'>";
  html += rtc_ok ? String(rtcTemperature, 2) + " &deg;C" : "Không khả dụng";
  html += "</span></div><button type='button' id='set-time'>Đặt giờ DS3231 theo thiết bị này</button><p id='time-result' role='status'></p></div>";

  // WiFi card
  html += "<div class='card'><h2>Cấu hình WiFi</h2>";
  html += "<div>Chế độ: <span id='status-wifi-mode'>";
  html += wifi_ap_mode ? "Điểm truy cập" : (WiFi.status() == WL_CONNECTED ? "Trạm" : "Ngoại tuyến");
  html += "</span></div>";
  if (wifi_ap_mode)
  {
    html += "<div>SSID: ";
    html += htmlEscape(wifiSettings.apSsid);
    html += "</div>";
  }
  else if (WiFi.status() == WL_CONNECTED)
  {
    html += "<div>SSID: ";
    html += htmlEscape(wifiSettings.staSsid);
    html += "</div>";
  }
  html += "<div>IP: <span id='status-wifi-ip'>";
  html += wifi_ap_mode ? WiFi.softAPIP().toString() : (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("Không khả dụng"));
  html += "</span></div>";
  html += "<form method='POST' action='/wifi'>";
  html += "<label>SSID trạm (để trống để tắt trạm): <input name='sta_ssid' maxlength='32' value='";
  html += htmlEscape(wifiSettings.staSsid);
  html += "'></label><br>";
  html += "<label>Mật khẩu trạm: <input type='password' name='sta_password' maxlength='63' autocomplete='new-password'></label><br>";
  html += "<label><input type='checkbox' name='clear_sta_password' value='1'> Xóa mật khẩu trạm (mạng mở)</label><br>";
  html += "<label>AP SSID: <input name='ap_ssid' maxlength='32' required value='";
  html += htmlEscape(wifiSettings.apSsid);
  html += "'></label><br>";
  html += "<label>Mật khẩu AP: <input type='password' name='ap_password' maxlength='63' autocomplete='new-password'></label><br>";
  html += "<p>Để trống mật khẩu để giữ nguyên. SSID trạm trống sẽ tắt trạm và giữ mật khẩu; dùng Xóa để chọn mạng mở. Mật khẩu không trống phải có 8–63 ký tự ASCII in được; mật khẩu AP là bắt buộc. Lưu sẽ khởi động lại thiết bị.</p>";
  html += "<button type='submit'>Lưu cài đặt WiFi và khởi động lại</button></form></div>";

  html += R"HTML(
  <p id='status-update' role='status'>Đang cập nhật trạng thái...</p>
  <script>
  (() => {
    const unavailable = 'Không khả dụng';
    const set = (id, value) => { document.getElementById(id).textContent = value; };
    const number = (value, digits, unit = '') =>
      typeof value === 'number' && Number.isFinite(value) ? value.toFixed(digits) + unit : unavailable;
    const onOff = value => typeof value === 'boolean' ? (value ? 'BẬT' : 'TẮT') : unavailable;
    const health = (id, value, yes, no) => {
      const element = document.getElementById(id);
      element.textContent = typeof value === 'boolean' ? (value ? yes : no) : unavailable;
      element.className = typeof value === 'boolean' ? (value ? 'status-on' : 'status-off') : '';
    };
    const moisture = value => {
      const adc = number(value?.adc, 0);
      const percent = number(value?.percent, 1, '%');
      return adc + ' (' + percent + ')';
    };
    function render(data) {
      const match = typeof data.rtc_time === 'string' && /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})$/.exec(data.rtc_time);
      set('status-time', match ? match[4] + ':' + match[5] + ':' + match[6] : unavailable);
      set('status-date', match ? match[3] + '/' + match[2] + '/' + match[1] : unavailable);
      set('status-lux', number(data.light?.lux, 1, ' lux'));
      set('status-average', number(data.light?.average_lux, 1, ' lux'));
      health('status-light', data.health?.light, 'Bình thường', 'LỖI');
      set('status-lamp', onOff(data.lamp));
      document.getElementById('status-lamp').className = 'value ' +
        (typeof data.lamp === 'boolean' ? (data.lamp ? 'status-on' : 'status-off') : '');
      set('status-water', moisture(data.water));
      set('status-soil', moisture(data.soil));
      set('status-pump', onOff(data.pump));
      health('status-rtc', data.health?.rtc, 'Thời gian hợp lệ', 'Thời gian không hợp lệ hoặc không khả dụng');
      set('status-temperature', number(data.ds3231_temperature_c, 2, ' °C'));
      const modes = { AP: 'Điểm truy cập', STA: 'Trạm', offline: 'Ngoại tuyến' };
      set('status-wifi-mode', Object.prototype.hasOwnProperty.call(modes, data.wifi?.mode) ? modes[data.wifi.mode] : unavailable);
      set('status-wifi-ip', typeof data.wifi?.ip === 'string' && data.wifi.ip ? data.wifi.ip : unavailable);
    }
    async function refresh() {
      const controller = new AbortController();
      const timeout = setTimeout(() => controller.abort(), 5000);
      try {
        const response = await fetch('/api/status', { cache: 'no-store', signal: controller.signal });
        if (!response.ok) throw new Error('HTTP ' + response.status);
        const data = await response.json();
        if (!data || typeof data !== 'object' || Array.isArray(data)) throw new Error('Dữ liệu không hợp lệ');
        render(data);
        set('status-update', 'Đã cập nhật trạng thái');
      } catch (error) {
        set('status-update', 'Lỗi cập nhật trạng thái; dữ liệu hiển thị có thể đã cũ. ' +
          (error.name === 'AbortError' ? 'Hết thời gian chờ.' : 'Vui lòng kiểm tra kết nối.'));
      } finally {
        clearTimeout(timeout);
        setTimeout(refresh, 1000);
      }
    }
    document.getElementById('set-time').addEventListener('click', async () => {
      const d = new Date(), pad = n => String(n).padStart(2, '0');
      const time = `${d.getFullYear()}-${pad(d.getMonth()+1)}-${pad(d.getDate())}T${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
      try {
        const response = await fetch('/api/time', {method: 'POST', headers: {'Content-Type': 'application/x-www-form-urlencoded'}, body: new URLSearchParams({time})});
        const result = await response.json();
        set('time-result', response.ok && result.ok ? 'Đã đặt giờ địa phương của thiết bị.' : (result.reason || 'Không thể đặt giờ.'));
      } catch (_) { set('time-result', 'Không thể kết nối để đặt giờ.'); }
    });
    refresh();
  })();
  </script>)HTML";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleWiFi()
{
  if (server.method() != HTTP_POST)
  {
    server.send(405, "text/plain", "Yêu cầu POST");
    return;
  }
  const char *fields[] = {"sta_ssid", "sta_password", "ap_ssid", "ap_password"};
  const size_t limits[] = {32, 63, 32, 63};
  for (size_t i = 0; i < 4; ++i)
  {
    if (!server.hasArg(fields[i]))
    {
      server.send(400, "text/plain", "Thiếu trường WiFi");
      return;
    }
    String input = server.arg(fields[i]);
    if (input.length() > limits[i] || strlen(input.c_str()) != input.length())
    {
      server.send(400, "text/plain", "Độ dài trường WiFi không hợp lệ");
      return;
    }
  }
  if (server.hasArg("clear_sta_password") && server.arg("sta_password").length())
  {
    server.send(400, "text/plain", "Không thể đồng thời đặt và xóa mật khẩu trạm");
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
    server.send(400, "text/plain", "Cài đặt WiFi không hợp lệ. Hãy kiểm tra độ dài/ký tự SSID và mật khẩu.");
    return;
  }
  Preferences preferences;
  if (!preferences.begin("stem-wifi", false) ||
      preferences.putBytes("settings", &candidate, sizeof(candidate)) != sizeof(candidate))
  {
    preferences.end();
    server.send(500, "text/plain", "Không thể lưu cài đặt WiFi");
    return;
  }
  preferences.end();
  wifiSettings = candidate;
  server.send(200, "text/plain", "Đã lưu cài đặt WiFi; đang khởi động lại...");
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
    Serial.print("Ngưỡng BẬT mới: ");
    Serial.println(new_on);
    Serial.print("Ngưỡng TẮT mới: ");
    Serial.println(new_off);
    Serial.print("Thời gian trễ mới: ");
    Serial.println(new_delay);

    // Redirect back to root
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Đang chuyển hướng...");
  }
  else
  {
    server.send(400, "text/plain", "Thiếu tham số");
  }
}

void handleMoisture()
{
  uint32_t sd, sw, wd, ww, sa;
  if (!server.hasArg("soil_dry") || !server.hasArg("soil_wet") || !server.hasArg("water_dry") || !server.hasArg("water_wet") || !server.hasArg("soil_alert") || !stem::number(server.arg("soil_dry").c_str(), 4095, sd) || !stem::number(server.arg("soil_wet").c_str(), 4095, sw) || !stem::number(server.arg("water_dry").c_str(), 4095, wd) || !stem::number(server.arg("water_wet").c_str(), 4095, ww) || !stem::number(server.arg("soil_alert").c_str(), 95, sa) || sd == sw || wd == ww)
  {
    if (server.uri() == "/api/moisture")
      sendJson(400, "{\"ok\":false,\"reason\":\"Cài đặt độ ẩm không hợp lệ\"}");
    else
      server.send(400, "text/plain", "Cài đặt độ ẩm không hợp lệ");
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
    if (server.uri() == "/api/moisture")
      sendJson(400, "{\"ok\":false,\"reason\":\"Cài đặt bị từ chối hoặc không thể ghi bộ nhớ\"}");
    else
      server.send(400, "text/plain", "Cài đặt bị từ chối hoặc không thể ghi bộ nhớ");
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
  if (server.uri() == "/api/moisture")
    sendJson(200, "{\"ok\":true}");
  else
  {
    server.sendHeader("Location", "/");
    server.send(303, "text/plain", "Đã lưu");
  }
}

void handleNotFound()
{
  if (server.uri().startsWith("/api/"))
    sendJson(404, "{\"ok\":false,\"reason\":\"Không tìm thấy đường dẫn hoặc phương thức\"}");
  else
    server.send(404, "text/plain", "Không tìm thấy");
}

void handleWebControl()
{
  if (server.uri() == "/command") apiCommand();
  else apiMode();
}

void setupWebServer()
{
  server.on("/api/status", HTTP_GET, apiStatus);
  server.on("/api/config", HTTP_GET, apiConfig);
  server.on("/api/command", HTTP_POST, apiCommand);
  server.on("/api/mode", HTTP_POST, apiMode);
  server.on("/command", HTTP_POST, handleWebControl);
  server.on("/mode", HTTP_POST, handleWebControl);
  server.on("/api/time", HTTP_POST, apiTime);
  server.on("/api/moisture", HTTP_POST, handleMoisture);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/config", HTTP_GET, handleConfig);
  server.on("/wifi", HTTP_POST, handleWiFi);
  server.on("/moisture", HTTP_POST, handleMoisture);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Đã khởi động máy chủ HTTP");
}

void setupWiFi()
{
  loadWiFiSettings();
  if (!validWiFiSettings(wifiSettings))
  {
    Serial.println("Mặc định WiFi không hợp lệ; hãy kiểm tra cấu hình nguồn");
    return;
  }
  WiFi.persistent(false);
  if (wifiSettings.staSsid[0])
  {
    Serial.print("Khởi động WiFi chế độ STA, SSID: ");
    Serial.println(wifiSettings.staSsid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSettings.staSsid, wifiSettings.staPassword);
    unsigned long started = millis();
    unsigned long lastReport = started;
    while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_TIMEOUT_MS)
    {
      delay(100);
      if (millis() - lastReport >= 3000)
      {
        lastReport = millis();
        Serial.printf("Đang kết nối STA (%lu/%lu giây), trạng thái: %d\n",
                      (lastReport - started) / 1000, WIFI_TIMEOUT_MS / 1000, (int)WiFi.status());
      }
    }
    if (WiFi.status() != WL_CONNECTED)
      Serial.printf("STA không kết nối được sau %lu giây (trạng thái: %d)\n",
                    WIFI_TIMEOUT_MS / 1000, (int)WiFi.status());
  }
  else
  {
    Serial.println("SSID STA trống; bỏ qua kết nối STA");
  }
  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.print("Địa chỉ IP trạm: ");
    Serial.println(WiFi.localIP());
  }
  else
  {
    Serial.print("Chuyển sang chế độ AP, SSID: ");
    Serial.println(wifiSettings.apSsid);
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    wifi_ap_mode = WiFi.softAP(wifiSettings.apSsid, wifiSettings.apPassword);
    if (wifi_ap_mode)
    {
      Serial.print("Địa chỉ IP AP: ");
      Serial.println(WiFi.softAPIP());
    }
    else
    {
      Serial.println("Không thể khởi động điểm truy cập");
    }
  }

  // Start mDNS
  if (!MDNS.begin("stem"))
  {
    Serial.println("Lỗi thiết lập bộ đáp ứng mDNS!");
  }
  else
  {
    Serial.println("Đã khởi động bộ đáp ứng mDNS");
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

  Serial.println("\nĐang khởi động hệ thống STEM...");

  // Initialize I2C
  Wire.begin(DAT_PIN, SCL_PIN);
  Serial.println("Đã khởi tạo I2C");

  // Initialize relay outputs HIGH before enabling outputs: active-LOW means OFF.
  digitalWrite(PUMP_PIN, HIGH);
  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(LAMP_PIN, HIGH);
  pinMode(LAMP_PIN, OUTPUT);
  trang_thai_den = false;
  pump.on = false;
  loadControlConfig();

  // Initialize BH1750 light sensor
  lightInitialized = lightsensor.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
  lan_doc_cuoi = millis();
  if (!lightInitialized)
  {
    Serial.println("Không tìm thấy BH1750!");
  }
  else
  {
    Serial.println("Đã tìm thấy BH1750!");
  }

  // Initialize DS3231 RTC
  setupRTC();

  // Initialize WiFi
  setupWiFi();

  // Initialize web server
  setupWebServer();

  Serial.println("Hoàn tất thiết lập!");
}

// ========== MAIN LOOP ==========
void loop()
{
  updatePump();
  sampleMoisture();
  updatePump();
  // Handle sensor reading
  if (!lightInitialized)
  {
    if (millis() - lan_doc_cuoi >= 5000)
    {
      lan_doc_cuoi = millis();
      lightInitialized = lightsensor.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
      cam_bien_ok = false;
      lux = lux_trung_binh = NAN;
      luxAverage.clear();
      dang_cho = false;
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
