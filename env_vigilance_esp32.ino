/*
  env_vigilance_esp32.ino
  Environment Vigilance & Nature Data Monitor (ESP32 DevKit)
  - Sensors: DHT22, BMP280, PMS5003 (UART), MQ135 (A0), BH1750 (I2C optional)
  - Web UI with simple password roles (admin/guest)
  - Posts JSON sensor data to cloud endpoint for satellite fusion and clinical analysis
  - Local advisory engine provides non-medical guidance (NOT medical prescriptions)
  - Arduino IDE compatible (no command-line required)

  NOTE: This device only gives general recommendations. Do NOT use it as a medical device.
  Author: Generated for user request
  Date: 2025
*/

// ----------- Include libraries -------------
#include <WiFi.h>
#include <WebServer.h>          // lightweight server for ESP32
#include <AsyncTCP.h>          // not used but keep minimal
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP280.h>
#include "DHT.h"
#include <BH1750.h>
#include <HardwareSerial.h>
#include <ArduinoJson.h>

// ----------- WiFi & Cloud config -----------
const char* WIFI_SSID = "YourWiFiSSID";
const char* WIFI_PASS = "YourWiFiPassword";

// Cloud endpoint to POST JSON (server that merges satellite & clinical logic)
const char* CLOUD_ENDPOINT = "http://example.com/api/ingest"; // replace with your server
const int CLOUD_PORT = 80;
const char* CLOUD_PATH = "/api/ingest";

// Web UI credentials
String ADMIN_PW = "admin123";
String GUEST_PW = "guest123";

WebServer server(80);

// ----------- Pins & sensors ----------------
#define DHTPIN 4            // GPIO4 (D2 on some boards)
#define DHTTYPE DHT22

#define MQ135_PIN 34        // ADC1_6 (GPIO34) - analog pin for MQ-135 (requires voltage divider)
#define PMS_RX 16           // UART RX (to PMS TX)
#define PMS_TX 17           // UART TX (to PMS RX) -- only for some modules if using bidir
#define BH1750_SDA 21
#define BH1750_SCL 22

#define BMP_CS_UNUSED -1    // using I2C

// sensor objects
DHT dht(DHTPIN, DHTTYPE);
Adafruit_BMP280 bmp;       // I2C
BH1750 lightMeter;
HardwareSerial pmSerial(2); // UART2 for PMS sensor (RX2=16, TX2=17)

// ----------- Constants & thresholds -----------
const unsigned long SAMPLE_INTERVAL_MS = 7000; // sample every 7s
const size_t HISTORY_SLOTS = 120; // store last N samples (approx 14 minutes if 7s interval)

const float PM25_GOOD = 12.0;
const float PM25_MODERATE = 35.4;
const float PM25_UNHEALTHY_SENSITIVE = 55.4; // for sensitive groups

const float VOC_SAFE = 100.0;   // arbitrary MQ-135 scaled units
const float VOC_WARN = 250.0;

const float TEMP_HIGH = 30.0;   // C
const float TEMP_LOW = 16.0;

// ---------- Data structures ---------------
struct Sample {
  unsigned long ts;
  float temp_dht;
  float hum;
  float temp_bmp;
  float pressure;
  int pm1_0;
  int pm2_5;
  int pm10;
  float voc; // scaled MQ-135
  float lux;
  int riskScore; // 0-100
  String advice;
};

Sample history[HISTORY_SLOTS];
size_t historyIndex = 0;
bool historyWrapped = false;

// ---------- Utilities ----------
String isoNow() {
  time_t t = time(nullptr);
  // If no NTP configured, fallback to millis as seconds since boot
  if (t == 0) {
    unsigned long s = millis() / 1000;
    return String(s) + "s";
  }
  char buf[32];
  struct tm *tminfo = gmtime(&t);
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", tminfo);
  return String(buf);
}

unsigned long lastSample = 0;

