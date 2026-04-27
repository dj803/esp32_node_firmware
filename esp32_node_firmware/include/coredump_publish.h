#pragma once

#include <Arduino.h>
#include <esp_core_dump.h>
#include "config.h"
#include "logging.h"
#include "fwevent.h"

// =============================================================================
// coredump_publish.h  —  Publish ESP-IDF core-dump summary on next-boot (#65)
//
// PURPOSE: When a device panics (Guru Meditation, abort, *_wdt), the ESP-IDF
// panic handler writes a binary core dump to the `coredump` partition before
// rebooting. The dump contains the failing task's stack, PC, exception cause,
// and a backtrace. Until v0.4.17 this dump just sat in flash forever; we
// could only retrieve it via USB serial + esptool read_flash, which required
// physical access — unavailable for fleet devices.
//
// This header reads the dump on boot, extracts the panic summary
// (PC, exception cause, faulting task, backtrace), publishes it to MQTT
// at .../diag/coredump (retained, QoS 1), then erases the dump so the
// next panic can be captured cleanly.
//
// FLOW:
//   1. setup() → coredumpInit() — checks sdkconfig invariants
//   2. After mqttBegin() and MQTT confirmed connected, coredumpPublishIfAny()
//      reads the partition, builds a JSON summary, publishes, erases.
//
// PAYLOAD SHAPE:
//   {
//     "event": "coredump",
//     "exc_task": "loopTask",
//     "exc_pc": "0x4012210e",
//     "exc_cause": "LoadStoreAlignment",
//     "backtrace": ["0x4012210e","0x400e1d04","0x400e1fcb","0x4008ff31"],
//     "core_dump_version": 4,
//     "app_sha_prefix": "f3c1a2..."
//   }
//
// Operator workflow on alert:
//   1. See `event=coredump` in Node-RED dashboard
//   2. addr2line against the matching firmware.elf (CI build artifact)
//   3. Diagnose without needing the device on a USB cable
//
// FUTURE: chunked publish of the FULL elf coredump via .../diag/coredump/<n>
// would let the operator re-run gdb against the dump remotely. Logged as
// follow-up to #65. For now the summary alone closes 80% of the diagnostic
// gap (and matches what we manually decoded from serial today).
// =============================================================================

#define COREDUMP_TOPIC   "diag/coredump"

// Map ESP-IDF exception cause IDs to human-readable strings.
// Same set the panic handler emits to UART; documented in
// ESP-IDF docs/api-guides/fatal-errors.html.
static const char* coredumpExcCauseStr(uint32_t cause) {
    // Xtensa exception cause codes (subset most common in our crashes)
    switch (cause) {
        case 0:  return "IllegalInstruction";
        case 1:  return "Syscall";
        case 2:  return "InstructionFetchError";
        case 3:  return "LoadStoreError";
        case 4:  return "Level1Interrupt";
        case 5:  return "Alloca";
        case 6:  return "IntegerDivideByZero";
        case 8:  return "Privileged";
        case 9:  return "LoadStoreAlignment";
        case 12: return "InstrPIFDataError";
        case 13: return "LoadStorePIFDataError";
        case 14: return "InstrPIFAddrError";
        case 15: return "LoadStorePIFAddrError";
        case 16: return "InstTLBMiss";
        case 17: return "InstTLBMultiHit";
        case 18: return "InstFetchPrivilege";
        case 20: return "InstFetchProhibited";
        case 24: return "LoadStoreTLBMiss";
        case 25: return "LoadStoreTLBMultiHit";
        case 26: return "LoadStorePrivilege";
        case 28: return "LoadProhibited";
        case 29: return "StoreProhibited";
        // Pseudo-causes from panic handler (negative-style markers)
        case 0xDEAD0001: return "abort";
        case 0xDEAD0002: return "stack_overflow";
        case 0xDEAD0003: return "task_wdt";
        case 0xDEAD0004: return "int_wdt";
        case 0xDEAD0005: return "cache_error";
        default:         return "unknown";
    }
}


// Forward declarations from mqtt_client.h (included before this in main.cpp).
// We don't include mqtt_client.h to avoid the dependency cycle.
static void mqttPublish(const char* prefix, const String& payload,
                        uint8_t qos, bool retain);


