#pragma once

// =============================================================================
// led_schedule.h  —  Time-of-day LED automation (#22, v0.4.26)
//
// Per-device cron-style table that fires LED commands at specific
// hour:minute pairs. Persisted to NVS so a reboot (or OTA) restores
// the schedule. Actions are stored as JSON strings — at fire time the
// stored payload is re-fed through handleLedCommand(), so any cmd/led
// schema is reusable in a schedule entry without duplicating the
// dispatcher.
//
// MQTT API (commands handled in mqtt_client.h, dispatched from cmd/led):
//
//   Add / replace a slot:
//     {"cmd":"sched_add", "id":"morning",
//      "hour":7, "minute":0,
//      "action":{"cmd":"override","r":255,"g":150,"b":0,
//                "anim":"breathing","duration_ms":3600000}}
//
//   Remove a slot by id:
//     {"cmd":"sched_remove", "id":"morning"}
//
//   List slots (publishes JSON to .../status/led_schedule, retained):
//     {"cmd":"sched_list"}
//
//   Clear all slots:
//     {"cmd":"sched_clear"}
//
// TIME SOURCE
//   configTime(SAST_OFFSET, 0, "pool.ntp.org") at first OPERATIONAL
//   tick after WiFi is up. Timezone is hard-coded to UTC+2 (SAST, no
//   DST). Operators outside that timezone should override by setting
//   LED_SCHEDULE_TZ_OFFSET_S in build_flags or editing config.h.
//
// FIRE WINDOW
//   Tick polls getLocalTime() once per loop iteration but only acts on
//   minute boundaries. So a slot at 07:00 fires once between 07:00:00
//   and 07:00:59 (whichever loop tick lands first after the minute
//   changes). Drift is bounded to ~loop period (~100 ms) which is well
//   within "time-of-day automation" expectations.
// =============================================================================

#include <Arduino.h>
#include <Preferences.h>
#include <time.h>
#include "config.h"
#include "logging.h"
#include "prefs_quiet.h"

#ifndef LED_SCHEDULE_TZ_OFFSET_S
#define LED_SCHEDULE_TZ_OFFSET_S   (2 * 3600)   // SAST (UTC+2, no DST)
#endif

#ifndef LED_SCHEDULE_MAX_SLOTS
#define LED_SCHEDULE_MAX_SLOTS     8
#endif

#define LED_SCHEDULE_NS            "led_sched"
#define LED_SCHEDULE_ID_MAX        12        // alphanumeric+underscore
#define LED_SCHEDULE_ACTION_MAX    256       // serialised JSON length cap

struct LedScheduleSlot {
    char     id[LED_SCHEDULE_ID_MAX + 1];   // empty = free slot
    uint8_t  hour;                          // 0-23 (UTC offset applied)
    uint8_t  minute;                        // 0-59
    char     action[LED_SCHEDULE_ACTION_MAX]; // raw cmd/led JSON
};

static LedScheduleSlot _ledScheduleSlots[LED_SCHEDULE_MAX_SLOTS];
static int8_t          _ledScheduleLastMinuteFired = -1;
static bool            _ledScheduleNtpStarted      = false;

// Forward decl — implemented in mqtt_client.h, called when a slot fires.
static void handleLedCommand(const char* payload, size_t len);


// ── ID validation ─────────────────────────────────────────────────────────────
static bool _ledScheduleIdOk(const char* id) {
    if (!id || !*id) return false;
    size_t n = strlen(id);
    if (n > LED_SCHEDULE_ID_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!isalnum((unsigned char)c) && c != '_') return false;
    }
    return true;
}


// ── NVS persistence ───────────────────────────────────────────────────────────
// Each slot stored under key "slot_<i>". Empty slot = key absent.
static void _ledScheduleSaveSlot(uint8_t i) {
    if (i >= LED_SCHEDULE_MAX_SLOTS) return;
    Preferences p;
    if (!p.begin(LED_SCHEDULE_NS, false)) return;
    char key[10]; snprintf(key, sizeof(key), "slot_%u", (unsigned)i);
    if (_ledScheduleSlots[i].id[0] == '\0') {
        p.remove(key);
    } else {
        p.putBytes(key, &_ledScheduleSlots[i], sizeof(LedScheduleSlot));
    }
    p.end();
}

static void _ledScheduleLoadAll() {
    memset(_ledScheduleSlots, 0, sizeof(_ledScheduleSlots));
    Preferences p;
    if (!prefsTryBegin(p, LED_SCHEDULE_NS, true)) return;
    for (uint8_t i = 0; i < LED_SCHEDULE_MAX_SLOTS; i++) {
        char key[10]; snprintf(key, sizeof(key), "slot_%u", (unsigned)i);
        size_t len = p.getBytesLength(key);
        if (len == sizeof(LedScheduleSlot)) {
            p.getBytes(key, &_ledScheduleSlots[i], sizeof(LedScheduleSlot));
        }
    }
    p.end();
}


// ── Public API ────────────────────────────────────────────────────────────────

// Initialise NTP (call after WiFi is up). Idempotent.
inline void ledScheduleNtpInit() {
    if (_ledScheduleNtpStarted) return;
    configTime(LED_SCHEDULE_TZ_OFFSET_S, 0, "pool.ntp.org");
    _ledScheduleNtpStarted = true;
    LOG_I("led_sched", "NTP configured (offset %d s) — pool.ntp.org",
          LED_SCHEDULE_TZ_OFFSET_S);
}

