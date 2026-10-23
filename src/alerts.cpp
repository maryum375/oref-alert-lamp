#include "alerts.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "log.h"
#include "settings.h"

namespace {

const char* ALERTS_URL = "https://www.oref.org.il/WarningMessages/alert/alerts.json";
const char* REFERER    = "https://www.oref.org.il/";

const char* TAG = "alerts";
const char* TAG_SIM = "sim";

const uint32_t POLL_MS = 3000;     // oref's own app samples every 2 s
const uint32_t HTTP_TIMEOUT_MS = 3500;
const uint32_t HOLD_MS = 10000;    // keep the lamp lit this long after the last hit,
                                   // so one empty/failed poll cannot flicker it off

uint32_t lastPollAt = 0;
uint32_t simUntil = 0;             // real polling is paused until this time
String lastPrimaryArea;            // last configured area that actually matched
String lastSecondaryArea;
uint32_t lastPrimaryAt = 0;
uint32_t lastSecondaryAt = 0;
bool havePrimary = false;
bool haveSecondary = false;
bool preWarning = false;

// diagnostics
uint32_t pollCount = 0;
uint32_t failCount = 0;
int lastCode = 0;
uint32_t lastLatency = 0;
uint32_t lastPayloadBytes = 0;
uint32_t lastSuccessAt = 0;
bool lastParseOk = true;

bool stillHolding(bool have, uint32_t at, uint32_t now) {
  if (!have) return false;
  return (uint32_t)(now - at) < HOLD_MS;
}

int readCat(JsonObject alert) {
  JsonVariant cv = alert["cat"];
  if (cv.is<int>() || cv.is<float>()) return cv.as<int>();
  if (cv.is<const char*>()) return String(cv.as<const char*>()).toInt();
  return -1;
}

void processAlert(JsonObject alert, bool& p, bool& s, bool& pre);
void processPayload(String payload);

void poll() {
  pollCount++;

  WiFiClientSecure client;
  client.setInsecure();  // TODO: pin the oref root CA instead of skipping verification

  HTTPClient http;
  String url = String(ALERTS_URL) + "?t=" + String(millis());  // cache buster
  http.begin(client, url);
  http.addHeader("Referer", REFERER);
  http.addHeader("X-Requested-With", "XMLHttpRequest");
  http.addHeader("Accept", "application/json, text/plain, */*");
  http.addHeader("Cache-Control", "no-cache");
  http.setUserAgent(
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36");
  http.setTimeout(HTTP_TIMEOUT_MS);

  uint32_t t0 = millis();
  int code = http.GET();
  lastCode = code;
  lastLatency = millis() - t0;

  if (code != HTTP_CODE_OK) {
    failCount++;
    LOGW(TAG, "poll failed: HTTP " + String(code) + " (" + http.errorToString(code) +
                  "), keeping previous state");
    http.end();
    return;  // keep previous state
  }

  String payload = http.getString();
  http.end();

  lastPayloadBytes = payload.length();
  lastSuccessAt = millis();
  LOGD(TAG, "poll ok: " + String(lastPayloadBytes) + " bytes in " + String(lastLatency) +
                " ms");
  processPayload(payload);
}

void processAlert(JsonObject alert, bool& p, bool& s, bool& pre) {
  int cat = readCat(alert);
  if (cat >= 15 && cat < 100) return;  // drills (and drill all-clears) are not real
  if (cat == 13) return;               // "האירוע הסתיים" – the areas listed are cleared,
                                       // and they no longer appear as active alerts
  bool isPre = (cat == 14);

  JsonArray data = alert["data"].as<JsonArray>();
  if (data.isNull()) return;

  for (JsonVariant v : data) {
    String area = v.as<String>();
    area.trim();
    if (!area.length()) continue;

    if (g_settings.isPrimary(area)) {
      if (isPre) pre = true; else p = true;
      lastPrimaryArea = area;
    } else if (g_settings.isSecondary(area)) {
      if (isPre) pre = true; else s = true;
      lastSecondaryArea = area;
    }
  }
}

// Shared by the real poller and the serial test hooks: everything from the
// raw body to the alert state, so tests exercise the identical code path.
void processPayload(String payload) {
  // Strip the UTF-8 BOM oref prepends, plus surrounding whitespace.
  payload.replace(String("\xEF\xBB\xBF"), "");
  payload.trim();

  bool p = false, s = false, pre = false;

  if (payload.length() == 0 || payload == "{}" || payload == "[]") {
    lastParseOk = true;  // calm
  } else {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
      lastParseOk = false;
      failCount++;
      // keep the previous state: a malformed body must not clear a real alert
      LOGE(TAG, String("payload is not valid JSON (") + err.c_str() +
                    ") – keeping previous state");
      return;
    }
    lastParseOk = true;

    // oref normally sends an array; special messages arrive as a single object.
    if (doc.is<JsonArray>()) {
      for (JsonObject alert : doc.as<JsonArray>()) {
        if (!alert.isNull()) processAlert(alert, p, s, pre);
      }
    } else if (doc.is<JsonObject>()) {
      processAlert(doc.as<JsonObject>(), p, s, pre);
    }
  }

  bool wasP = alerts::primaryActive(), wasS = alerts::secondaryActive(),
       wasPre = alerts::preWarningActive();

  uint32_t now = millis();
  if (p) { havePrimary = true; lastPrimaryAt = now; }
  if (s) { haveSecondary = true; lastSecondaryAt = now; }
  preWarning = pre;

  // one line per state transition (not per poll)
  if (!wasP && alerts::primaryActive())
    LOGI(TAG, "PRIMARY alert matched: '" + lastPrimaryArea + "'");
  else if (wasP && !alerts::primaryActive())
    LOGI(TAG, "PRIMARY alert cleared");
  if (!wasS && alerts::secondaryActive())
    LOGI(TAG, "SECONDARY alert matched: '" + lastSecondaryArea + "'");
  else if (wasS && !alerts::secondaryActive())
    LOGI(TAG, "SECONDARY alert cleared");
  if (!wasPre && alerts::preWarningActive()) LOGI(TAG, "pre-warning started");
  else if (wasPre && !alerts::preWarningActive()) LOGI(TAG, "pre-warning cleared");
}

}  // namespace