// ---------- PMS5003 parsing (basic) -------------
bool readPMS(int &pm1, int &pm2_5, int &pm10) {
  // PMS5003 frame is 32 bytes starting with 0x42 0x4d
  // We'll attempt to read available bytes and parse frame
  const int FRAME_LEN = 32;
  uint8_t buf[FRAME_LEN];
  int readCount = 0;
  unsigned long start = millis();
  while (millis() - start < 120) { // timeout 120ms
    if (pmSerial.available()) {
      if (readCount < FRAME_LEN) {
        buf[readCount++] = pmSerial.read();
      } else {
        // shift buffer
        for (int i = 0; i < FRAME_LEN - 1; ++i) buf[i] = buf[i + 1];
        buf[FRAME_LEN - 1] = pmSerial.read();
      }
      // try to find header at pos 0
      if (readCount >= FRAME_LEN) {
        for (int s = 0; s <= readCount - FRAME_LEN; ++s) {
          if (buf[s] == 0x42 && buf[s+1] == 0x4d) {
            // compute index offset
            uint8_t *p = buf + s;
            uint16_t frame_len = (p[2] << 8) | p[3];
            if (frame_len == 28) {
              // PM1.0 at bytes 10-11, PM2.5 at 12-13, PM10 at 14-15 (ambient)
              int pm1_v = (p[10] << 8) | p[11];
              int pm2_v = (p[12] << 8) | p[13];
              int pm10_v = (p[14] << 8) | p[15];
              pm1 = pm1_v;
              pm2_5 = pm2_v;
              pm10 = pm10_v;
              return true;
            }
          }
        }
      }
    }
  }
  return false;
}

// ---------- MQ-135 reading (scaled) ----------
float readMQ135Scaled() {
  // Raw ADC read (0-4095). Convert to voltage and approximate VOC score.
  int raw = analogRead(MQ135_PIN);
  // Note: ADC on ESP32 default is 12-bit (0-4095). Ensure attenuation set if needed.
  float v = (raw / 4095.0) * 3.3; // voltage across divider
  // Convert rough scale - this is heuristic: higher voltage = more VOC here (depends on circuit)
  // We map voltage 0.0-3.3 to 0-500 scale (arbitrary units)
  float score = (v / 3.3) * 500.0;
  return score;
}

// ---------- Simple advisory engine (NON-MEDICAL) ----------
String generateAdvice(const Sample &s) {
  // Combine PM2.5 and VOC and thermal comfort into simple rules
  int pmRisk = 0;
  if (s.pm2_5 <= PM25_GOOD) pmRisk = 0;
  else if (s.pm2_5 <= PM25_MODERATE) pmRisk = 30;
  else if (s.pm2_5 <= PM25_UNHEALTHY_SENSITIVE) pmRisk = 60;
  else pmRisk = 85;

  int vocRisk = 0;
  if (s.voc <= VOC_SAFE) vocRisk = 0;
  else if (s.voc <= VOC_WARN) vocRisk = 35;
  else vocRisk = 70;

  int thermal = 0;
  if (s.temp_dht < TEMP_LOW || s.temp_dht > TEMP_HIGH) thermal = 15;

  int combined = pmRisk * 6/10 + vocRisk * 3/10 + thermal;
  if (combined > 100) combined = 100;

  // non-medical advice text
  String adv = "";
  if (s.pm2_5 > PM25_UNHEALTHY_SENSITIVE) {
    adv += "High PM2.5 detected — reduce outdoor exposure, use air purifier if available. ";
  } else if (s.pm2_5 > PM25_MODERATE) {
    adv += "Moderate PM2.5 — avoid prolonged outdoor activity. ";
  } else {
    adv += "PM2.5 levels are acceptable. ";
  }

  if (s.voc > VOC_WARN) {
    adv += "Poor indoor air (VOC) — increase ventilation, avoid chemical aerosols. ";
  } else if (s.voc > VOC_SAFE) {
    adv += "Slight VOC presence — ventilate indoor spaces periodically. ";
  } else {
    adv += "Indoor air quality looks normal. ";
  }

  if (thermal > 0) {
    adv += "Temperature outside comfortable range — adjust heating/cooling. ";
  } else {
    adv += "Temperature & humidity are comfortable. ";
  }

  adv += "If you're pregnant and concerned, please contact your healthcare provider for personalized advice.";
  return adv;
}

