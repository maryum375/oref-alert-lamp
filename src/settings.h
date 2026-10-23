#pragma once

#include <Arduino.h>
#include <vector>

// ──────────────────────────────────────────────
// Device configuration, persisted in NVS.
// ──────────────────────────────────────────────
struct Settings {
  String ssid;
  String pass;

  // Hebrew area labels, byte-identical to alerts.json -> data[]
  std::vector<String> primary;    // main areas   → urgent pattern
  std::vector<String> secondary;  // extra areas  → calmer pattern

  int ledPin   = 8;     // GPIO8 = onboard addressable RGB on ESP32-C6-DevKitC-1
  int ledCount = 1;
  int ledBri   = 40;    // percent, clamped by a power budget for long strips

  int updateIntervalSec = 120;  // GitHub release check interval; 0 = never

  // Log capture filter for the /dev page ring buffer (serial always gets
  // everything): 0 = ERROR, 1 = WARN, 2 = INFO (default), 3 = DEBUG.
  int logLevel = 2;

  bool hasWifi() const { return ssid.length() > 0; }
  bool hasAreas() const { return !primary.empty() || !secondary.empty(); }
  bool isComplete() const { return hasWifi() && hasAreas(); }

  bool isPrimary(const String& area) const;
  bool isSecondary(const String& area) const;
};

extern Settings g_settings;

namespace cfg {
void load();            // read everything from NVS
void save();            // write everything to NVS
void clearWifi();       // forget WiFi credentials only
void factoryReset();    // wipe the whole namespace
String dump();          // one-line summary for the serial log (never prints the password)
}  // namespace cfg
