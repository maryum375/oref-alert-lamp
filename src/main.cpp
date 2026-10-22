
#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include "alerts.h"
#include "areas_data.h"
#include "led.h"
#include "portal.h"
#include "settings.h"
#include "version.h"

// ──────────────────────────────────────────────
// Hardware / behaviour constants
// ──────────────────────────────────────────────
const int BOOT_BTN_PIN = 9;           // "BOOT" button on the C6-DevKitC-1, active LOW.
                                      // Hold it while resetting → open the portal
                                      // without erasing the stored configuration.
const uint32_t WIFI_TIMEOUT_MS = 20000;   // give up joining after 20 s → portal
const uint32_t WIFI_RETRY_MS = 15000;     // retry interval while disconnected
const uint32_t WIFI_DOWN_TO_PORTAL_MS = 120000;  // unreachable for 2 min → portal
const uint32_t STATUS_MS = 10000;

enum class State { PORTAL, WIFI_WAITING, RUNNING };

State state = State::PORTAL;
uint32_t wifiStartedAt = 0;
uint32_t disconnectedSince = 0;
uint32_t lastWifiRetryAt = 0;
uint32_t lastStatusAt = 0;

void startWifi() {
  Serial.println("[wifi] connecting to '" + g_settings.ssid + "' …");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(g_settings.ssid.c_str(), g_settings.pass.c_str());
  wifiStartedAt = millis();
  state = State::WIFI_WAITING;
  led::setMode(LedMode::CONNECTING);
}

void startPortal(const char* reason) {
  Serial.println(String("[portal] ") + reason);
  portal::begin();
  state = State::PORTAL;
  led::setMode(LedMode::PORTAL);
}

void onWifiUp() {
  Serial.println("[wifi] connected, IP " + WiFi.localIP().toString() +
                 " RSSI " + String(WiFi.RSSI()) + " dBm");
  alerts::begin();
  disconnectedSince = 0;
  lastWifiRetryAt = 0;
  state = State::RUNNING;

  portal::beginRuntime();
  if (MDNS.begin("alertlamp")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("[mdns] http://alertlamp.local/");
  }
}

void printStatus() {
  Serial.print("[status] wifi=");
  Serial.print(WiFi.status() == WL_CONNECTED ? "up" : "DOWN");
  Serial.print(" heap=");
  Serial.print(ESP.getFreeHeap());
  Serial.print(" alerts=");
  Serial.print(alerts::primaryActive() ? "PRIMARY" : "-");
  Serial.print(alerts::secondaryActive() ? " SECONDARY" : "");
  Serial.print(alerts::preWarningActive() ? " PRE" : "");
  Serial.print(" | ");
  Serial.println(alerts::status());
}

bool areaExists(const String& name) {
  for (int i = 0; i < AREAS_COUNT; i++) {
    if (name == AREAS[i].label) return true;
  }
  return false;
}

void validateAreas(const char* kind, const std::vector<String>& areas) {
  for (const String& a : areas) {
    if (areaExists(a)) {
      Serial.println(String("[cfg] ") + kind + " area OK: '" + a + "'");
    } else {
      Serial.println(String("[cfg] WARNING: ") + kind + " '" + a +
                     "' is NOT an official oref area label – it will never match!");
    }
  }
}

// Serial test console: p/s inject a fake alert through the real matching
// code, c clears it, o reopens the portal, ? prints status.
void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 'p': alerts::simulatePrimary(); break;
      case 's': alerts::simulateSecondary(); break;
      case 'c': alerts::clearSimulation(); break;
      case 'o': startPortal("opened by serial command"); break;
      case '?': printStatus(); break;
      case '\n': case '\r': case ' ': break;
      default:
        Serial.println("commands: p=sim primary  s=sim secondary  c=clear  o=open portal  ?=status");
    }
  }
}

// ══════════════════════════════════════════════
// setup
// ══════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(150);
  Serial.println();
  Serial.println("=== Homefront Alert Lamp v" FW_VERSION " ===");

  cfg::load();
  Serial.println("[cfg] " + cfg::dump());
  validateAreas("primary", g_settings.primary);
  validateAreas("secondary", g_settings.secondary);

  led::begin(g_settings.ledPin, g_settings.ledCount, g_settings.ledBri);
  led::update();  // boot flash proves the LED wiring works

  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
  delay(20);
  bool btnHeld = digitalRead(BOOT_BTN_PIN) == LOW;
  if (btnHeld) Serial.println("[boot] BOOT button held");

  if (!g_settings.isComplete() || btnHeld) {
    startPortal(g_settings.isComplete() ? "opened by BOOT button"
                                        : "first run / incomplete configuration");
  } else {
    startWifi();
  }

  Serial.println("[hint] serial commands: p=sim primary alert  s=sim secondary  c=clear  o=open portal  ?=status");
}

// ══════════════════════════════════════════════
// loop – fully non-blocking: the LED keeps animating
// while WiFi and HTTP do their thing.
// ══════════════════════════════════════════════
void loop() {
  handleSerial();
  led::update();

  if (portal::active()) {
    portal::tick();
    return;
  }

  uint32_t now = millis();

  if (state == State::WIFI_WAITING) {
    if (WiFi.status() == WL_CONNECTED) {
      onWifiUp();
    } else if (now - wifiStartedAt > WIFI_TIMEOUT_MS) {
      // Don't wipe the credentials: bring the portal up so the user can
      // correct them (the previous firmware deleted them on failure).
      startPortal("WiFi connection failed");
    }
    return;
  }

  // ── RUNNING ──
  if (state == State::RUNNING && WiFi.status() != WL_CONNECTED) {
    if (!disconnectedSince) disconnectedSince = now;

    if (now - disconnectedSince > WIFI_DOWN_TO_PORTAL_MS) {
      startPortal("WiFi unreachable for 2 minutes");
      return;
    }

    if (now - lastWifiRetryAt > WIFI_RETRY_MS) {
      lastWifiRetryAt = now;
      Serial.println("[wifi] lost – reconnecting…");
      WiFi.reconnect();
    }
    led::setMode(LedMode::CONNECTING);
    return;
  }
  disconnectedSince = 0;

  portal::runtimeTick();
  alerts::tick();

  if (alerts::primaryActive()) {
    led::setMode(LedMode::ALERT_PRIMARY);
  } else if (alerts::secondaryActive() || alerts::preWarningActive()) {
    led::setMode(LedMode::ALERT_SECONDARY);
  } else {
    led::setMode(LedMode::OFF);
  }

  if (now - lastStatusAt >= STATUS_MS) {
    lastStatusAt = now;
    printStatus();
  }
}