// ---------- Compute risk score ----------
int computeRiskScore(const Sample &s) {
  // reuse simple math from advice: map pm, voc, thermal to 0-100
  int pmScore = 0;
  if (s.pm2_5 <= PM25_GOOD) pmScore = 0;
  else if (s.pm2_5 <= PM25_MODERATE) pmScore = 30;
  else if (s.pm2_5 <= PM25_UNHEALTHY_SENSITIVE) pmScore = 60;
  else pmScore = 90;

  int vocScore = 0;
  if (s.voc <= VOC_SAFE) vocScore = 0;
  else if (s.voc <= VOC_WARN) vocScore = 30;
  else vocScore = 60;

  int thermal = 0;
  if (s.temp_dht < TEMP_LOW || s.temp_dht > TEMP_HIGH) thermal = 15;

  int risk = (pmScore * 6 + vocScore * 3 + thermal * 1) / 10;
  if (risk > 100) risk = 100;
  return risk;
}

// ---------- Save to history ----------
void pushHistory(const Sample &s) {
  history[historyIndex] = s;
  historyIndex++;
  if (historyIndex >= HISTORY_SLOTS) {
    historyIndex = 0;
    historyWrapped = true;
  }
}

// ---------- Post JSON to cloud ----------
void postToCloud(const Sample &s) {
  // Build JSON payload
  StaticJsonDocument<512> doc;
  doc["ts"] = s.ts;
  doc["iso"] = isoNow();
  doc["temp_dht"] = s.temp_dht;
  doc["hum"] = s.hum;
  doc["temp_bmp"] = s.temp_bmp;
  doc["pressure"] = s.pressure;
  doc["pm1_0"] = s.pm1_0;
  doc["pm2_5"] = s.pm2_5;
  doc["pm10"] = s.pm10;
  doc["voc"] = s.voc;
  doc["lux"] = s.lux;
  doc["riskScore"] = s.riskScore;
  doc["advice"] = s.advice;

  String payload;
  serializeJson(doc, payload);

  WiFiClient client;
  if (!client.connect(CLOUD_ENDPOINT, CLOUD_PORT)) {
    // If CLOUD_ENDPOINT is a hostname:port you'd parse differently. Here we assume example.com and port provided.
    // For HTTP URL strings you'd need to parse hostname and path; keep this minimal.
    Serial.println("Failed to connect to cloud endpoint");
    return;
  }

  // send HTTP POST
  client.print(String("POST ") + CLOUD_PATH + " HTTP/1.1\r\n" +
               "Host: " + CLOUD_ENDPOINT + "\r\n" +
               "Content-Type: application/json\r\n" +
               "Content-Length: " + payload.length() + "\r\n" +
               "Connection: close\r\n\r\n" +
               payload);

  // read response (non-blocking small wait)
  unsigned long start = millis();
  while (client.connected() && millis() - start < 2000) {
    while (client.available()) {
      String line = client.readStringUntil('\n');
      // optionally parse response
    }
  }
  client.stop();
}

// ---------- Web Server handlers ----------
bool checkAuth(String role) {
  if (!server.hasArg("pw")) return false;
  String pw = server.arg("pw");
  if (role == "admin") return (pw == ADMIN_PW);
  else return (pw == GUEST_PW) || (pw == ADMIN_PW);
}

String pageHeader(const String &title) {
  String h = "<!doctype html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  h += "<title>" + title + "</title>";
  h += "<style>body{font-family:Arial;padding:8px;} .card{border-radius:8px;padding:10px;margin:8px 0;border:1px solid #ddd;} button{padding:8px 10px;margin:6px;} .on{color:green;} .off{color:red;} .small{font-size:12px;color:#444;}</style>";
  h += "</head><body><h2>" + title + "</h2>";
  return h;
}

