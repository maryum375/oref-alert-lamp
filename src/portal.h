#pragma once

#include <Arduino.h>

// ──────────────────────────────────────────────
// Captive-portal configuration interface.
// Runs as AP (+STA so the area list / WiFi scan / connect test still work).
//
// Pages:
//   /            initial setup – WiFi + areas (only while configuration is
//                incomplete; a configured device is redirected to /settings)
//   /settings    settings with a sidebar: areas · networking · LED brightness,
//                plus a factory-reset button (with confirmation)
//   /dev         internal development / troubleshooting page – never linked
//
// Endpoints:
//   /style.css, /app.js   shared assets
//   /areas.json           the 1452 official alert areas, grouped by district
//   /scan.json            results of an on-demand WiFi scan
//   /status               current settings + WiFi + alert + OTA diagnostics
//   /connect      POST: try to join a network (nothing persisted)
//   /save         POST: initial setup – persist WiFi + areas and restart
//   /savenet      POST: persist WiFi credentials and restart
//   /saveareas    POST: persist areas (applied live)
//   /saveled      POST: persist brightness (applied live)
//   /savedev      POST: persist pin/count/brightness/update-interval/log-level (dev)
//   /sim          POST: alert simulation through the real matching code (dev)
//   /restart      POST: reboot (dev)
//   /test         force an LED pattern for a few seconds
//   /logs.json    live log: lines with seq > ?since=N (0/absent = whole buffer)
//   /logs.txt     live log: whole buffer as a downloadable text file
//   /logclear     POST: drop every captured log line (dev)
//   /reset        POST: factory reset and restart
//   /updcheck     POST: queue an immediate firmware update check
// ──────────────────────────────────────────────
namespace portal {

void begin();
void tick();
bool active();

// Read-only status interface, available while the lamp is running on WiFi:
//   http://alertlamp.local  (mDNS) or http://<ip>/
void beginRuntime();
void runtimeTick();

}  // namespace portal
