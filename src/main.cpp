
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ──────────────────────────────────────────────
// Hardware
// ──────────────────────────────────────────────
const int ALERT_LED_PIN = 2;          // Built-in LED on most ESP32 devkits (active HIGH)

// ──────────────────────────────────────────────
// Network / AP
// ──────────────────────────────────────────────
const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 1);
DNSServer dnsServer;
WebServer server(80);
Preferences preferences;

// ──────────────────────────────────────────────
// Alert API
// ──────────────────────────────────────────────
const char* ALERTS_URL  = "https://www.oref.org.il/WarningMessages/alert/alerts.json";
const char* REFERER     = "https://www.oref.org.il/";
const unsigned long POLL_INTERVAL_MS  = 5000;   // poll every 5 s
const unsigned long BLINK_INTERVAL_MS = 400;    // LED blink speed during alert

// ──────────────────────────────────────────────
// Runtime state
// ──────────────────────────────────────────────
String wifi_ssid       = "";
String wifi_pass       = "";
String target_location = "";   // Hebrew area name, e.g. "רמת גן"

bool  isAlertActive  = false;
bool  ledState       = false;
unsigned long lastPollTime  = 0;
unsigned long lastBlinkTime = 0;

// ──────────────────────────────────────────────
// Forward declarations
// ──────────────────────────────────────────────
void startCaptivePortal();
void handleRootLayout();
void handleSaveConfig();
void connectToWiFi();
void pollAlerts();
void updateLed();
void checkAlertObject(JsonObject alert, bool& matched, bool& ended);

// ══════════════════════════════════════════════
// setup
// ══════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  // LED
  pinMode(ALERT_LED_PIN, OUTPUT);
  digitalWrite(ALERT_LED_PIN, LOW);

  // NVS
  preferences.begin("alarm-config", false);
  wifi_ssid       = preferences.getString("ssid",     "");
  wifi_pass       = preferences.getString("pass",     "");
  target_location = preferences.getString("location", "");

  Serial.print("Saved location: ");
  Serial.println(target_location);

  if (wifi_ssid == "") {
    startCaptivePortal();
  } else {
    connectToWiFi();
  }
}

// ══════════════════════════════════════════════
// loop
// ══════════════════════════════════════════════
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    // AP / captive-portal mode
    dnsServer.processNextRequest();
    server.handleClient();
  } else {
    // Station mode – poll the alert API
    unsigned long now = millis();
    if (now - lastPollTime >= POLL_INTERVAL_MS) {
      lastPollTime = now;
      pollAlerts();
    }
    updateLed();
  }
}

// ══════════════════════════════════════════════
// checkAlertObject
// Inspect one alert object: { id, cat, title, data: ["רמת גן", …] }
// cat 13 = event ended (all-clear for listed areas).
// ══════════════════════════════════════════════
void checkAlertObject(JsonObject alert, bool& matched, bool& ended) {
  JsonArray data = alert["data"].as<JsonArray>();
  if (data.isNull()) return;

  String cat = alert["cat"].as<String>();
  bool isEnded = (cat == "13");

  for (JsonVariant area : data) {
    if (area.as<String>() == target_location) {
      if (isEnded) {
        ended = true;
      } else {
        matched = true;
      }
      return;
    }
  }
}

// ══════════════════════════════════════════════
// pollAlerts
// Query oref.org.il and check whether target_location
// is listed in the current active alerts.
// ══════════════════════════════════════════════
void pollAlerts() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (target_location.length() == 0) return;

  WiFiClientSecure client;
  client.setInsecure();   // skip cert verification (oref uses a valid cert, but avoids needing root CA)

  HTTPClient http;
  http.begin(client, ALERTS_URL);
  http.addHeader("Referer",           REFERER);
  http.addHeader("X-Requested-With",  "XMLHttpRequest");
  http.addHeader("Accept",            "application/json");
  http.setTimeout(4000);

  int code = http.GET();
  Serial.print("[alerts] HTTP ");
  Serial.println(code);

  if (code == HTTP_CODE_OK) {
    String payload = http.getString();
    payload.trim();
    Serial.print("[alerts] payload: ");
    Serial.println(payload);

    // When there are no alerts, oref returns an empty body or "{}"
    if (payload.length() == 0 || payload == "{}") {
      isAlertActive = false;
      Serial.println("[alerts] No active alerts.");
    } else {
      StaticJsonDocument<8192> doc;
      DeserializationError err = deserializeJson(doc, payload);

      if (err) {
        Serial.print("[alerts] JSON parse error: ");
        Serial.println(err.c_str());
        isAlertActive = false;
      } else {
        bool matched = false;
        bool ended   = false;

        if (doc.is<JsonArray>()) {
          for (JsonObject alert : doc.as<JsonArray>()) {
            checkAlertObject(alert, matched, ended);
          }
        } else if (doc.is<JsonObject>()) {
          checkAlertObject(doc.as<JsonObject>(), matched, ended);
        }

        isAlertActive = matched && !ended;
        Serial.print("[alerts] Match for '");
        Serial.print(target_location);
        Serial.print("': ");
        if (ended && !matched) {
          Serial.println("ended");
        } else {
          Serial.println(isAlertActive ? "YES – ALERT!" : "no");
        }
      }
    }
  } else {
    Serial.println("[alerts] Request failed; keeping previous state.");
  }

  http.end();
}