String pageFooter() {
  return "<hr><div class='small'>Environment Vigilance Device - Non-medical advisories only.</div></body></html>";
}

void handleRoot() {
  String html = pageHeader("Environment Vigilance - Login");
  html += "<div class='card'><form action='/dashboard' method='GET'>";
  html += "<label>Password (admin or guest):</label><br>";
  html += "<input name='pw' type='password' style='padding:6px;width:70%;'><br>";
  html += "<button type='submit'>Enter</button></form></div>";
  html += pageFooter();
  server.send(200, "text/html", html);
}

void handleDashboard() {
  if (!server.hasArg("pw")) {
    server.sendHeader("Location", "/");
    server.send(302);
    return;
  }
  String pw = server.arg("pw");
  bool isAdmin = (pw == ADMIN_PW);
  bool isGuest = (pw == GUEST_PW) || isAdmin;
  if (!isGuest) {
    server.send(403, "text/plain", "Forbidden - invalid password");
    return;
  }

  // latest sample is last written slot (-1)
  size_t idx = (historyIndex == 0 && !historyWrapped) ? 0 : (historyIndex + HISTORY_SLOTS - 1) % HISTORY_SLOTS;
  Sample latest = history[idx];

  String html = pageHeader("Environment Dashboard");
  html += "<div class='card'><h3>Latest Readings</h3>";
  html += "<div>Time: " + String(latest.ts) + " (" + isoNow() + ")</div>";
  html += "<div>Temperature (DHT): " + String(latest.temp_dht,1) + " °C</div>";
  html += "<div>Humidity: " + String(latest.hum,1) + " %</div>";
  html += "<div>Pressure (BMP): " + String(latest.pressure,1) + " hPa</div>";
  html += "<div>PM2.5: " + String(latest.pm2_5) + " µg/m³</div>";
  html += "<div>VOC (MQ-135 scaled): " + String(latest.voc,1) + "</div>";
  html += "<div>Ambient Light (lux): " + String(latest.lux,1) + "</div>";
  html += "<div>Risk Score: " + String(latest.riskScore) + " / 100</div>";
  html += "<div style='margin-top:8px'><strong>Advice:</strong><br>" + latest.advice + "</div></div>";

  // small history view
  html += "<div class='card'><h3>Recent History (last " + String(HISTORY_SLOTS) + " samples)</h3>";
  html += "<div class='small'><table border='0' cellpadding='4'><tr><th>Time</th><th>PM2.5</th><th>VOC</th><th>Risk</th></tr>";
  // show up to last 12 entries
  int shown = 0;
  int pos = (historyIndex + HISTORY_SLOTS - 1) % HISTORY_SLOTS;
  for (int i = 0; i < min((int)HISTORY_SLOTS, 12); ++i) {
    Sample s = history[(pos + HISTORY_SLOTS - i) % HISTORY_SLOTS];
    if (s.ts == 0) continue;
    html += "<tr><td>" + String(s.ts) + "</td><td>" + String(s.pm2_5) + "</td><td>" + String(s.voc,0) + "</td><td>" + String(s.riskScore) + "</td></tr>";
    shown++;
  }
  if (shown == 0) html += "<tr><td colspan='4'>No samples yet</td></tr>";
  html += "</table></div></div>";

  // admin controls
  if (isAdmin) {
    html += "<div class='card'><h3>Admin</h3>";
    html += "<form action='/setpw' method='POST'><input type='hidden' name='pw' value='" + pw + "'>";
    html += "<label>New Admin Password:</label><br><input type='password' name='newadmin' style='padding:6px;width:70%;'><br>";
    html += "<label>New Guest Password:</label><br><input type='password' name='newguest' style='padding:6px;width:70%;'><br>";
    html += "<button type='submit'>Save</button></form></div>";
  }

  html += pageFooter();
  server.send(200, "text/html", html);
}

