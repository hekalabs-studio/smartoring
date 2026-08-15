// ==================== BLYNK CONFIG (HARUS DI PALING ATAS) ====================
#define BLYNK_TEMPLATE_ID "TMPL6pMvZkpCV"
#define BLYNK_TEMPLATE_NAME "Smartoring"
#define BLYNK_AUTH_TOKEN "RQYO1XM6r56Tt-6nYvot4GniupFynfbJ"
#define BLYNK_PRINT Serial

#include <WiFi.h>
#include <WebServer.h>
#include "DHT.h"
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <BlynkSimpleEsp32.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <WiFiManager.h> // library: "WiFiManager" by tzapu (install via Library Manager)

// ==================== PIN DEFINITIONS ====================
#define DHTPIN 4
#define BUZZER 15
#define BUTTON 25
#define DHTTYPE DHT22
#define RELAY_KIPAS 26
#define RELAY_HUMIDIFIER 27

#define ON LOW
#define OFF HIGH

// ==================== WIFI ====================
// Kredensial WiFi TIDAK lagi ditulis manual di kode.
// Diatur lewat portal konfigurasi WiFiManager saat pertama nyala,
// atau saat direset lewat tombol BUTTON (tahan 3 detik).
WiFiManager wm;

// ==================== BLYNK VIRTUAL PIN MAP ====================
// V0  -> Suhu (display)
// V1  -> Kelembapan (display)
// V2  -> Kontrol Kipas       (0=OFF, 1=ON, 2=AUTO)
// V3  -> Kontrol Humidifier  (0=OFF, 1=ON, 2=AUTO)
// V4  -> Status Kipas (LED 0/255)
// V5  -> Status Humidifier (LED 0/255)
// V6  -> Sensor Error (LED 0/255)
// V7  -> Threshold tempOn
// V8  -> Threshold tempOff
// V9  -> Threshold humOn
// V10 -> Threshold humOff
// V12 -> Uptime (string)

// ==================== OBJECTS ====================
Adafruit_SSD1306 lcd(128, 64, &Wire, -1);
DHT dht(DHTPIN, DHTTYPE);
WebServer server(80);
BlynkTimer blynkTimer;
Preferences prefs;

// ==================== STATE ====================
float h = 0, t = 0;
bool buzzerState = false;
bool sensorError = false;
bool wifiError = false;
bool showIP = false;

bool kipasOverride = false;
bool humidifierOverride = false;

float tempOn = 28.0;
float tempOff = 26.0;
float humOn = 80.0;
float humOff = 90.0;

float tempMin = 999.0, tempMax = -999.0;
float humMin = 999.0, humMax = -999.0;

unsigned long kipasOnCount = 0;
unsigned long humidifierOnCount = 0;

#define HISTORY_SIZE 60
float tempHistory[HISTORY_SIZE];
float humHistory[HISTORY_SIZE];
unsigned long timeHistory[HISTORY_SIZE];
int historyIndex = 0;
int historyCount = 0;

// ==================== TIMERS ====================
unsigned long lastBuzzer = 0;
unsigned long lastLCD = 0;
unsigned long lastDHT = 0;
unsigned long lastButton = 0;
unsigned long lastWifiCheck = 0;
unsigned long lastBlynkCheck = 0;
unsigned long showIPStart = 0;
unsigned long uptimeStart = 0;

const unsigned long buzzerTime = 500;
const unsigned long LCDTime = 500;
const unsigned long dhtInterval = 2000;
const unsigned long buttonTime = 300;
const unsigned long wifiCheckTime = 10000;
const unsigned long blynkCheckTime = 15000;
const unsigned long showIPDuration = 3000;

// ==================== HELPERS ====================
void ALARM()
{
  if (millis() - lastBuzzer >= buzzerTime)
  {
    buzzerState = !buzzerState;
    digitalWrite(BUZZER, buzzerState);
    lastBuzzer = millis();
  }
}
void ALARM_OFF()
{
  digitalWrite(BUZZER, LOW);
  buzzerState = false;
}
bool anyError() { return sensorError || wifiError; }

// Ditampilkan di OLED saat ESP32 membuka Access Point konfigurasi WiFi
// (dipanggil otomatis oleh WiFiManager saat gagal connect ke WiFi tersimpan)
void configModeCallback(WiFiManager *myWM)
{
  lcd.clearDisplay();
  lcd.setTextSize(1);
  lcd.setCursor(0, 0);
  lcd.println("== SETUP WIFI ==");
  lcd.println();
  lcd.println("Connect HP ke:");
  lcd.println("Smartoring-Setup");
  lcd.println();
  lcd.print("Lalu buka: ");
  lcd.println(WiFi.softAPIP());
  lcd.display();
  Serial.println("[WiFiManager] Mode AP aktif - konfigurasi via browser.");
}

// ==================== THRESHOLD PERSISTENCE (NVS) ====================
// Menyimpan pengaturan ambang batas secara permanen agar tidak reset
// setiap kali ESP32 restart -- penting karena alat ini dipakai bergantian
// di berbagai jenis ruang dengan kebutuhan ambang yang berbeda-beda.
void loadThresholds()
{
  prefs.begin("smartoring", true);
  tempOn = prefs.getFloat("tempOn", tempOn);
  tempOff = prefs.getFloat("tempOff", tempOff);
  humOn = prefs.getFloat("humOn", humOn);
  humOff = prefs.getFloat("humOff", humOff);
  prefs.end();
}

void saveThresholds()
{
  prefs.begin("smartoring", false);
  prefs.putFloat("tempOn", tempOn);
  prefs.putFloat("tempOff", tempOff);
  prefs.putFloat("humOn", humOn);
  prefs.putFloat("humOff", humOff);
  prefs.end();
}

// Mencegah relay menyala/mati terus-menerus (flapping) akibat ambang
// yang salah diisi, misalnya tempOn <= tempOff.
bool validThresholds(float tOn, float tOff, float hOn, float hOff)
{
  return (tOn > tOff) && (hOff > hOn);
}

String uptimeString()
{
  unsigned long s = (millis() - uptimeStart) / 1000;
  unsigned long m = s / 60;
  s %= 60;
  unsigned long hr = m / 60;
  m %= 60;
  char buf[16];
  sprintf(buf, "%02lu:%02lu:%02lu", hr, m, s);
  return String(buf);
}

// ==================== BLYNK ====================
BLYNK_WRITE(V2)
{
  int val = param.asInt();
  if (val == 1)
  {
    kipasOverride = true;
    digitalWrite(RELAY_KIPAS, ON);
    Blynk.logEvent("device_control", "Kipas ON manual");
  }
  else if (val == 0)
  {
    kipasOverride = true;
    digitalWrite(RELAY_KIPAS, OFF);
    Blynk.logEvent("device_control", "Kipas OFF manual");
  }
  else if (val == 2)
  {
    kipasOverride = false;
    Blynk.logEvent("device_control", "Kipas AUTO");
  }
}
BLYNK_WRITE(V3)
{
  int val = param.asInt();
  if (val == 1)
  {
    humidifierOverride = true;
    digitalWrite(RELAY_HUMIDIFIER, ON);
    Blynk.logEvent("device_control", "Humidifier ON manual");
  }
  else if (val == 0)
  {
    humidifierOverride = true;
    digitalWrite(RELAY_HUMIDIFIER, OFF);
    Blynk.logEvent("device_control", "Humidifier OFF manual");
  }
  else if (val == 2)
  {
    humidifierOverride = false;
    Blynk.logEvent("device_control", "Humidifier AUTO");
  }
}
// Catatan: Blynk mengirim tiap virtual pin satu per satu, jadi validasi
// silang (tOn > tOff) dilakukan penuh di endpoint /api/threshold. Di sini
// nilai baru hanya disimpan jika kombinasinya masih masuk akal terhadap
// nilai pasangannya saat ini.
BLYNK_WRITE(V7)
{
  float v = param.asFloat();
  if (v > tempOff) { tempOn = v; saveThresholds(); }
}
BLYNK_WRITE(V8)
{
  float v = param.asFloat();
  if (v < tempOn) { tempOff = v; saveThresholds(); }
}
BLYNK_WRITE(V9)
{
  float v = param.asFloat();
  if (v < humOff) { humOn = v; saveThresholds(); }
}
BLYNK_WRITE(V10)
{
  float v = param.asFloat();
  if (v > humOn) { humOff = v; saveThresholds(); }
}
BLYNK_CONNECTED()
{
  Blynk.syncVirtual(V2, V3, V7, V8, V9, V10);
  Serial.println("[Blynk] Connected & synced");
}

