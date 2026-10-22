#pragma once

#include <Arduino.h>

// ──────────────────────────────────────────────
// Addressable LED (WS2812 / SK6812) output.
// All patterns are non-blocking and time-phase based, so a slow
// HTTP request elsewhere in loop() can never leave the lamp in a
// wrong state - it may only delay the visible update briefly.
// ──────────────────────────────────────────────
enum class LedMode {
  OFF,
  BOOT,             // short white flash after start-up
  PORTAL,           // slow blue breathing  – configuration AP running
  CONNECTING,       // amber blink          – joining WiFi
  ALERT_PRIMARY,    // fast red strobe      – main areas
  ALERT_SECONDARY,  // slow amber breathing – secondary areas
  TEST,             // white pixel walk     – wiring / pixel-count check
  UPDATING,         // spinning rainbow     – firmware update in progress
};

namespace led {

void begin(int pin, int count, int briPercent);
void setMode(LedMode m);
LedMode mode();

// Call every loop() pass.
void update();

// Portal "test" endpoint: force a mode for a while, then resume.
void overrideFor(LedMode m, uint32_t ms);

}  // namespace led
