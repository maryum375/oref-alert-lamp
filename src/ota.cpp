#include "ota.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "alerts.h"
#include "led.h"
#include "settings.h"
#include "version.h"

namespace {

// GitHub "get the latest published release" endpoint. Unauthenticated calls
// are rate-limited to 60/hour/IP, hence the 60 s floor on the interval.
const char* API_URL = "https://api.github.com/repos/" OTA_REPO "/releases/latest";

const uint32_t META_TIMEOUT_MS = 10000;   // release metadata fetch
const uint32_t DL_TIMEOUT_MS = 60000;     // whole .bin download
const uint32_t IDLE_TIMEOUT_MS = 15000;   // no bytes at all → stalled
const uint32_t MIN_INTERVAL_S = 60;       // keep clear of GitHub's rate limit

uint32_t lastCheckAt = 0;
uint32_t checks = 0;
uint32_t fails = 0;
int lastCode = 0;
String latestTag;      // newest release tag we saw ("" = never checked)
String lastError;
bool forceCheck = false;   // set by checkNow()
bool updating = false;     // download/flash in progress

// Compare two "1.2.3" version strings (a leading 'v'/'V' is ignored).
// Returns -1 when a<b, 0 when equal, +1 when a>b.
int cmpVersion(String a, String b) {
  a.trim(); b.trim();
  if (a.startsWith("v") || a.startsWith("V")) a = a.substring(1);
  if (b.startsWith("v") || b.startsWith("V")) b = b.substring(1);
  for (int part = 0; part < 3; part++) {
    int da = a.indexOf('.'), db = b.indexOf('.');
    String pa = da < 0 ? a : a.substring(0, da);
    String pb = db < 0 ? b : b.substring(0, db);
    a = da < 0 ? "0" : a.substring(da + 1);
    b = db < 0 ? "0" : b.substring(db + 1);
    int va = pa.toInt(), vb = pb.toInt();
    if (va != vb) return va < vb ? -1 : 1;
  }
  return 0;
}

// Download the release asset and write it to the inactive OTA partition.
// Chunked (not Update::writeStream) so the rainbow LED keeps animating and
// the serial log can show progress while the transfer runs.
bool downloadAndFlash(const String& url, int expectedSize, String& err) {
  WiFiClientSecure client;
  client.setInsecure();  // TODO: pin GitHub's root CA instead of skipping

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(DL_TIMEOUT_MS / 4);
  http.setUserAgent("oref-alert-lamp/" FW_VERSION);
  if (!http.begin(client, url)) {
    err = "begin() failed";
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    err = "download HTTP " + String(code);
    http.end();
    return false;
  }

  int total = http.getSize();
  if (total <= 0) total = expectedSize;   // chunked transfer – use the API's size
  if (total <= 0) {
    err = "unknown download size";
    http.end();
    return false;
  }

  if (!Update.begin(total)) {
    err = String("not enough space: ") + Update.errorString();
    http.end();
    return false;
  }

  updating = true;
  led::setMode(LedMode::UPDATING);

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[1024];
  int remaining = total;
  uint32_t t0 = millis(), lastByteAt = millis(), lastPct = 0;

  while (http.connected() && remaining > 0) {
    size_t avail = stream->available();
    if (avail) {
      size_t want = avail > sizeof(buf) ? sizeof(buf) : avail;
      int rd = stream->read(buf, want);
      if (rd <= 0) break;
      if (Update.write(buf, rd) != (size_t)rd) {
        err = String("write failed: ") + Update.errorString();
        Update.abort();
        http.end();
        updating = false;
        return false;
      }
      remaining -= rd;
      lastByteAt = millis();
      uint32_t pct = (uint32_t)(total - remaining) * 100 / total;
      if (pct >= lastPct + 10) {
        lastPct = pct - pct % 10;
        Serial.println("[ota] download " + String(pct) + "%");
      }
    } else {
      if (millis() - lastByteAt > IDLE_TIMEOUT_MS) {
        err = "download stalled";
        Update.abort();
        http.end();
        updating = false;
        return false;
      }
      delay(1);
    }
    // A real alert may have started while we were transferring – abort and
    // let the lamp do its job; the next cycle will retry the install.
    alerts::tick();
    if (alerts::primaryActive() || alerts::secondaryActive() ||
        alerts::preWarningActive() || alerts::simulating()) {
      err = "aborted – alert started during download";
      Update.abort();
      http.end();
      updating = false;
      return false;
    }
    led::update();   // keep the rainbow circle running during the transfer
    if (millis() - t0 > DL_TIMEOUT_MS) {
      err = "download timed out";
      Update.abort();
      http.end();
      updating = false;
      return false;
    }
  }
  http.end();

  if (remaining > 0) {
    err = "incomplete download (" + String(total - remaining) + " bytes missing)";
    Update.abort();
    updating = false;
    return false;
  }

  if (!Update.end()) {
    err = String("flash verify failed: ") + Update.errorString();
    updating = false;
    return false;
  }

  Serial.println("[ota] image written OK – rebooting into v" FW_VERSION "→new build");
  return true;
}

void doCheck() {
  checks++;

  WiFiClientSecure client;
  client.setInsecure();  // TODO: pin GitHub's root CA

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(META_TIMEOUT_MS);
  http.setUserAgent("oref-alert-lamp/" FW_VERSION);
  http.addHeader("Accept", "application/vnd.github+json");

  uint32_t t0 = millis();
  int code = http.GET();
  lastCode = code;

  if (code != HTTP_CODE_OK) {
    fails++;
    lastError = "release lookup HTTP " + String(code) + " (" +
                http.errorToString(code) + ")";
    if (code == 403 || code == 429) lastError += " (rate limited?)";
    Serial.println("[ota] check failed: " + lastError);
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    fails++;
    lastError = "release metadata not valid JSON";
    Serial.println("[ota] check failed: " + lastError);
    return;
  }

  String tag = doc["tag_name"].as<const char*>() ? doc["tag_name"].as<String>() : "";
  if (!tag.length()) {
    fails++;
    lastError = "no published release found";
    Serial.println("[ota] check failed: " + lastError);
    return;
  }
  latestTag = tag;

  int cmp = cmpVersion(tag, FW_VERSION);
  Serial.println("[ota] latest release " + tag + " vs current v" FW_VERSION " → " +
                 (cmp > 0 ? "UPDATE AVAILABLE" : cmp == 0 ? "up to date" : "older than local build") +
                 " (" + String(millis() - t0) + " ms)");
  if (cmp <= 0) {
    lastError = "";
    return;   // nothing to do
  }

  // Never take the lamp offline while it is warning you about something.
  if (alerts::primaryActive() || alerts::secondaryActive() ||
      alerts::preWarningActive() || alerts::simulating()) {
    lastError = "update deferred – alert active";
    Serial.println("[ota] deferring install while an alert is active");
    return;
  }

  // Find the firmware image among the release assets.
  String url;
  int assetSize = 0;
  for (JsonObject a : doc["assets"].as<JsonArray>()) {
    String name = a["name"].as<const char*>() ? a["name"].as<String>() : "";
    if (name.endsWith(".bin")) {
      url = a["browser_download_url"].as<const char*>() ? a["browser_download_url"].as<String>() : "";
      assetSize = a["size"].as<int>();
      break;
    }
  }
  if (!url.length()) {
    fails++;
    lastError = "release " + tag + " has no .bin asset";
    return;
  }

  Serial.println("[ota] installing " + tag + " (" + String(assetSize) + " bytes)…");
  String err;
  if (downloadAndFlash(url, assetSize, err)) {
    // Leave the rainbow on briefly so the final state is visible, then boot.
    for (int i = 0; i < 40; i++) { led::update(); delay(25); }
    Serial.println("[ota] restarting…");
    delay(150);
    ESP.restart();
  }
  fails++;
  lastError = err;
  led::setMode(LedMode::OFF);
  Serial.println("[ota] update failed: " + err);
}

}  // namespace