void sendToBlynk()
{
  if (!Blynk.connected())
    return;
  Blynk.virtualWrite(V0, sensorError ? -1 : t);
  Blynk.virtualWrite(V1, sensorError ? -1 : h);
  Blynk.virtualWrite(V4, digitalRead(RELAY_KIPAS) == LOW ? 255 : 0);
  Blynk.virtualWrite(V5, digitalRead(RELAY_HUMIDIFIER) == LOW ? 255 : 0);
  Blynk.virtualWrite(V6, sensorError ? 255 : 0);
  Blynk.virtualWrite(V12, uptimeString());
  static bool tempAlertSent = false, humAlertSent = false;
  if (!sensorError)
  {
    if (t > tempOn + 2.0 && !tempAlertSent)
    {
      Blynk.logEvent("temp_alert", String("SUHU KRITIS: ") + t + "C! Periksa kipas.");
      tempAlertSent = true;
    }
    else if (t <= tempOn)
      tempAlertSent = false;
    if (h < humOn - 10.0 && !humAlertSent)
    {
      Blynk.logEvent("hum_alert", String("KELEMBAPAN RENDAH: ") + h + "%! Periksa humidifier.");
      humAlertSent = true;
    }
    else if (h >= humOn)
      humAlertSent = false;
  }
}