// ══════════════════════════════════════════════
// updateLed
// Blink the LED when an alert is active, turn it
// off when everything is calm.
// ══════════════════════════════════════════════
void updateLed() {
  if (isAlertActive) {
    unsigned long now = millis();
    if (now - lastBlinkTime >= BLINK_INTERVAL_MS) {
      lastBlinkTime = now;
      ledState = !ledState;
      digitalWrite(ALERT_LED_PIN, ledState ? HIGH : LOW);
    }
  } else {
    if (ledState) {
      ledState = false;
      digitalWrite(ALERT_LED_PIN, LOW);
    }
  }
}

// ══════════════════════════════════════════════
// startCaptivePortal
// ══════════════════════════════════════════════
void startCaptivePortal() {
  Serial.println("Starting Access Point configuration portal…");
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP("Homefront-Alert-Lamp");

  dnsServer.start(DNS_PORT, "*", apIP);

  server.on("/",     handleRootLayout);
  server.on("/save", handleSaveConfig);
  server.onNotFound(handleRootLayout);
  server.begin();
  Serial.println("Captive portal running at 192.168.4.1");
}

// ══════════════════════════════════════════════
// handleRootLayout – captive portal HTML
// ══════════════════════════════════════════════
void handleRootLayout() {
  String html =
    "<!DOCTYPE html><html lang='he'><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>Alert Lamp Setup</title>"
    "<style>"
    "  body{font-family:sans-serif;padding:24px;direction:rtl;}"
    "  label{display:block;margin-top:12px;font-weight:bold;}"
    "  input{width:100%;padding:10px;margin-top:4px;box-sizing:border-box;font-size:16px;}"
    "  small{color:#555;}"
    "  button{margin-top:16px;width:100%;padding:12px;font-size:16px;"
    "         background:#e63946;color:#fff;border:none;border-radius:4px;cursor:pointer;}"
    "</style></head><body>"
    "<h2>הגדרת מנורת ההתרעה</h2>"
    "<form action='/save' method='POST'>"
    "  <label>שם רשת WiFi</label>"
    "  <input type='text'     name='ssid' autocomplete='off'>"
    "  <label>סיסמת WiFi</label>"
    "  <input type='password' name='pass'>"
    "  <label>שם היישוב / האזור שלך (בעברית)</label>"
    "  <input type='text'     name='location' placeholder='לדוגמה: רמת גן'>"
    "  <small>הקלד את שם האזור בדיוק כפי שמופיע באתר פיקוד העורף.</small>"
    "  <button type='submit'>שמור והתחבר</button>"
    "</form></body></html>";

  server.send(200, "text/html; charset=utf-8", html);
}

// ══════════════════════════════════════════════
// handleSaveConfig – persist credentials to NVS
// ══════════════════════════════════════════════
void handleSaveConfig() {
  if (server.hasArg("ssid") && server.hasArg("pass") && server.hasArg("location")) {
    wifi_ssid       = server.arg("ssid");
    wifi_pass       = server.arg("pass");
    target_location = server.arg("location");

    preferences.putString("ssid",     wifi_ssid);
    preferences.putString("pass",     wifi_pass);
    preferences.putString("location", target_location);

    Serial.print("Config saved. Location: ");
    Serial.println(target_location);

    server.send(200, "text/html; charset=utf-8",
      "<!DOCTYPE html><html><head><meta charset='UTF-8'></head>"
      "<body><h3>ההגדרות נשמרו. מתחבר…</h3></body></html>");
    delay(2000);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Bad Request – missing fields");
  }
}

// ══════════════════════════════════════════════
// connectToWiFi
// ══════════════════════════════════════════════
void connectToWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(wifi_ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500);
    Serial.print(".");
    tries++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nConnection failed – reverting to hotspot mode.");
    preferences.putString("ssid", "");   // clear bad credentials
    startCaptivePortal();
  } else {
    Serial.println("\nConnected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  }
}