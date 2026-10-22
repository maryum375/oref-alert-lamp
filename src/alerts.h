#pragma once

#include <Arduino.h>

// ──────────────────────────────────────────────
// Pikud HaOref (Home Front Command) alert poller.
// Endpoint: https://www.oref.org.il/WarningMessages/alert/alerts.json
// (unofficial, geo-restricted to Israeli IPs, requires Referer +
//  X-Requested-With headers, answers with a UTF-8 BOM and possibly an
//  empty body / "{}" / "[]" when everything is calm.)
// ──────────────────────────────────────────────
namespace alerts {

void begin();
void tick();   // call every loop() while WiFi is connected

// Test hooks: push a synthetic payload through the real parsing/matching path.
void simulatePrimary();
void simulateSecondary();
void clearSimulation();
bool simulating();

bool primaryActive();    // a configured main area is under alert
bool secondaryActive();  // a configured secondary area is under alert
bool preWarningActive(); // cat 14 – "alerts expected shortly"

String status();  // diagnostics as JSON (used by the portal /status route)

}  // namespace alerts