// ==================== WEB API ====================
void handleApiData()
{
  JsonDocument doc;
  doc["temp"] = isnan(t) ? 0 : t;
  doc["hum"] = isnan(h) ? 0 : h;
  doc["sensorError"] = sensorError;
  doc["wifiError"] = wifiError;
  doc["kipas"] = (digitalRead(RELAY_KIPAS) == LOW);
  doc["humidifier"] = (digitalRead(RELAY_HUMIDIFIER) == LOW);
  doc["kipasOverride"] = kipasOverride;
  doc["humidOverride"] = humidifierOverride;
  doc["tempOn"] = tempOn;
  doc["tempOff"] = tempOff;
  doc["humOn"] = humOn;
  doc["humOff"] = humOff;
  doc["uptime"] = uptimeString();
  doc["ip"] = WiFi.localIP().toString();
  doc["blynk"] = Blynk.connected();
  doc["tempMin"] = (tempMin == 999.0) ? 0 : tempMin;
  doc["tempMax"] = (tempMax == -999.0) ? 0 : tempMax;
  doc["humMin"] = (humMin == 999.0) ? 0 : humMin;
  doc["humMax"] = (humMax == -999.0) ? 0 : humMax;
  doc["rssi"] = WiFi.RSSI();
  doc["kipasOnCount"] = kipasOnCount;
  doc["humidifierOnCount"] = humidifierOnCount;
  JsonArray tArr = doc["tempHistory"].to<JsonArray>();
  JsonArray hArr = doc["humHistory"].to<JsonArray>();
  JsonArray tsArr = doc["timestamps"].to<JsonArray>();
  int start = (historyCount < HISTORY_SIZE) ? 0 : historyIndex;
  int count = min(historyCount, HISTORY_SIZE);
  for (int i = 0; i < count; i++)
  {
    int idx = (start + i) % HISTORY_SIZE;
    tArr.add(tempHistory[idx]);
    hArr.add(humHistory[idx]);
    tsArr.add(timeHistory[idx]);
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleApiControl()
{
  if (!server.hasArg("plain"))
  {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")))
  {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  String device = doc["device"].as<String>();
  String state = doc["state"].as<String>();
  if (device == "kipas")
  {
    if (state == "on")
    {
      kipasOverride = true;
      digitalWrite(RELAY_KIPAS, ON);
      Blynk.virtualWrite(V2, 1);
    }
    else if (state == "off")
    {
      kipasOverride = true;
      digitalWrite(RELAY_KIPAS, OFF);
      Blynk.virtualWrite(V2, 0);
    }
    else if (state == "auto")
    {
      kipasOverride = false;
      Blynk.virtualWrite(V2, 2);
    }
  }
  else if (device == "humidifier")
  {
    if (state == "on")
    {
      humidifierOverride = true;
      digitalWrite(RELAY_HUMIDIFIER, ON);
      Blynk.virtualWrite(V3, 1);
    }
    else if (state == "off")
    {
      humidifierOverride = true;
      digitalWrite(RELAY_HUMIDIFIER, OFF);
      Blynk.virtualWrite(V3, 0);
    }
    else if (state == "auto")
    {
      humidifierOverride = false;
      Blynk.virtualWrite(V3, 2);
    }
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiThreshold()
{
  if (!server.hasArg("plain"))
  {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")))
  {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  float newTempOn = doc["tempOn"].is<float>() ? doc["tempOn"].as<float>() : tempOn;
  float newTempOff = doc["tempOff"].is<float>() ? doc["tempOff"].as<float>() : tempOff;
  float newHumOn = doc["humOn"].is<float>() ? doc["humOn"].as<float>() : humOn;
  float newHumOff = doc["humOff"].is<float>() ? doc["humOff"].as<float>() : humOff;

  if (!validThresholds(newTempOn, newTempOff, newHumOn, newHumOff))
  {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"Ambang tidak valid: Suhu ON harus > Suhu OFF, dan Kelembapan OFF harus > Kelembapan ON\"}");
    return;
  }

  tempOn = newTempOn;
  tempOff = newTempOff;
  humOn = newHumOn;
  humOff = newHumOff;
  saveThresholds();

  Blynk.virtualWrite(V7, tempOn);
  Blynk.virtualWrite(V8, tempOff);
  Blynk.virtualWrite(V9, humOn);
  Blynk.virtualWrite(V10, humOff);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiResetMinMax()
{
  tempMin = 999.0;
  tempMax = -999.0;
  humMin = 999.0;
  humMax = -999.0;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleRoot()
{
  String html = R"rawhtml(<!DOCTYPE html>
<html lang="id">
<head>
<meta charset="UTF-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1.0"/>
<title>OWL Tech - Kandang Monitor</title>
<link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700;800&family=JetBrains+Mono:wght@400;600&display=swap" rel="stylesheet"/>
<script src="https://cdnjs.cloudflare.com/ajax/libs/Chart.js/4.4.1/chart.umd.min.js"></script>
<style>
:root{
  --bg:#0d1b2e;--surface:#112240;--surface2:#0a1628;--border:#1e3a5f;--border2:#2d5a8e;
  --accent:#3b9eff;--accent2:#5bb8ff;--accent-t:#ff8c42;--accent-h:#22d3ee;
  --accent-ok:#34d399;--accent-err:#f87171;--accent-warn:#fbbf24;
  --text:#cce4ff;--text2:#7ab8f5;--muted:#4a7aaa;--radius:14px;
}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--text);font-family:'Inter',sans-serif;min-height:100vh;overflow-x:hidden;font-size:16px}
header{display:flex;align-items:center;justify-content:space-between;padding:16px 28px;position:sticky;top:0;background:#0a1628;border-bottom:1px solid var(--border);z-index:100;box-shadow:0 2px 20px rgba(0,0,0,.4)}
.logo{display:flex;align-items:center;gap:12px}
.logo-icon{width:40px;height:40px;background:rgba(59,158,255,.15);border-radius:10px;display:flex;align-items:center;justify-content:center;font-size:22px;border:1px solid rgba(59,158,255,.3)}
.logo-text{font-size:1.2rem;font-weight:800;letter-spacing:-.02em;color:#e8f4ff}
.logo-text span{color:var(--accent2)}
.header-badges{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
#statusBadge,#blynkBadge{display:flex;align-items:center;gap:7px;font-family:'JetBrains Mono',monospace;font-size:.78rem;padding:6px 14px;border-radius:20px;border:1px solid var(--border);background:var(--surface);color:var(--muted);transition:.3s}
#statusBadge.ok{border-color:rgba(52,211,153,.4);background:rgba(52,211,153,.1);color:#34d399}
#statusBadge.err{border-color:rgba(248,113,113,.4);background:rgba(248,113,113,.1);color:#f87171}
#blynkBadge.ok{border-color:rgba(167,139,250,.4);background:rgba(167,139,250,.1);color:#a78bfa}
#blynkBadge.err{border-color:var(--border);background:var(--surface);color:var(--muted)}
.dot{width:7px;height:7px;border-radius:50%;background:currentColor;animation:pulse 1.5s infinite;flex-shrink:0}
@keyframes pulse{0%,100%{opacity:1;transform:scale(1)}50%{opacity:.4;transform:scale(.7)}}
main{max-width:1280px;margin:0 auto;padding:28px 20px;display:grid;gap:22px}

/* ===================== GAUGE — FIXED ===================== */
.gauge-row{display:grid;grid-template-columns:1fr 1fr;gap:18px}
@media(max-width:600px){.gauge-row{grid-template-columns:1fr}}

.gauge-card{
  background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);
  padding:22px 26px 20px;box-shadow:0 4px 20px rgba(0,0,0,.3);
  display:flex;flex-direction:column;align-items:center;gap:10px
}
.gauge-title{
  font-size:.78rem;font-weight:700;text-transform:uppercase;letter-spacing:.1em;
  color:var(--muted);align-self:flex-start
}

/* Container: lebar 240, tinggi 150 agar arc punya ruang + nilai tidak tumpang tindih */
.gauge-wrap{position:relative;width:240px;height:150px;flex-shrink:0}
.gauge-wrap canvas{position:absolute;top:0;left:0;width:240px!important;height:150px!important}

/* Nilai berada di tengah bawah canvas, TIDAK menimpa arc */
.gauge-value-wrap{
  position:absolute;
  bottom:4px;          /* jarak dari bawah canvas */
  left:50%;transform:translateX(-50%);
  text-align:center;white-space:nowrap;
  display:flex;flex-direction:column;align-items:center;gap:2px
}
.gauge-val{
  font-family:'JetBrains Mono',monospace;font-size:2.5rem;font-weight:700;line-height:1
}
.gauge-unit{font-size:.75rem;color:var(--muted);font-family:'JetBrains Mono',monospace}

/* Trend di bawah gauge-wrap, bukan di dalam angka */
.gauge-trend-row{
  display:flex;justify-content:center;width:100%
}
.trend-badge{
  display:inline-flex;align-items:center;gap:4px;
  font-size:.75rem;font-family:'JetBrains Mono',monospace;
  padding:3px 10px;border-radius:12px
}
.trend-up{background:rgba(248,113,113,.15);color:#f87171}
.trend-down{background:rgba(52,211,153,.15);color:#34d399}
.trend-stable{background:rgba(59,158,255,.1);color:var(--accent)}

.gauge-minmax{
  display:flex;gap:20px;font-family:'JetBrains Mono',monospace;
  font-size:.74rem;color:var(--muted)
}
.gauge-minmax span{display:flex;align-items:center;gap:4px}
.gauge-minmax .mn{color:var(--accent2)}
.gauge-minmax .mx{color:var(--accent-warn)}
.gauge-sub{font-size:.8rem;font-family:'JetBrains Mono',monospace;color:var(--muted)}
.gauge-sub.ok{color:var(--accent-ok)}
.gauge-sub.warn{color:var(--accent-warn)}
.gauge-sub.err{color:var(--accent-err)}
/* ========================================================= */

.metrics-row{display:grid;grid-template-columns:1fr 1fr;gap:18px}
@media(max-width:500px){.metrics-row{grid-template-columns:1fr}}
.metric-card{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:20px 22px;position:relative;overflow:hidden;transition:border-color .3s,box-shadow .3s;box-shadow:0 4px 20px rgba(0,0,0,.3)}
.metric-card.temp{border-top:3px solid var(--accent-t)}
.metric-card.hum{border-top:3px solid var(--accent-h)}
.metric-card.warn{border-color:var(--accent-warn)!important;box-shadow:0 4px 20px rgba(251,191,36,.1)!important}
.metric-card.danger{border-color:var(--accent-err)!important;box-shadow:0 4px 20px rgba(248,113,113,.12)!important}
.metric-label{font-size:.78rem;font-weight:700;letter-spacing:.1em;text-transform:uppercase;color:var(--muted);margin-bottom:8px}
.metric-value{font-family:'JetBrains Mono',monospace;font-size:3.4rem;font-weight:700;line-height:1;transition:color .3s}
.metric-card.temp .metric-value{color:var(--accent-t)}
.metric-card.hum .metric-value{color:var(--accent-h)}
.metric-sub{font-size:.82rem;color:var(--muted);margin-top:8px;font-family:'JetBrains Mono',monospace}
.metric-icon{position:absolute;right:20px;top:50%;transform:translateY(-50%);font-size:4rem;opacity:.06}

.status-bar{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:12px}
.status-item{background:var(--surface);border:1px solid var(--border);border-radius:10px;padding:14px 16px;display:flex;flex-direction:column;gap:5px;box-shadow:0 2px 10px rgba(0,0,0,.25)}
.status-item-label{font-size:.68rem;text-transform:uppercase;letter-spacing:.1em;color:var(--muted);font-weight:700}
.status-item-value{font-family:'JetBrains Mono',monospace;font-size:.92rem;font-weight:600;color:var(--text)}
.rssi-bar{display:flex;gap:3px;align-items:flex-end;height:18px;margin-top:4px}
.rssi-bar span{width:5px;border-radius:2px;background:var(--border)}
.rssi-bar span.active{background:var(--accent-ok)}
.rssi-bar span.warn{background:var(--accent-warn)}
.rssi-bar span.bad{background:var(--accent-err)}

.charts-row{display:grid;grid-template-columns:1fr 1fr;gap:18px}
@media(max-width:700px){.charts-row{grid-template-columns:1fr}}
.chart-card{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:22px;box-shadow:0 4px 20px rgba(0,0,0,.3)}
.card-title{font-size:.8rem;font-weight:700;text-transform:uppercase;letter-spacing:.1em;color:var(--text2);margin-bottom:16px;display:flex;align-items:center;justify-content:space-between;gap:6px}
.chart-wrap{position:relative;height:180px}

.controls-grid{display:grid;grid-template-columns:1fr 1fr;gap:18px}
@media(max-width:600px){.controls-grid{grid-template-columns:1fr}}
.control-card{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:22px;box-shadow:0 4px 20px rgba(0,0,0,.3)}
.device-header{display:flex;align-items:center;justify-content:space-between;margin-bottom:18px}
.device-name{font-size:1.05rem;font-weight:700;color:var(--text)}
.device-icon{font-size:1.5rem;margin-bottom:4px}
.device-counter{font-family:'JetBrains Mono',monospace;font-size:.68rem;color:var(--muted);margin-top:2px}
.state-badge{font-family:'JetBrains Mono',monospace;font-size:.72rem;padding:5px 13px;border-radius:20px;font-weight:700;letter-spacing:.05em}
.state-badge.on{background:rgba(52,211,153,.12);color:#34d399;border:1px solid rgba(52,211,153,.3)}
.state-badge.off{background:rgba(74,122,170,.1);color:var(--muted);border:1px solid var(--border)}
.btn-row{display:flex;gap:8px;flex-wrap:wrap}
.btn{font-family:'Inter',sans-serif;font-size:.82rem;font-weight:600;padding:9px 16px;border-radius:8px;border:1px solid var(--border2);cursor:pointer;transition:all .2s;letter-spacing:.02em;background:var(--surface2);color:var(--text2)}
.btn:hover{transform:translateY(-1px);border-color:var(--accent);color:var(--accent);box-shadow:0 3px 12px rgba(59,158,255,.2)}
.btn.on{background:rgba(52,211,153,.1);border-color:rgba(52,211,153,.4);color:#34d399}
.btn.off{background:rgba(248,113,113,.08);border-color:rgba(248,113,113,.3);color:#f87171}
.btn.auto{background:rgba(59,158,255,.1);border-color:rgba(59,158,255,.4);color:var(--accent)}
.btn.active{box-shadow:0 0 0 2px currentColor;font-weight:700}
.btn-sm{font-size:.72rem;padding:5px 10px}

.threshold-card{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:24px;box-shadow:0 4px 20px rgba(0,0,0,.3)}
.threshold-grid{display:grid;grid-template-columns:1fr 1fr;gap:16px;margin-top:16px}
@media(max-width:500px){.threshold-grid{grid-template-columns:1fr}}
.input-group{display:flex;flex-direction:column;gap:7px}
.input-group label{font-size:.74rem;text-transform:uppercase;letter-spacing:.08em;color:var(--muted);font-weight:700}
.input-group input{background:var(--surface2);border:1px solid var(--border);border-radius:8px;color:var(--text);font-family:'JetBrains Mono',monospace;font-size:.95rem;padding:10px 14px;width:100%;outline:none;transition:border-color .2s}
.input-group input:focus{border-color:var(--accent);box-shadow:0 0 0 3px rgba(59,158,255,.15)}
.btn-save{margin-top:18px;width:100%;padding:13px;background:linear-gradient(90deg,#1456b8,#3b9eff);border:none;border-radius:8px;color:#e8f4ff;font-family:'Inter',sans-serif;font-size:.92rem;font-weight:700;cursor:pointer;letter-spacing:.04em;transition:opacity .2s,box-shadow .2s;box-shadow:0 3px 16px rgba(59,158,255,.25)}
.btn-save:hover{opacity:.88;box-shadow:0 4px 24px rgba(59,158,255,.4)}

.reco-card{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:24px;box-shadow:0 4px 20px rgba(0,0,0,.3)}
#recoList{list-style:none;display:flex;flex-direction:column;gap:10px;margin-top:16px}
#recoList li{display:flex;align-items:flex-start;gap:10px;font-size:.9rem;line-height:1.55;color:var(--text);padding:12px 14px;border-radius:8px;background:var(--surface2);border-left:3px solid var(--accent)}
#recoList li.warn-item{border-left-color:var(--accent-warn);background:rgba(251,191,36,.06)}
#recoList li.err-item{border-left-color:var(--accent-err);background:rgba(248,113,113,.06)}
#recoList li.ok-item{border-left-color:var(--accent-ok);background:rgba(52,211,153,.06)}
.reco-icon{font-size:1.1rem;flex-shrink:0;margin-top:1px}

.log-card{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:24px;box-shadow:0 4px 20px rgba(0,0,0,.3)}
#logBox{background:var(--surface2);border:1px solid var(--border);border-radius:8px;padding:14px;height:160px;overflow-y:auto;font-family:'JetBrains Mono',monospace;font-size:.78rem;color:var(--muted);margin-top:14px;scroll-behavior:smooth}
#logBox .log-entry{padding:3px 0;border-bottom:1px solid rgba(59,158,255,.06)}
#logBox .log-ok{color:#34d399}
#logBox .log-warn{color:#fbbf24}
#logBox .log-err{color:#f87171}
#logBox .log-info{color:var(--accent2)}
.info-row{display:flex;align-items:center;justify-content:space-between;gap:8px;flex-wrap:wrap;padding:14px 0;border-top:1px solid var(--border);margin-top:4px}
.info-chip{font-family:'JetBrains Mono',monospace;font-size:.78rem;color:var(--muted)}
.info-chip span{color:var(--text2);margin-left:4px;font-weight:600}

#toast{position:fixed;bottom:24px;right:24px;background:var(--surface);border:1px solid var(--border2);padding:13px 20px;border-radius:10px;font-size:.84rem;font-family:'JetBrains Mono',monospace;opacity:0;transform:translateY(10px);transition:.3s;pointer-events:none;z-index:999;max-width:280px;box-shadow:0 6px 24px rgba(0,0,0,.5);color:var(--text)}
#toast.show{opacity:1;transform:translateY(0)}
#toast.ok{border-color:rgba(52,211,153,.5);color:#34d399}
#toast.err{border-color:rgba(248,113,113,.5);color:#f87171}
#toast.warn{border-color:rgba(251,191,36,.5);color:#fbbf24}

#alarmOverlay{position:fixed;inset:0;background:rgba(248,113,113,.08);border:3px solid #f87171;z-index:998;pointer-events:none;opacity:0;transition:.5s}
#alarmOverlay.active{opacity:1;animation:alarmPulse 1s infinite}
@keyframes alarmPulse{0%,100%{opacity:.3}50%{opacity:1}}
#alarmBanner{position:fixed;top:70px;left:50%;transform:translateX(-50%);background:#1a0a0a;border:1.5px solid #f87171;border-radius:10px;padding:12px 24px;font-family:'JetBrains Mono',monospace;font-size:.9rem;color:#f87171;z-index:1000;display:none;box-shadow:0 4px 24px rgba(248,113,113,.3);text-align:center}
#alarmBanner.active{display:block}

@keyframes fadeIn{from{opacity:0;transform:translateY(12px)}to{opacity:1;transform:translateY(0)}}
main>*{animation:fadeIn .45s ease both}
main>*:nth-child(1){animation-delay:.05s}main>*:nth-child(2){animation-delay:.1s}
main>*:nth-child(3){animation-delay:.15s}main>*:nth-child(4){animation-delay:.2s}
main>*:nth-child(5){animation-delay:.25s}main>*:nth-child(6){animation-delay:.3s}
main>*:nth-child(7){animation-delay:.35s}main>*:nth-child(8){animation-delay:.4s}
main>*:nth-child(9){animation-delay:.45s}
</style>
</head>
<body>
<div id="alarmOverlay"></div>
<div id="alarmBanner"></div>
<header>
  <div class="logo"><div class="logo-icon">&#x1F989;</div><div class="logo-text">OWL<span>Tech</span></div></div>
  <div class="header-badges">
    <div id="blynkBadge" class="err"><span class="dot"></span><span id="blynkText">BLYNK</span></div>
    <div id="statusBadge" class="ok"><span class="dot"></span><span id="statusText">ONLINE</span></div>
  </div>
</header>
<main>

  <!-- GAUGE ROW — struktur HTML dipisah agar trend tidak tumpang tindih -->
  <div class="gauge-row">
    <div class="gauge-card">
      <div class="gauge-title">&#x1F321; Suhu Ruangan</div>
      <div class="gauge-wrap">
        <canvas id="gaugeTemp"></canvas>
        <div class="gauge-value-wrap">
          <span class="gauge-val" id="gaugeTempVal" style="color:#ff8c42">--.-</span>
          <span class="gauge-unit">&#xB0;C</span>
        </div>
      </div>
      <!-- trend di luar gauge-wrap supaya tidak tumpang tindih dengan arc -->
      <div class="gauge-trend-row" id="trendTempRow"></div>
      <div class="gauge-minmax">
        <span>&#x2193; Min: <b class="mn" id="tempMinVal">--</b>&#xB0;C</span>
        <span>&#x2191; Max: <b class="mx" id="tempMaxVal">--</b>&#xB0;C</span>
      </div>
      <div class="gauge-sub" id="gaugeSubTemp">Memuat data...</div>
    </div>
    <div class="gauge-card">
      <div class="gauge-title">&#x1F4A7; Kelembapan</div>
      <div class="gauge-wrap">
        <canvas id="gaugeHum"></canvas>
        <div class="gauge-value-wrap">
          <span class="gauge-val" id="gaugeHumVal" style="color:#22d3ee">--.-</span>
          <span class="gauge-unit">%</span>
        </div>
      </div>
      <!-- trend di luar gauge-wrap -->
      <div class="gauge-trend-row" id="trendHumRow"></div>
      <div class="gauge-minmax">
        <span>&#x2193; Min: <b class="mn" id="humMinVal">--</b>%</span>
        <span>&#x2191; Max: <b class="mx" id="humMaxVal">--</b>%</span>
      </div>
      <div class="gauge-sub" id="gaugeSubHum">Memuat data...</div>
    </div>
  </div>

  <!-- STATUS BAR -->
  <div class="status-bar">
    <div class="status-item"><div class="status-item-label">Kipas</div><div class="status-item-value" id="st_kipas">-</div></div>
    <div class="status-item"><div class="status-item-label">Humidifier</div><div class="status-item-value" id="st_hum">-</div></div>
    <div class="status-item"><div class="status-item-label">Sensor</div><div class="status-item-value" id="st_sensor">-</div></div>
    <div class="status-item"><div class="status-item-label">Uptime</div><div class="status-item-value" id="st_uptime">-</div></div>
    <div class="status-item"><div class="status-item-label">Blynk Cloud</div><div class="status-item-value" id="st_blynk">-</div></div>
    <div class="status-item">
      <div class="status-item-label">Sinyal WiFi</div>
      <div class="status-item-value" id="st_rssi">-</div>
      <div class="rssi-bar" id="rssiBar">
        <span style="height:5px" id="r1"></span><span style="height:9px" id="r2"></span>
        <span style="height:13px" id="r3"></span><span style="height:17px" id="r4"></span>
      </div>
    </div>
    <div class="status-item"><div class="status-item-label">&#x1F300; Kipas ON</div><div class="status-item-value" id="st_kipasCount">0x</div></div>
    <div class="status-item"><div class="status-item-label">&#x1F4A8; Humid ON</div><div class="status-item-value" id="st_humCount">0x</div></div>
  </div>

  <!-- CHARTS -->
  <div class="charts-row">
    <div class="chart-card">
      <div class="card-title">
        <span>&#x1F4C8; Riwayat Suhu (&#xB0;C)</span>
        <button class="btn btn-sm" onclick="exportCSV()">&#x1F4E5; Export CSV</button>
      </div>
      <div class="chart-wrap"><canvas id="chartTemp"></canvas></div>
    </div>
    <div class="chart-card">
      <div class="card-title">
        <span>&#x1F4C9; Riwayat Kelembapan (%)</span>
        <button class="btn btn-sm" onclick="exportCSV()">&#x1F4E5; Export CSV</button>
      </div>
      <div class="chart-wrap"><canvas id="chartHum"></canvas></div>
    </div>
  </div>

  <!-- CONTROLS -->
  <div class="controls-grid">
    <div class="control-card">
      <div class="device-header">
        <div><div class="device-icon">&#x1F300;</div><div class="device-name">Kipas Angin</div>
          <div class="device-counter">Relay ON: <span id="ctr_kipas">0</span>x sesi ini</div></div>
        <div class="state-badge off" id="badge_kipas">OFF</div>
      </div>
      <div class="btn-row">
        <button class="btn on" onclick="control('kipas','on')">&#x25B6; NYALA</button>
        <button class="btn off" onclick="control('kipas','off')">&#x25A0; MATI</button>
        <button class="btn auto active" id="btn_kipas_auto" onclick="control('kipas','auto')">&#x27F3; AUTO</button>
      </div>
    </div>
    <div class="control-card">
      <div class="device-header">
        <div><div class="device-icon">&#x1F4A8;</div><div class="device-name">Humidifier</div>
          <div class="device-counter">Relay ON: <span id="ctr_hum">0</span>x sesi ini</div></div>
        <div class="state-badge off" id="badge_hum">OFF</div>
      </div>
      <div class="btn-row">
        <button class="btn on" onclick="control('humidifier','on')">&#x25B6; NYALA</button>
        <button class="btn off" onclick="control('humidifier','off')">&#x25A0; MATI</button>
        <button class="btn auto active" id="btn_hum_auto" onclick="control('humidifier','auto')">&#x27F3; AUTO</button>
      </div>
    </div>
  </div>

  <!-- THRESHOLD -->
  <div class="threshold-card">
    <div class="card-title">&#x2699;&#xFE0F; Pengaturan Ambang Batas Otomatis</div>
    <div class="threshold-grid">
      <div class="input-group"><label>Suhu Nyalakan Kipas (&#xB0;C)</label><input type="number" id="in_tempOn" step="0.5" placeholder="28"/></div>
      <div class="input-group"><label>Suhu Matikan Kipas (&#xB0;C)</label><input type="number" id="in_tempOff" step="0.5" placeholder="26"/></div>
      <div class="input-group"><label>Kelembapan Nyalakan Humidifier (%)</label><input type="number" id="in_humOn" step="1" placeholder="80"/></div>
      <div class="input-group"><label>Kelembapan Matikan Humidifier (%)</label><input type="number" id="in_humOff" step="1" placeholder="90"/></div>
    </div>
    <button class="btn-save" onclick="saveThreshold()">&#x1F4BE; Simpan Pengaturan</button>
  </div>

  <!-- RECO -->
  <div class="reco-card">
    <div class="card-title">&#x1F4A1; Rekomendasi &amp; Status Kandang</div>
    <ul id="recoList"><li class="ok-item"><span class="reco-icon">&#x23F3;</span> Menunggu data sensor...</li></ul>
  </div>

  <!-- LOG -->
  <div class="log-card">
    <div class="card-title">&#x1F4CB; Log Aktivitas</div>
    <div id="logBox"></div>
    <div class="info-row">
      <div class="info-chip">IP: <span id="chipIP">-</span></div>
      <div class="info-chip">Refresh: <span id="chipRefresh">-</span>s lalu</div>
      <div style="display:flex;gap:8px;flex-wrap:wrap">
        <button class="btn btn-sm" onclick="exportCSV()">&#x1F4E5; Export CSV</button>
        <button class="btn btn-sm" onclick="resetMinMax()">&#x1F504; Reset Min/Max</button>
        <button class="btn btn-sm" onclick="clearLog()">&#x1F5D1; Bersihkan Log</button>
      </div>
    </div>
  </div>

</main>
<div id="toast"></div>
<script>
// ==================== GAUGE ====================
// Canvas 240x150: arc setengah lingkaran dari kiri ke kanan
// Pusat di (120, 128) sehingga arc muncul di atas dan nilai tidak tertimpa
function makeGauge(canvasId, color, minVal, maxVal) {
  const W = 240, H = 150;
  const CX = W / 2;
  const CY = 128;   // pusat jauh ke bawah → arc muncul besar di atas
  const R  = 100;   // radius busur
  const LW = 14;    // ketebalan busur

  const canvas = document.getElementById(canvasId);
  canvas.width  = W;
  canvas.height = H;
  const ctx = canvas.getContext('2d');

  function draw(val) {
    ctx.clearRect(0, 0, W, H);

    const SA = Math.PI;          // mulai dari kiri (180°)
    const EA = 2 * Math.PI;      // selesai di kanan (360°/0°)

    // Track (background arc)
    ctx.beginPath();
    ctx.arc(CX, CY, R, SA, EA);
    ctx.strokeStyle = '#1e3a5f';
    ctx.lineWidth   = LW;
    ctx.lineCap     = 'round';
    ctx.stroke();

    // Value arc
    const pct = Math.min(Math.max((val - minVal) / (maxVal - minVal), 0), 1);
    const VA  = SA + pct * Math.PI;
    ctx.beginPath();
    ctx.arc(CX, CY, R, SA, VA);
    ctx.strokeStyle = color;
    ctx.lineWidth   = LW;
    ctx.lineCap     = 'round';
    ctx.stroke();

    // Tick marks (5 titik: 0%, 25%, 50%, 75%, 100%)
    for (let i = 0; i <= 4; i++) {
      const a  = SA + (i / 4) * Math.PI;
      const x1 = CX + (R - 18) * Math.cos(a);
      const y1 = CY + (R - 18) * Math.sin(a);
      const x2 = CX + (R - 24) * Math.cos(a);
      const y2 = CY + (R - 24) * Math.sin(a);
      ctx.beginPath();
      ctx.moveTo(x1, y1); ctx.lineTo(x2, y2);
      ctx.strokeStyle = '#2d5a8e';
      ctx.lineWidth   = 2;
      ctx.stroke();
    }

    // Label min/max di ujung busur
    ctx.font = '10px JetBrains Mono, monospace';
    ctx.fillStyle = '#4a7aaa';
    ctx.textAlign = 'right';
    ctx.fillText(minVal, CX - R + 8, CY + 14);
    ctx.textAlign = 'left';
    ctx.fillText(maxVal, CX + R - 8, CY + 14);
  }

  draw(minVal);
  return draw;
}

const drawGaugeTemp = makeGauge('gaugeTemp', '#ff8c42', 15, 45);
const drawGaugeHum  = makeGauge('gaugeHum',  '#22d3ee',  0, 100);

// ==================== CHARTS ====================
const chartOpts = (color, unit) => ({
  responsive:true, maintainAspectRatio:false, animation:{duration:400},
  plugins:{legend:{display:false},tooltip:{
    backgroundColor:'#0a1628',borderColor:'#2d5a8e',borderWidth:1,
    titleColor:'#7ab8f5',bodyColor:'#cce4ff',
    callbacks:{label:ctx=>` ${ctx.parsed.y.toFixed(1)} ${unit}`}
  }},
  scales:{
    x:{ticks:{color:'#4a7aaa',font:{family:'JetBrains Mono',size:10},maxTicksLimit:6},grid:{color:'#1e3a5f'}},
    y:{ticks:{color:'#4a7aaa',font:{family:'JetBrains Mono',size:10}},grid:{color:'#1e3a5f'}}
  }
});
const makeDataset = (color) => ({data:[],borderColor:color,backgroundColor:color+'22',borderWidth:2.5,pointRadius:0,fill:true,tension:0.4});
const chartTemp = new Chart(document.getElementById('chartTemp').getContext('2d'),{type:'line',data:{labels:[],datasets:[makeDataset('#ff8c42')]},options:chartOpts('#ff8c42','C')});
const chartHum  = new Chart(document.getElementById('chartHum').getContext('2d'), {type:'line',data:{labels:[],datasets:[makeDataset('#22d3ee')]},options:chartOpts('#22d3ee','%')});

// ==================== STATE ====================
let lastRefresh=0, threshold={tempOn:28,tempOff:26,humOn:80,humOff:90};
let consecutiveErrors=0, prevTemp=null, prevHum=null, allData=[], alarmActive=false;

// ==================== FETCH ====================
async function fetchData() {
  try {
    const res = await fetch('/api/data');
    if (!res.ok) throw new Error('HTTP '+res.status);
    const d = await res.json();
    consecutiveErrors=0; lastRefresh=Date.now();
    threshold={tempOn:d.tempOn,tempOff:d.tempOff,humOn:d.humOn,humOff:d.humOff};
    if (!d.sensorError) allData.push({ts:new Date().toISOString(),temp:d.temp,hum:d.hum});
    if (allData.length>2000) allData.shift();
    updateGauges(d); updateStatus(d); updateCharts(d); updateControls(d);
    updateReco(d); fillThresholdInputs(d); updateAlarm(d);
    document.getElementById('chipIP').textContent=d.ip;
    document.getElementById('st_uptime').textContent=d.uptime;
    document.getElementById('st_kipasCount').textContent=d.kipasOnCount+'x';
    document.getElementById('st_humCount').textContent=d.humidifierOnCount+'x';
    document.getElementById('ctr_kipas').textContent=d.kipasOnCount;
    document.getElementById('ctr_hum').textContent=d.humidifierOnCount;
    updateRSSI(d.rssi);
    const bb=document.getElementById('blynkBadge'), sb=document.getElementById('st_blynk');
    if(d.blynk){bb.className='ok';sb.textContent='Terhubung';sb.style.color='#a78bfa';}
    else{bb.className='err';sb.textContent='Terputus';sb.style.color='var(--muted)';}
    prevTemp=d.sensorError?prevTemp:d.temp;
    prevHum =d.sensorError?prevHum :d.hum;
  } catch(e) {
    consecutiveErrors++;
    if(consecutiveErrors>=3){setStatusBadge(false);addLog('err','Gagal mengambil data ('+consecutiveErrors+'x)');}
  }
}

// ==================== GAUGE UPDATE ====================
function updateGauges(d) {
  const tEl = document.getElementById('gaugeTempVal');
  const hEl = document.getElementById('gaugeHumVal');
  tEl.textContent = d.sensorError ? 'ERR' : d.temp.toFixed(1);
  hEl.textContent = d.sensorError ? 'ERR' : d.hum.toFixed(1);

  if (!d.sensorError) {
    drawGaugeTemp(d.temp); drawGaugeHum(d.hum);
    document.getElementById('tempMinVal').textContent = d.tempMin.toFixed(1);
    document.getElementById('tempMaxVal').textContent = d.tempMax.toFixed(1);
    document.getElementById('humMinVal').textContent  = d.humMin.toFixed(1);
    document.getElementById('humMaxVal').textContent  = d.humMax.toFixed(1);
  }

  // Trend — ditampilkan di luar gauge-wrap (trendTempRow / trendHumRow)
  function trendHtml(cur, prev) {
    if (prev===null||cur===null) return '';
    const diff=cur-prev;
    if(diff>0.2)  return `<span class="trend-badge trend-up">&#x2191; +${diff.toFixed(1)}</span>`;
    if(diff<-0.2) return `<span class="trend-badge trend-down">&#x2193; ${diff.toFixed(1)}</span>`;
    return `<span class="trend-badge trend-stable">&#x2194; stabil</span>`;
  }
  if (!d.sensorError) {
    document.getElementById('trendTempRow').innerHTML = trendHtml(d.temp, prevTemp);
    document.getElementById('trendHumRow').innerHTML  = trendHtml(d.hum,  prevHum);
  }

  const gst=document.getElementById('gaugeSubTemp'), gsh=document.getElementById('gaugeSubHum');
  if(d.sensorError){gst.textContent='Sensor error';gst.className='gauge-sub err';gsh.textContent='Sensor error';gsh.className='gauge-sub err';}
  else {
    if(d.temp>threshold.tempOn){gst.textContent='Terlalu panas';gst.className='gauge-sub warn';}
    else if(d.temp<threshold.tempOff-2){gst.textContent='Terlalu dingin';gst.className='gauge-sub warn';}
    else{gst.textContent='Suhu optimal';gst.className='gauge-sub ok';}
    if(d.hum<threshold.humOn){gsh.textContent='Terlalu kering';gsh.className='gauge-sub warn';}
    else if(d.hum>threshold.humOff){gsh.textContent='Terlalu lembap';gsh.className='gauge-sub warn';}
    else{gsh.textContent='Kelembapan optimal';gsh.className='gauge-sub ok';}
  }
}

// ==================== RSSI ====================
function updateRSSI(rssi) {
  document.getElementById('st_rssi').textContent=rssi+' dBm';
  const bars=[document.getElementById('r1'),document.getElementById('r2'),document.getElementById('r3'),document.getElementById('r4')];
  bars.forEach(b=>{b.className='';});
  let level=0;
  if(rssi>=-55)level=4; else if(rssi>=-65)level=3; else if(rssi>=-75)level=2; else if(rssi>=-85)level=1;
  const cls=rssi>=-65?'active':rssi>=-75?'warn':'bad';
  for(let i=0;i<level;i++) bars[i].className=cls;
}

// ==================== ALARM ====================
function updateAlarm(d) {
  const overlay=document.getElementById('alarmOverlay'), banner=document.getElementById('alarmBanner');
  let msgs=[];
  if(d.sensorError) msgs.push('&#x26A0; SENSOR ERROR!');
  else {
    if(d.temp>threshold.tempOn+2) msgs.push('&#x1F321; SUHU KRITIS: '+d.temp.toFixed(1)+'&#xB0;C');
    if(d.hum<threshold.humOn-10)  msgs.push('&#x1F4A7; KELEMBAPAN SANGAT RENDAH: '+d.hum.toFixed(1)+'%');
  }
  if(msgs.length>0){
    overlay.className='active'; banner.className='active'; banner.innerHTML=msgs.join(' &nbsp;|&nbsp; ');
    if(!alarmActive){addLog('err','[ALARM] '+msgs.join(' | '));alarmActive=true;}
  } else { overlay.className=''; banner.className=''; alarmActive=false; }
}

// ==================== STATUS ====================
function updateStatus(d) {
  setStatusBadge(!d.wifiError&&!d.sensorError);
  document.getElementById('st_sensor').textContent=d.sensorError?'ERROR':'OK';
  document.getElementById('st_sensor').style.color=d.sensorError?'var(--accent-err)':'var(--accent-ok)';
}

// ==================== CHARTS ====================
function updateCharts(d) {
  if(!d.tempHistory||d.tempHistory.length===0) return;
  const labels=d.timestamps.map((_,i)=>i%5===0?`${i*2}s`:'');
  chartTemp.data.labels=labels; chartTemp.data.datasets[0].data=d.tempHistory;
  chartHum.data.labels=labels;  chartHum.data.datasets[0].data=d.humHistory;
  chartTemp.update('none'); chartHum.update('none');
}

// ==================== CONTROLS ====================
function updateControls(d) {
  const bk=document.getElementById('badge_kipas');
  bk.textContent=d.kipas?'ON':'OFF'; bk.className='state-badge '+(d.kipas?'on':'off');
  document.getElementById('st_kipas').textContent=d.kipas?'Menyala':'Mati';
  document.getElementById('st_kipas').style.color=d.kipas?'var(--accent-ok)':'var(--muted)';
  document.getElementById('btn_kipas_auto').classList.toggle('active',!d.kipasOverride);
  const bh=document.getElementById('badge_hum');
  bh.textContent=d.humidifier?'ON':'OFF'; bh.className='state-badge '+(d.humidifier?'on':'off');
  document.getElementById('st_hum').textContent=d.humidifier?'Menyala':'Mati';
  document.getElementById('st_hum').style.color=d.humidifier?'var(--accent-ok)':'var(--muted)';
  document.getElementById('btn_hum_auto').classList.toggle('active',!d.humidOverride);
}

// ==================== RECO ====================
function updateReco(d) {
  const list=document.getElementById('recoList'); list.innerHTML='';
  const add=(icon,msg,cls)=>{const li=document.createElement('li');li.className=cls+'-item';li.innerHTML=`<span class="reco-icon">${icon}</span>${msg}`;list.appendChild(li);};
  if(d.sensorError){add('&#x274C;','Sensor DHT22 tidak terbaca. Periksa kabel dan koneksi pin GPIO 4.','err');return;}
  let ok=true;
  if(d.temp>threshold.tempOn){add('&#x1F321;','Suhu <b>'+d.temp.toFixed(1)+'&deg;C</b> terlalu tinggi. Pastikan kipas menyala.','warn');ok=false;}
  else if(d.temp<threshold.tempOff-2){add('&#x1F976;','Suhu terlalu rendah.','warn');ok=false;}
  else add('&#x2705;','Suhu dalam rentang ideal ('+threshold.tempOff+' - '+threshold.tempOn+'&deg;C).','ok');
  if(d.hum<threshold.humOn){add('&#x1F4A7;','Kelembapan <b>'+d.hum.toFixed(1)+'%</b> terlalu rendah.','warn');ok=false;}
  else if(d.hum>threshold.humOff){add('&#x1F30A;','Kelembapan terlalu tinggi.','warn');ok=false;}
  else add('&#x2705;','Kelembapan dalam rentang ideal ('+threshold.humOn+' - '+threshold.humOff+'%).','ok');
  if(d.kipasOverride) add('&#x1F527;','Kipas dalam mode MANUAL.','warn');
  if(d.humidOverride) add('&#x1F527;','Humidifier dalam mode MANUAL.','warn');
  if(ok&&!d.kipasOverride&&!d.humidOverride) add('&#x1F33F;','Semua parameter optimal. Kandang dalam kondisi baik!','ok');
}

function fillThresholdInputs(d) {
  const ids=['in_tempOn','in_tempOff','in_humOn','in_humOff'];
  const vals=[d.tempOn,d.tempOff,d.humOn,d.humOff];
  ids.forEach((id,i)=>{const el=document.getElementById(id);if(document.activeElement!==el)el.value=vals[i];});
}

function setStatusBadge(ok) {
  document.getElementById('statusBadge').className=ok?'ok':'err';
  document.getElementById('statusText').textContent=ok?'ONLINE':'ERROR';
}

// ==================== CONTROL ====================
async function control(device,state){
  try{
    await fetch('/api/control',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({device,state})});
    const sl=state==='auto'?'Mode AUTO':(state==='on'?'Dinyalakan':'Dimatikan');
    const dl=device==='kipas'?'Kipas':'Humidifier';
    showToast(dl+': '+sl,'ok'); addLog('info','[Kontrol] '+dl+' - '+sl.toUpperCase());
    setTimeout(fetchData,300);
  }catch(e){showToast('Gagal mengirim perintah','err');}
}

async function saveThreshold(){
  const body={
    tempOn:parseFloat(document.getElementById('in_tempOn').value),
    tempOff:parseFloat(document.getElementById('in_tempOff').value),
    humOn:parseFloat(document.getElementById('in_humOn').value),
    humOff:parseFloat(document.getElementById('in_humOff').value)
  };
  if(Object.values(body).some(isNaN)){showToast('Nilai tidak valid','err');return;}
  try{
    await fetch('/api/threshold',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
    showToast('Pengaturan tersimpan','ok');
    addLog('ok','[Threshold] Suhu '+body.tempOff+'-'+body.tempOn+'C | Humid '+body.humOn+'-'+body.humOff+'%');
  }catch(e){showToast('Gagal menyimpan','err');}
}

async function resetMinMax(){
  try{await fetch('/api/resetminmax',{method:'POST'});showToast('Min/Max direset','ok');addLog('info','[Reset] Min/Max direset');}
  catch(e){showToast('Gagal reset','err');}
}

function exportCSV(){
  if(allData.length===0){showToast('Belum ada data','err');return;}
  let csv='Timestamp,Suhu (C),Kelembapan (%)\n';
  allData.forEach(r=>{csv+=r.ts+','+r.temp.toFixed(2)+','+r.hum.toFixed(2)+'\n';});
  const blob=new Blob([csv],{type:'text/csv'});
  const url=URL.createObjectURL(blob);
  const a=document.createElement('a');
  a.href=url; a.download='owltech_data_'+new Date().toISOString().slice(0,19).replace(/:/g,'-')+'.csv';
  a.click(); URL.revokeObjectURL(url);
  showToast('CSV didownload ('+allData.length+' baris)','ok');
  addLog('ok','[Export] '+allData.length+' baris data diexport ke CSV');
}

const logs=[];
function addLog(type,msg){const ts=new Date().toTimeString().slice(0,8);logs.push({type,msg,ts});if(logs.length>80)logs.shift();renderLog();}
function renderLog(){document.getElementById('logBox').innerHTML=logs.slice().reverse().map(l=>`<div class="log-entry log-${l.type}">[${l.ts}] ${l.msg}</div>`).join('');}
function clearLog(){logs.length=0;renderLog();}

let toastTimer;
function showToast(msg,type){const t=document.getElementById('toast');t.textContent=msg;t.className='show '+(type||'ok');clearTimeout(toastTimer);toastTimer=setTimeout(()=>t.className='',2800);}

setInterval(()=>{if(lastRefresh){const s=Math.round((Date.now()-lastRefresh)/1000);document.getElementById('chipRefresh').textContent=s;}},1000);
fetchData();
setInterval(fetchData,3000);
addLog('info','Dashboard OWL Tech dimuat.');
</script>
</body>
</html>)rawhtml";
  server.send(200, "text/html", html);
}

// ==================== SETUP ====================
void setup()
{
  Serial.begin(115200);
  Wire.begin(21, 22);
  uptimeStart = millis();

  pinMode(BUZZER, OUTPUT);
  pinMode(BUTTON, INPUT_PULLUP);
  pinMode(RELAY_KIPAS, OUTPUT);
  pinMode(RELAY_HUMIDIFIER, OUTPUT);
  digitalWrite(BUZZER, LOW);
  digitalWrite(RELAY_KIPAS, OFF);
  digitalWrite(RELAY_HUMIDIFIER, OFF);

  dht.begin();
  loadThresholds(); // pulihkan ambang batas tersimpan (penting saat alat dipindah ruang)
  memset(tempHistory, 0, sizeof(tempHistory));
  memset(humHistory, 0, sizeof(humHistory));
  memset(timeHistory, 0, sizeof(timeHistory));

  if (!lcd.begin(SSD1306_SWITCHCAPVCC, 0x3C))
  {
    Serial.println(F("LCD TIDAK DITEMUKAN"));
    for (int i = 0; i < 6; i++)
    {
      digitalWrite(BUZZER, HIGH);
      delay(200);
      digitalWrite(BUZZER, LOW);
      delay(200);
    }
    while (true)
      ;
  }

  lcd.clearDisplay();
  lcd.setTextColor(WHITE);
  lcd.setTextSize(2);
  lcd.setCursor(0, 0);
  lcd.println("Monitoring");
  lcd.setTextSize(1);
  lcd.println();
  lcd.println("Suhu & kelembapan");
  lcd.println();
  lcd.println("By : OWL Tech");
  lcd.display();

  lcd.clearDisplay();
  lcd.setTextSize(1);
  lcd.setCursor(0, 20);
  lcd.println("Mencari WiFi...");
  lcd.display();

  wm.setAPCallback(configModeCallback);
  wm.setConfigPortalTimeout(180); // portal terbuka 3 menit, lalu lanjut boot (retry di background)
  // Nama AP "Smartoring-Setup", password AP "smartoring123" (min 8 karakter)
  bool connectedWifi = wm.autoConnect("Smartoring-Setup", "smartoring123");

  if (!connectedWifi)
  {
    wifiError = true;
    Serial.println("\nWiFi GAGAL / Portal timeout!");
    lcd.clearDisplay();
    lcd.setTextSize(1);
    lcd.setCursor(0, 20);
    lcd.println("WiFi ERROR!");
    lcd.println("Tahan tombol 3 detik");
    lcd.println("untuk setup ulang.");
    lcd.display();
  }
  else
  {
    wifiError = false;
    Serial.println();
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    Blynk.config(BLYNK_AUTH_TOKEN, "sgp1.blynk.cloud", 80);
    if (Blynk.connect(10000))
      Serial.println("[Blynk] Connected!");
    else
      Serial.println("[Blynk] Gagal koneksi awal - akan retry di loop.");

    blynkTimer.setInterval(2000L, sendToBlynk);

    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/data", HTTP_GET, handleApiData);
    server.on("/api/control", HTTP_POST, handleApiControl);
    server.on("/api/threshold", HTTP_POST, handleApiThreshold);
    server.on("/api/resetminmax", HTTP_POST, handleApiResetMinMax);
    server.begin();
    Serial.println("WebServer Started");

    if (MDNS.begin("smartoring"))
    {
      MDNS.addService("http", "tcp", 80);
      Serial.println("[mDNS] Akses dashboard via: http://smartoring.local");
    }
    else
    {
      Serial.println("[mDNS] Gagal memulai mDNS, gunakan IP langsung.");
    }
  }
}

// ==================== LOOP ====================
void loop()
{
  server.handleClient();

  if (!wifiError)
  {
    Blynk.run();
    blynkTimer.run();
    if (millis() - lastBlynkCheck >= blynkCheckTime)
    {
      lastBlynkCheck = millis();
      if (!Blynk.connected())
      {
        Serial.println("[Blynk] Reconnecting...");
        Blynk.connect(3000);
      }
    }
  }

  if (millis() - lastWifiCheck >= wifiCheckTime)
  {
    wifiError = (WiFi.status() != WL_CONNECTED);
    if (wifiError)
      Serial.println("[WiFi] TERPUTUS!");
    lastWifiCheck = millis();
  }

  if (millis() - lastButton >= buttonTime)
  {
    if (digitalRead(BUTTON) == LOW)
    {
      showIP = true;
      showIPStart = millis();
    }
    lastButton = millis();
  }

  // ============ TAHAN TOMBOL 3 DETIK = RESET WIFI ============
  // Berguna karena alat ini dipindah-pindah antar ruang/rumah:
  // tidak perlu buka kode & re-upload untuk ganti jaringan WiFi.
  {
    static unsigned long pressStart = 0;
    static bool longPressDone = false;
    if (digitalRead(BUTTON) == LOW)
    {
      if (pressStart == 0)
        pressStart = millis();
      if (!longPressDone && millis() - pressStart >= 3000)
      {
        longPressDone = true;
        Serial.println("[WiFi] Reset kredensial WiFi diminta (tombol ditahan 3s).");
        lcd.clearDisplay();
        lcd.setTextSize(1);
        lcd.setCursor(0, 20);
        lcd.println("Reset WiFi...");
        lcd.println("Restart ke mode");
        lcd.println("konfigurasi...");
        lcd.display();
        delay(1200);
        wm.resetSettings();
        delay(300);
        ESP.restart();
      }
    }
    else
    {
      pressStart = 0;
      longPressDone = false;
    }
  }

  if (millis() - lastDHT >= dhtInterval)
  {
    float newH = dht.readHumidity(), newT = dht.readTemperature();
    if (isnan(newH) || isnan(newT))
    {
      sensorError = true;
      Serial.println("[DHT] ERROR");
    }
    else
    {
      sensorError = false;
      h = newH;
      t = newT;
      Serial.printf("[DHT] Suhu: %.1f C | Lembap: %.1f %%\n", t, h);
      if (t < tempMin)
        tempMin = t;
      if (t > tempMax)
        tempMax = t;
      if (h < humMin)
        humMin = h;
      if (h > humMax)
        humMax = h;
      tempHistory[historyIndex] = t;
      humHistory[historyIndex] = h;
      timeHistory[historyIndex] = millis() / 1000;
      historyIndex = (historyIndex + 1) % HISTORY_SIZE;
      if (historyCount < HISTORY_SIZE)
        historyCount++;

      if (!humidifierOverride)
      {
        bool cur = (digitalRead(RELAY_HUMIDIFIER) == LOW);
        if (h < humOn)
        {
          if (!cur)
            humidifierOnCount++;
          digitalWrite(RELAY_HUMIDIFIER, ON);
        }
        else if (h > humOff)
          digitalWrite(RELAY_HUMIDIFIER, OFF);
      }
      if (!kipasOverride)
      {
        bool cur = (digitalRead(RELAY_KIPAS) == LOW);
        if (t > tempOn)
        {
          if (!cur)
            kipasOnCount++;
          digitalWrite(RELAY_KIPAS, ON);
        }
        else if (t < tempOff)
          digitalWrite(RELAY_KIPAS, OFF);
      }
    }
    lastDHT = millis();
  }

  // Buzzer aktif bukan cuma saat error sensor/WiFi, tapi juga saat suhu/
  // kelembapan sudah di zona kritis -- sinkron dengan ambang yang dipakai
  // untuk notifikasi Blynk (temp_alert / hum_alert) di sendToBlynk().
  bool envCritical = !sensorError && (t > tempOn + 2.0 || h < humOn - 10.0);
  if (anyError() || envCritical)
    ALARM();
  else
    ALARM_OFF();

  if (millis() - lastLCD >= LCDTime)
  {
    lcd.clearDisplay();
    if (showIP)
    {
      lcd.setTextSize(1);
      lcd.setCursor(0, 0);
      lcd.println("== IP Address ==");
      lcd.println();
      lcd.println(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "WiFi tidak\nterhubung!");
      if (millis() - showIPStart >= showIPDuration)
        showIP = false;
    }
    else if (sensorError)
    {
      lcd.setTextSize(1);
      lcd.setCursor(10, 20);
      lcd.println("!! Sensor ERROR !!");
      lcd.setCursor(10, 35);
      lcd.println("Periksa DHT22");
    }
    else if (wifiError)
    {
      lcd.setTextSize(1);
      lcd.setCursor(10, 20);
      lcd.println("!! WiFi ERROR !!");
      lcd.setCursor(10, 35);
      lcd.println("Periksa jaringan");
    }
    else
    {
      lcd.setTextSize(2);
      lcd.setCursor(0, 0);
      lcd.println("Ruangan");
      lcd.setTextSize(1);
      lcd.setCursor(0, 25);
      lcd.print("Suhu : ");
      lcd.print(t, 1);
      lcd.print(" C");
      lcd.setCursor(0, 40);
      lcd.print("Lembap: ");
      lcd.print(h, 1);
      lcd.print(" %");
      lcd.setCursor(0, 54);
      lcd.print(digitalRead(RELAY_KIPAS) == LOW ? "FAN:ON " : "FAN:OFF");
      lcd.print(digitalRead(RELAY_HUMIDIFIER) == LOW ? " HUM:ON" : " HUM:OFF");
    }
    lcd.display();
    lastLCD = millis();
  }
}