namespace alerts {

void begin() {
  lastPollAt = millis() - POLL_MS;  // poll immediately on start
}

void tick() {
  if (simulating()) return;  // a test payload is in charge right now
  if (WiFi.status() != WL_CONNECTED) return;
  if (!g_settings.hasAreas()) return;

  uint32_t now = millis();
  if ((int32_t)(now - lastPollAt) < (int32_t)POLL_MS) return;
  lastPollAt = now;
  poll();
}

bool primaryActive() {
  return stillHolding(havePrimary, lastPrimaryAt, millis());
}

bool secondaryActive() {
  return stillHolding(haveSecondary, lastSecondaryAt, millis());
}

bool preWarningActive() { return preWarning; }

// ── serial test hooks ────────────────────────
bool simulating() { return (int32_t)(millis() - simUntil) < 0; }

void simulatePrimary() {
  if (!g_settings.primary.size()) {
    LOGW(TAG_SIM, "no PRIMARY area configured – nothing to match");
    return;
  }
  String area = g_settings.primary[0];
  simUntil = millis() + 12000;
  LOGI(TAG_SIM, "injecting primary alert for '" + area + "'");
  processPayload("[{\"id\":\"sim\",\"cat\":\"1\",\"title\":\"TEST\",\"data\":[\"" + area +
                 "\"],\"desc\":\"\"}]");
}

void simulateSecondary() {
  if (!g_settings.secondary.size()) {
    LOGW(TAG_SIM, "no SECONDARY area configured – nothing to match");
    return;
  }
  String area = g_settings.secondary[0];
  simUntil = millis() + 12000;
  LOGI(TAG_SIM, "injecting secondary alert for '" + area + "'");
  processPayload("[{\"id\":\"sim\",\"cat\":\"1\",\"title\":\"TEST\",\"data\":[\"" + area +
                 "\"],\"desc\":\"\"}]");
}

void clearSimulation() {
  simUntil = 0;
  havePrimary = false;
  haveSecondary = false;
  preWarning = false;
  lastPrimaryArea = "";
  lastSecondaryArea = "";
  LOGI(TAG_SIM, "cleared");
}

String status() {
  JsonDocument doc;
  doc["polls"] = pollCount;
  doc["fails"] = failCount;
  doc["http"] = lastCode;
  doc["latency_ms"] = lastLatency;
  doc["payload_bytes"] = lastPayloadBytes;
  doc["parse_ok"] = lastParseOk;
  doc["last_success_ms_ago"] = lastSuccessAt ? (millis() - lastSuccessAt) : 0;
  doc["primary"] = primaryActive();
  doc["secondary"] = secondaryActive();
  doc["prewarning"] = preWarningActive();
  doc["sim"] = simulating();
  doc["primary_area"] = lastPrimaryArea;
  doc["secondary_area"] = lastSecondaryArea;
  String out;
  serializeJson(doc, out);
  return out;
}

}  // namespace alerts