// Load saved slots from NVS. Call once during boot.
inline void ledScheduleBegin() {
    _ledScheduleLoadAll();
    int slotsUsed = 0;
    for (uint8_t i = 0; i < LED_SCHEDULE_MAX_SLOTS; i++) {
        if (_ledScheduleSlots[i].id[0] != '\0') slotsUsed++;
    }
    if (slotsUsed > 0) {
        LOG_I("led_sched", "Loaded %d/%d schedule slots from NVS",
              slotsUsed, (int)LED_SCHEDULE_MAX_SLOTS);
    }
}

// Add or replace a slot. Returns true on success.
inline bool ledScheduleAdd(const char* id, uint8_t hour, uint8_t minute,
                           const char* actionJson) {
    if (!_ledScheduleIdOk(id))      return false;
    if (hour >= 24 || minute >= 60) return false;
    if (!actionJson)                return false;
    size_t alen = strlen(actionJson);
    if (alen >= LED_SCHEDULE_ACTION_MAX) return false;
    // Find existing slot for this id, or first empty.
    int8_t replace = -1, empty = -1;
    for (uint8_t i = 0; i < LED_SCHEDULE_MAX_SLOTS; i++) {
        if (_ledScheduleSlots[i].id[0] == '\0' && empty < 0) empty = i;
        if (strcmp(_ledScheduleSlots[i].id, id) == 0) { replace = i; break; }
    }
    int8_t target = (replace >= 0) ? replace : empty;
    if (target < 0) {
        LOG_W("led_sched", "add '%s' refused — all %d slots full",
              id, (int)LED_SCHEDULE_MAX_SLOTS);
        return false;
    }
    LedScheduleSlot& s = _ledScheduleSlots[target];
    strlcpy(s.id, id, sizeof(s.id));
    s.hour   = hour;
    s.minute = minute;
    strlcpy(s.action, actionJson, sizeof(s.action));
    _ledScheduleSaveSlot(target);
    LOG_I("led_sched", "%s '%s' at %02u:%02u",
          (replace >= 0 ? "replaced" : "added"), id, hour, minute);
    return true;
}

// Remove a slot by id. Returns true if found and removed.
inline bool ledScheduleRemove(const char* id) {
    if (!_ledScheduleIdOk(id)) return false;
    for (uint8_t i = 0; i < LED_SCHEDULE_MAX_SLOTS; i++) {
        if (strcmp(_ledScheduleSlots[i].id, id) == 0) {
            memset(&_ledScheduleSlots[i], 0, sizeof(LedScheduleSlot));
            _ledScheduleSaveSlot(i);
            LOG_I("led_sched", "removed '%s'", id);
            return true;
        }
    }
    return false;
}

// Clear all slots.
inline void ledScheduleClear() {
    for (uint8_t i = 0; i < LED_SCHEDULE_MAX_SLOTS; i++) {
        if (_ledScheduleSlots[i].id[0] != '\0') {
            memset(&_ledScheduleSlots[i], 0, sizeof(LedScheduleSlot));
            _ledScheduleSaveSlot(i);
        }
    }
    LOG_I("led_sched", "cleared all slots");
}

// Build a JSON snapshot of all slots — for cmd/led sched_list publish.
// Caller-owned String.
inline String ledScheduleListJson() {
    String out = "{\"slots\":[";
    bool first = true;
    for (uint8_t i = 0; i < LED_SCHEDULE_MAX_SLOTS; i++) {
        const LedScheduleSlot& s = _ledScheduleSlots[i];
        if (s.id[0] == '\0') continue;
        if (!first) out += ",";
        out += "{\"id\":\"";
        out += s.id;
        out += "\",\"hour\":";
        out += s.hour;
        out += ",\"minute\":";
        out += s.minute;
        out += ",\"action\":";
        out += s.action;
        out += "}";
        first = false;
    }
    out += "]}";
    return out;
}

// Tick the schedule. Call from main loop(). Cheap when nothing matches:
// no NTP query, just a single getLocalTime() + minute-change check + 8
// strcmp-free struct compares.
inline void ledScheduleTick() {
    if (!_ledScheduleNtpStarted) return;
    struct tm tm_now;
    if (!getLocalTime(&tm_now, 0)) return;   // 0 ms — non-blocking
    if (tm_now.tm_min == _ledScheduleLastMinuteFired) return;
    int8_t prev = _ledScheduleLastMinuteFired;
    _ledScheduleLastMinuteFired = (int8_t)tm_now.tm_min;
    // Skip the first observation — we don't want to fire schedules
    // for the minute we just landed on if NTP only just synced.
    if (prev < 0) return;
    for (uint8_t i = 0; i < LED_SCHEDULE_MAX_SLOTS; i++) {
        const LedScheduleSlot& s = _ledScheduleSlots[i];
        if (s.id[0] == '\0') continue;
        if (s.hour != (uint8_t)tm_now.tm_hour) continue;
        if (s.minute != (uint8_t)tm_now.tm_min) continue;
        LOG_I("led_sched", "fire '%s' at %02d:%02d",
              s.id, tm_now.tm_hour, tm_now.tm_min);
        // Re-feed the stored action through the cmd/led handler.
        handleLedCommand(s.action, strlen(s.action));
    }
}
