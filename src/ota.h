#pragma once

#include <Arduino.h>

// ──────────────────────────────────────────────
// GitHub Releases auto-updater.
// Every g_settings.updateIntervalSec seconds (0 = disabled) it fetches
// https://api.github.com/repos/<OTA_REPO>/releases/latest, compares the tag
// with FW_VERSION and, when the release is newer, downloads its *.bin into
// the inactive OTA partition and reboots.
// An update never interrupts an active alert – it is deferred until calm.
// ──────────────────────────────────────────────
namespace ota {

void tick(bool allowAuto = true);   // call every loop(); allowAuto=false in portal mode
void checkNow();    // queue an immediate check (portal button / serial 'u')

String status();    // diagnostics as JSON (used by the /status route)

}  // namespace ota
