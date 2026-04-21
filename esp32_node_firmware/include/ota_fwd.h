#pragma once

// =============================================================================
// ota_fwd.h  —  Forward declarations for ota.h public API
//
// Include this file instead of ota.h when you only need to CALL these
// functions (not define them). This breaks the include-order coupling:
// mqtt_client.h can call otaCheckNow() without ota.h having been included yet.
//
// The actual definitions live in ota.h, which must be included exactly once
// in the translation unit (currently esp32_firmware.ino).
// =============================================================================

void otaCheckNow(bool isSiblingRetry = false);   // Trigger an immediate OTA firmware check
void otaTrigger();    // Schedule an OTA check on the next loop() tick