namespace ota {

void tick(bool allowAuto) {
  if (updating) return;                       // we're inside a transfer
  if (WiFi.status() != WL_CONNECTED) return;

  uint32_t intervalMs = (uint32_t)g_settings.updateIntervalSec * 1000UL;
  if (!allowAuto && !forceCheck) return;      // portal mode: manual checks only

  uint32_t now = millis();
  if (!forceCheck) {
    if (intervalMs == 0) return;              // 0 = automatic checks off
    if (lastCheckAt == 0) {                    // first check ~20 s after boot,
      if (now < 20000) return;                 // so WiFi/alerts settle first
    } else if ((int32_t)(now - lastCheckAt) < (int32_t)intervalMs) {
      return;
    }
  }
  forceCheck = false;
  lastCheckAt = now;
  doCheck();
}

void checkNow() { forceCheck = true; }

String status() {
  JsonDocument doc;
  doc["current"] = FW_VERSION;
  doc["latest"] = latestTag;
  doc["interval_s"] = g_settings.updateIntervalSec;
  doc["checks"] = checks;
  doc["fails"] = fails;
  doc["http"] = lastCode;
  doc["last_error"] = lastError;
  doc["updating"] = updating;
  doc["checked_ms_ago"] = lastCheckAt ? (millis() - lastCheckAt) : 0;
  String out;
  serializeJson(doc, out);
  return out;
}

}  // namespace ota
