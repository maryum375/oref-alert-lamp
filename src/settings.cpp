#include "settings.h"

#include <Preferences.h>

Settings g_settings;

namespace {

Preferences prefs;
const char* NS = "alert-lamp";
const char SEP = '\n';

String join(const std::vector<String>& v) {
  String out;
  for (size_t i = 0; i < v.size(); i++) {
    if (i) out += SEP;
    out += v[i];
  }
  return out;
}

std::vector<String> split(const String& s) {
  std::vector<String> out;
  int start = 0;
  while (start <= (int)s.length()) {
    int end = s.indexOf(SEP, start);
    if (end < 0) end = s.length();
    String part = s.substring(start, end);
    part.trim();
    if (part.length()) out.push_back(part);
    start = end + 1;
  }
  return out;
}

}  // namespace

bool Settings::isPrimary(const String& area) const {
  for (const String& a : primary) if (a == area) return true;
  return false;
}

bool Settings::isSecondary(const String& area) const {
  for (const String& a : secondary) if (a == area) return true;
  return false;
}

namespace cfg {

void load() {
  prefs.begin(NS, true);
  g_settings.ssid      = prefs.getString("ssid", "");
  g_settings.pass      = prefs.getString("pass", "");
  g_settings.primary   = split(prefs.getString("primary", ""));
  g_settings.secondary = split(prefs.getString("secondary", ""));
  g_settings.ledPin    = prefs.getInt("led_pin", g_settings.ledPin);
  g_settings.ledCount  = prefs.getInt("led_count", g_settings.ledCount);
  g_settings.ledBri    = prefs.getInt("led_bri", g_settings.ledBri);
  g_settings.updateIntervalSec = prefs.getInt("upd_int", g_settings.updateIntervalSec);
  prefs.end();

  if (g_settings.ledCount < 1) g_settings.ledCount = 1;
  if (g_settings.ledCount > 1024) g_settings.ledCount = 1024;
  if (g_settings.ledBri < 1) g_settings.ledBri = 1;
  if (g_settings.ledBri > 100) g_settings.ledBri = 100;
  if (g_settings.updateIntervalSec < 0) g_settings.updateIntervalSec = 0;
  if (g_settings.updateIntervalSec > 0 && g_settings.updateIntervalSec < 60)
    g_settings.updateIntervalSec = 60;  // GitHub allows 60 unauthenticated calls/hour
}

void save() {
  prefs.begin(NS, false);
  prefs.putString("ssid", g_settings.ssid);
  prefs.putString("pass", g_settings.pass);
  prefs.putString("primary", join(g_settings.primary));
  prefs.putString("secondary", join(g_settings.secondary));
  prefs.putInt("led_pin", g_settings.ledPin);
  prefs.putInt("led_count", g_settings.ledCount);
  prefs.putInt("led_bri", g_settings.ledBri);
  prefs.putInt("upd_int", g_settings.updateIntervalSec);
  prefs.end();
}

void clearWifi() {
  prefs.begin(NS, false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
  g_settings.ssid = "";
  g_settings.pass = "";
}

void factoryReset() {
  prefs.begin(NS, false);
  prefs.clear();
  prefs.end();
  g_settings = Settings();
}

String dump() {
  String out = "ssid='" + g_settings.ssid + "' primary=" + String(g_settings.primary.size()) +
               " secondary=" + String(g_settings.secondary.size()) + " led=GPIO" +
               String(g_settings.ledPin) + "x" + String(g_settings.ledCount) + "@" +
               String(g_settings.ledBri) + "%";
  return out;
}

}  // namespace cfg
