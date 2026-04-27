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
[[maybe_unused]] static bool semverParse(const char* s, int& maj, int& min, int& pat) {
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

// Returns true if the version string has a pre-release suffix such as
// "-dev", "-rc1", "-beta". Convention: any character after the patch number
// that is not '\0' qualifies — this fits the project's local-build style of
// "0.4.17-dev" produced by config.h's fallback FIRMWARE_VERSION literal.
//
// CI builds use FIRMWARE_VERSION_OVERRIDE injected from the git tag (no
// suffix), so a release binary is always plain "MAJOR.MINOR.PATCH".
[[maybe_unused]] static bool semverHasPreReleaseSuffix(const char* s) {
    if (!s) return false;
    int maj = 0, min = 0, pat = 0;
    int consumed = 0;
    int n = sscanf(s, "%d.%d.%d%n", &maj, &min, &pat, &consumed);
    if (n != 3) return false;
    return s[consumed] != '\0';
}

// Parse a 4-component dev version "MAJOR.MINOR.PATCH.DEV" — returns the
// 4th-component DEV value if present, or -1 if the string has only 3
// components (= release version). Uses %n to detect a 4th component
// without consuming further input on failure.
[[maybe_unused]] static int semverDevComponent(const char* s) {
    if (!s) return -1;
    int a = 0, b = 0, c = 0, d = 0;
    int n = sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d);
    return (n == 4) ? d : -1;
}

// Returns true if `candidate` is strictly newer than `installed`.
// Parses MAJOR.MINOR.PATCH numerically so "0.2.15" > "0.2.7" correctly.
// ESP32OTAPull uses String::compareTo() which is lexicographic and breaks on
// double-digit patch/minor numbers — we bypass it by always passing "0.0.0"
// as the installed version and doing our own comparison here.
//
// Pre-release suffix handling (#70, v0.4.18): when MAJOR.MINOR.PATCH match,
// a candidate WITHOUT a pre-release suffix is considered newer than an
// installed version that has one. semver.org § 11 — pre-release < release.
//
// 4-component dev versioning (#70 option B, v0.4.20): "MAJOR.MINOR.PATCH.DEV"
// indicates a local dev build BELOW the same-numbered release. e.g.
// `0.4.20.0` is older than `0.4.20`. The 4th component is independent of any
// `-dev` suffix and orders strictly numerically when both have a 4th
// component. Lets local USB-flashed builds auto-OTA to the matching release.
//
// Comparison rules (numeric parts equal):
//   1. installed has 4th component AND candidate doesn't → candidate newer
//   2. installed clean AND candidate has 4th component   → candidate older
//   3. both have 4th components → numeric compare those
//   4. neither has 4th → fall through to suffix comparison
//
// Tolerant: malformed inputs parse to 0.0.0 rather than being rejected.
static bool semverIsNewer(const char* installed, const char* candidate) {
    int iMaj = 0, iMin = 0, iPat = 0;
    int cMaj = 0, cMin = 0, cPat = 0;
    sscanf(installed,  "%d.%d.%d", &iMaj, &iMin, &iPat);
    sscanf(candidate,  "%d.%d.%d", &cMaj, &cMin, &cPat);
    if (cMaj != iMaj) return cMaj > iMaj;
    if (cMin != iMin) return cMin > iMin;
    if (cPat != iPat) return cPat > iPat;
    // MAJOR.MINOR.PATCH match. Try 4-component dev versioning first.
    int iDev = semverDevComponent(installed);
    int cDev = semverDevComponent(candidate);
    if (iDev >= 0 && cDev < 0)  return true;     // installed=dev, candidate=release → upgrade
    if (iDev < 0  && cDev >= 0) return false;    // installed=release, candidate=dev → no downgrade
    if (iDev >= 0 && cDev >= 0) return cDev > iDev;  // both dev → compare numerically
    // Both 3-component — compare pre-release suffixes per the v0.4.18 fix.
    bool iHasSuffix = semverHasPreReleaseSuffix(installed);
    bool cHasSuffix = semverHasPreReleaseSuffix(candidate);
    return iHasSuffix && !cHasSuffix;
}
