#pragma once

#include <Arduino.h>

// ──────────────────────────────────────────────
// Captive-portal configuration interface.
// Runs as AP (+STA so the area list / WiFi scan still work) and serves:
//   /            setup page (RTL Hebrew)
//   /areas.json  the 1452 official alert areas, grouped by district
//   /scan.json   results of an on-demand WiFi scan
//   /status      current settings + WiFi + alert diagnostics
//   /save        POST: persist and restart
//   /test        force an LED pattern for a few seconds
//   /reset       POST: factory reset and restart
// ──────────────────────────────────────────────
namespace portal {

void begin();
void tick();
bool active();

// Read-only status interface, available while the lamp is running on WiFi:
//   http://alertlamp.local  (mDNS) or http://<ip>/
String statusJson();
void beginRuntime();
void runtimeTick();

}  // namespace portal
