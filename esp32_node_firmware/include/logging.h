#pragma once

// =============================================================================
// logging.h  —  Compile-time filtered structured logging macros
//
// Usage:
//   LOG_D("MQTT", "rx topic %s len %d", topic, len);   // DEBUG
//   LOG_I("OTA",  "update available: %s", ver);        // INFO
//   LOG_W("WiFi", "retry %d / %d", attempt, max);      // WARN
//   LOG_E("BOOT", "NVS load failed - entering AP mode"); // ERROR
//
// Each call emits  [L][MODULE] message\n  on Serial.
//
// Set LOG_LEVEL in config.h or via build_flags:
//   -DLOG_LEVEL=LOG_LEVEL_WARN   (production: only WARN + ERROR)
//   -DLOG_LEVEL=LOG_LEVEL_DEBUG  (verbose: everything)
//
// The default (if config.h does not define it) is LOG_LEVEL_INFO.
// Messages below the active level are removed entirely at compile time —
// zero flash footprint, zero runtime cost.
// =============================================================================

#define LOG_LEVEL_NONE  0
#define LOG_LEVEL_ERROR 1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_INFO  3
#define LOG_LEVEL_DEBUG 4

// Default level: INFO (errors + warnings + operational events)
#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_LEVEL_INFO
#endif

// ── Macro definitions ─────────────────────────────────────────────────────────
// The tag argument MUST be a string literal (it is concatenated at compile time).
// The fmt argument must NOT end with \n — the macro appends one.
// Extra args are forwarded to Serial.printf.

#if LOG_LEVEL >= LOG_LEVEL_ERROR
#define LOG_E(tag, fmt, ...) Serial.printf("[E][" tag "] " fmt "\n", ##__VA_ARGS__)
#else
#define LOG_E(tag, fmt, ...) do {} while (0)
#endif

#if LOG_LEVEL >= LOG_LEVEL_WARN
#define LOG_W(tag, fmt, ...) Serial.printf("[W][" tag "] " fmt "\n", ##__VA_ARGS__)
#else
#define LOG_W(tag, fmt, ...) do {} while (0)
#endif

#if LOG_LEVEL >= LOG_LEVEL_INFO
#define LOG_I(tag, fmt, ...) Serial.printf("[I][" tag "] " fmt "\n", ##__VA_ARGS__)
#else
#define LOG_I(tag, fmt, ...) do {} while (0)
#endif

#if LOG_LEVEL >= LOG_LEVEL_DEBUG
#define LOG_D(tag, fmt, ...) Serial.printf("[D][" tag "] " fmt "\n", ##__VA_ARGS__)
#else
#define LOG_D(tag, fmt, ...) do {} while (0)
#endif
