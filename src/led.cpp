#include "led.h"

#include <Adafruit_NeoPixel.h>

namespace {

Adafruit_NeoPixel* strip = nullptr;

int g_pin = 8;
int g_count = 1;
uint8_t g_bri = 255;

LedMode g_mode = LedMode::OFF;
LedMode g_forced = LedMode::OFF;
bool g_forcedActive = false;
uint32_t g_forcedUntil = 0;
uint32_t g_bootUntil = 0;

uint32_t g_lastKey = 0;
bool g_shownOnce = false;

// A pixel at full white draws ~60 mA; keep the whole strip inside a
// budget that a USB port can safely deliver, then apply the user %.
uint8_t powerBudget(int count, int percent) {
  const int budgetMa = 500;
  float scale = 1.0f;
  int fullMa = count * 60;
  if (fullMa > budgetMa) scale = (float)budgetMa / (float)fullMa;
  float b = 255.0f * (percent / 100.0f) * scale;
  if (b < 1.0f) b = 1.0f;
  if (b > 255.0f) b = 255.0f;
  return (uint8_t)b;
}

// 0..255 triangle wave – reads like a soft pulse without float math per call.
uint8_t triangle(uint32_t now, uint32_t period) {
  uint32_t p = (now % period) * 512 / period;  // 0..511
  if (p > 255) p = 511 - p;
  return (uint8_t)p;
}

uint32_t scaled(uint8_t r, uint8_t g, uint8_t b, uint8_t f) {
  return ((uint32_t)((r * (uint16_t)f) / 255) << 16) |
         ((uint32_t)((g * (uint16_t)f) / 255) << 8) |
         ((uint32_t)((b * (uint16_t)f) / 255));
}

void paintAll(uint32_t color) {
  for (int i = 0; i < g_count; i++) strip->setPixelColor(i, color);
  strip->show();
}

}  // namespace

namespace led {

void begin(int pin, int count, int briPercent) {
  g_pin = pin;
  g_count = count < 1 ? 1 : count;
  g_bri = powerBudget(g_count, briPercent);

  if (strip) {          // re-initialised at runtime (portal "test" with new values)
    delete strip;
    strip = nullptr;
  }
  strip = new Adafruit_NeoPixel(g_count, g_pin, NEO_GRB + NEO_KHZ800);
  strip->begin();
  strip->setBrightness(g_bri);
  strip->clear();
  strip->show();

  g_shownOnce = false;   // force the next update() to repaint the new driver
  g_bootUntil = millis() + 700;  // white flash proves the LED path works
}

void setMode(LedMode m) { g_mode = m; }

LedMode mode() { return g_mode; }

void overrideFor(LedMode m, uint32_t ms) {
  g_forced = m;
  g_forcedActive = true;
  g_forcedUntil = millis() + ms;
}

void update() {
  if (!strip) return;

  uint32_t now = millis();

  LedMode m = g_mode;
  if (g_forcedActive) {
    if ((int32_t)(now - g_forcedUntil) >= 0) {
      g_forcedActive = false;
    } else {
      m = g_forced;
    }
  }
  if ((int32_t)(now - g_bootUntil) < 0) m = LedMode::BOOT;

  // Compute the frame; only push to the strip when something changed.
  uint32_t key = 0;
  uint32_t color = 0;
  int walk = -1;  // TEST mode uses a moving pixel instead of a uniform fill

  switch (m) {
    case LedMode::OFF:
      color = 0;
      key = 1;
      break;

    case LedMode::BOOT:
      color = strip->Color(70, 70, 70);
      key = 2;
      break;

    case LedMode::PORTAL:
      color = scaled(0, 80, 255, triangle(now, 2400));
      key = (3u << 28) | (now / 60);
      break;

    case LedMode::CONNECTING:
      color = ((now / 300) % 2) ? scaled(255, 140, 0, 255) : 0;
      key = (4u << 28) | (now / 100);
      break;

    case LedMode::ALERT_PRIMARY:
      // hard strobe: unmistakably an active alert in the main areas
      color = ((now / 150) % 2) ? scaled(255, 0, 0, 255) : 0;
      key = (5u << 28) | (now / 50);
      break;

    case LedMode::ALERT_SECONDARY:
      color = scaled(255, 120, 0, triangle(now, 1400));
      key = (6u << 28) | (now / 60);
      break;

    case LedMode::TEST: {
      key = (7u << 28) | (now / 250);
      if (g_count == 1) {
        color = ((now / 250) % 2) ? scaled(255, 255, 255, 255) : 0;
      } else {
        walk = (int)((now / 250) % (uint32_t)(g_count + 2));
        if (walk >= g_count) walk = -1;  // short gap between passes
      }
      break;
    }
  }

  if (g_shownOnce && key == g_lastKey) return;
  g_lastKey = key;
  g_shownOnce = true;

  if (m == LedMode::TEST && walk >= 0) {
    strip->clear();
    strip->setPixelColor(walk, strip->Color(255, 255, 255));
    strip->show();
  } else {
    paintAll(color);
  }
}

}  // namespace led