void handleSetPw() {
  if (!server.hasArg("pw")) { server.send(403, "text/plain", "Forbidden"); return; }
  if (!checkAuth("admin")) { server.send(403, "text/plain", "Forbidden"); return; }
  if (server.hasArg("newadmin")) {
    String n = server.arg("newadmin");
    if (n.length() >= 4) ADMIN_PW = n;
  }
  if (server.hasArg("newguest")) {
    String ng = server.arg("newguest");
    if (ng.length() >= 3) GUEST_PW = ng;
  }
  server.sendHeader("Location", "/dashboard?pw=" + server.arg("pw"));
  server.send(302);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ---------- Setup & loop ----------
void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println("Environment Vigilance Device starting...");

  // init sensors
  dht.begin();
  Wire.begin();

  if (!bmp.begin()) {
    Serial.println("BMP280 init failed");
  } else {
    Serial.println("BMP280 OK");
  }

  if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println("BH1750 not found or init failed");
  } else {
    Serial.println("BH1750 OK");
  }

  // UART for PMS
  pmSerial.begin(9600, SERIAL_8N1, PMS_RX, PMS_TX);
  Serial.println("PMS UART started");

  // ADC pin attenuation (if needed)
  analogReadResolution(12); // 0-4095
  // optionally set attenuation for MQ sensor if voltage > 1V
  // (use analogSetPinAttenuation if needed on specific board)

  // init history
  for (size_t i = 0; i < HISTORY_SLOTS; ++i) history[i].ts = 0;

  // WiFi connect
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Connecting to WiFi '%s' ...\n", WIFI_SSID);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected! IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connect failed; starting AP mode.");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("EnvVigilanceAP");
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  }

  // web routes
  server.on("/", handleRoot);
  server.on("/dashboard", handleDashboard);
  server.on("/setpw", HTTP_POST, handleSetPw);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Web server started on port 80");

  lastSample = 0;
}

void doSampleAndProcess() {
  Sample s;
  s.ts = millis();
  // DHT
  float hum = dht.readHumidity();
  float temp = dht.readTemperature();
  if (isnan(hum) || isnan(temp)) {
    Serial.println("DHT read failed");
    // fallback to previous values if available
    hum = history[(historyIndex+HISTORY_SLOTS-1)%HISTORY_SLOTS].hum;
    temp = history[(historyIndex+HISTORY_SLOTS-1)%HISTORY_SLOTS].temp_dht;
  }
  s.hum = hum;
  s.temp_dht = temp;

  // BMP
  if (bmp.begin()) {
    s.temp_bmp = bmp.readTemperature();
    s.pressure = bmp.readPressure() / 100.0F;
  } else {
    s.temp_bmp = s.temp_dht;
    s.pressure = 0;
  }

  // PMS
  int p1=0,p2=0,p10=0;
  if (readPMS(p1,p2,p10)) {
    s.pm1_0 = p1;
    s.pm2_5 = p2;
    s.pm10 = p10;
  } else {
    // fallback to previous sample
    Sample prev = history[(historyIndex+HISTORY_SLOTS-1)%HISTORY_SLOTS];
    s.pm1_0 = prev.pm1_0;
    s.pm2_5 = prev.pm2_5;
    s.pm10 = prev.pm10;
  }

  // MQ-135
  s.voc = readMQ135Scaled();

  // Lux
  float lux = 0;
  if (lightMeter.begin()) {
    lux = lightMeter.readLightLevel();
  }
  s.lux = lux;

  // compute risk and advice
  s.riskScore = computeRiskScore(s);
  s.advice = generateAdvice(s);

  // push to history
  pushHistory(s);

  // log
  Serial.printf("Sample ts=%lu T=%.1fC H=%.1f%% PM2.5=%d VOC=%.0f Risk=%d\n", s.ts, s.temp_dht, s.hum, s.pm2_5, s.voc, s.riskScore);

  // send to cloud (non-blocking-ish)
  if (WiFi.status() == WL_CONNECTED) {
    postToCloud(s);
  }
}

void loop() {
  server.handleClient();

  if (millis() - lastSample >= SAMPLE_INTERVAL_MS) {
    lastSample = millis();
    doSampleAndProcess();
  }

  // simple delay to allow other processes
  delay(10);
}