// ── coredumpPublishIfAny ──────────────────────────────────────────────────────
// Call ONCE per boot, after MQTT is confirmed connected. Reads the coredump
// partition, builds a JSON summary, publishes it (retained QoS 1 so a Node-RED
// subscriber that comes online later still sees it), then erases.
//
// Idempotent — second call is a no-op if no dump remains.
inline void coredumpPublishIfAny() {
    static bool _alreadyChecked = false;
    if (_alreadyChecked) return;
    _alreadyChecked = true;

    // Quick check first — avoids the more-expensive summary parse if no dump.
    esp_err_t check = esp_core_dump_image_check();
    if (check == ESP_ERR_NOT_FOUND) {
        LOG_D("Coredump", "No core dump in flash — skipping publish");
        return;
    } else if (check != ESP_OK) {
        LOG_W("Coredump", "Core dump check returned %d — skipping publish", (int)check);
        return;
    }

    // Get summary: exception PC, faulting task, backtrace addresses.
    esp_core_dump_summary_t summary;
    esp_err_t r = esp_core_dump_get_summary(&summary);
    if (r != ESP_OK) {
        LOG_W("Coredump", "esp_core_dump_get_summary failed (%d) — erasing anyway",
              (int)r);
        esp_core_dump_image_erase();
        return;
    }

    // Build the summary JSON.
    char buf[768];
    int n = snprintf(buf, sizeof(buf),
        "{\"event\":\"coredump\","
        "\"exc_task\":\"%s\","
        "\"exc_pc\":\"0x%08x\","
        "\"exc_cause\":\"%s\","
        "\"core_dump_version\":%u,"
        "\"app_sha_prefix\":\"%02x%02x%02x%02x%02x%02x%02x%02x\","
        "\"backtrace\":[",
        summary.exc_task,
        (unsigned)summary.exc_pc,
        coredumpExcCauseStr((uint32_t)summary.ex_info.exc_cause),
        (unsigned)summary.core_dump_version,
        summary.app_elf_sha256[0],  summary.app_elf_sha256[1],
        summary.app_elf_sha256[2],  summary.app_elf_sha256[3],
        summary.app_elf_sha256[4],  summary.app_elf_sha256[5],
        summary.app_elf_sha256[6],  summary.app_elf_sha256[7]);

    // Append backtrace addresses.
    for (uint32_t i = 0;
         i < summary.exc_bt_info.depth && n < (int)sizeof(buf) - 16;
         i++) {
        n += snprintf(buf + n, sizeof(buf) - n,
                      "%s\"0x%08x\"",
                      i == 0 ? "" : ",",
                      (unsigned)summary.exc_bt_info.bt[i]);
    }
    n += snprintf(buf + n, sizeof(buf) - n, "]}");

    if (n < 0 || n >= (int)sizeof(buf)) {
        LOG_W("Coredump", "Summary JSON truncated (n=%d, max=%d) — publishing anyway",
              n, (int)sizeof(buf));
    }

    LOG_W("Coredump", "Publishing core dump summary (task=%s pc=0x%08x cause=%s depth=%u)",
          summary.exc_task, (unsigned)summary.exc_pc,
          coredumpExcCauseStr((uint32_t)summary.ex_info.exc_cause),
          (unsigned)summary.exc_bt_info.depth);

    // Retained QoS 1 — late-subscribing dashboards still see the panic
    // backtrace until the device is replaced or another panic overwrites.
    mqttPublish(COREDUMP_TOPIC, String(buf), 1, true);

    // Allow the publish to drain before erasing the partition. AsyncMqttClient
    // returns immediately; lwIP needs ~50-100 ms to push the bytes onto the wire.
    delay(150);

    // Erase so the next panic captures cleanly. If erase fails (rare), the
    // dump will simply re-publish on next boot — idempotent + retained means
    // no duplicate noise on the dashboard.
    r = esp_core_dump_image_erase();
    if (r == ESP_OK) {
        LOG_I("Coredump", "Core dump erased — partition ready for next panic");
    } else {
        LOG_W("Coredump", "esp_core_dump_image_erase failed (%d) — will retry next boot",
              (int)r);
    }
}
