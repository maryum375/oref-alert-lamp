#pragma once

// Firmware version – shown on the portal status page and compared against
// the latest GitHub Release tag (leading 'v' optional) by the OTA updater.
#define FW_VERSION "0.5.2"

// GitHub repository (owner/name) whose releases are checked for updates.
// Must contain release assets named *.bin (uploaded by .github/workflows).
#define OTA_REPO "maryum375/oref-alert-lamp"
