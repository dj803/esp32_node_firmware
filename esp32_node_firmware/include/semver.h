#pragma once

// =============================================================================
// semver.h  —  Minimal semantic-version comparison utility
//
// Dependency-free (only <stdio.h>) — safe to include in both firmware and
// host-side Unity tests without any Arduino or FreeRTOS headers.
//
// Used by:
//   ota.h      — compares installed vs candidate firmware versions
//   test_main  — semverIsNewer() unit tests
// =============================================================================

#include <stdio.h>

// Strict parser: returns true only if `s` matches "<uint>.<uint>.<uint>" with
// all three components present. Writes parsed components into maj/min/pat on
// success; leaves them untouched on failure.
//
// Callers that need to reject malformed input (e.g. strict validators) should
// use this overload. The legacy semverIsNewer() tolerates malformed input and
// treats missing components as 0 — useful when OTA_JSON contains partial data,
// but unacceptable for security-sensitive flows.
static bool semverParse(const char* s, int& maj, int& min, int& pat) {
    if (!s) return false;
    int a = 0, b = 0, c = 0;
    int consumed = 0;
    int n = sscanf(s, "%d.%d.%d%n", &a, &b, &c, &consumed);
    if (n != 3) return false;
    // Reject negative components and trailing garbage.
    if (a < 0 || b < 0 || c < 0) return false;
    if (s[consumed] != '\0') return false;
    maj = a; min = b; pat = c;
    return true;
}

// Returns true if `candidate` is strictly newer than `installed`.
// Parses MAJOR.MINOR.PATCH numerically so "0.2.15" > "0.2.7" correctly.
// ESP32OTAPull uses String::compareTo() which is lexicographic and breaks on
// double-digit patch/minor numbers — we bypass it by always passing "0.0.0"
// as the installed version and doing our own comparison here.
//
// Tolerant: malformed inputs parse to 0.0.0 rather than being rejected — this
// mirrors the legacy behavior and is relied on by the OTA retry path. Use
// semverParse() when strictness is required.
static bool semverIsNewer(const char* installed, const char* candidate) {
    int iMaj = 0, iMin = 0, iPat = 0;
    int cMaj = 0, cMin = 0, cPat = 0;
    sscanf(installed,  "%d.%d.%d", &iMaj, &iMin, &iPat);
    sscanf(candidate,  "%d.%d.%d", &cMaj, &cMin, &cPat);
    if (cMaj != iMaj) return cMaj > iMaj;
    if (cMin != iMin) return cMin > iMin;
    return cPat > iPat;
}
