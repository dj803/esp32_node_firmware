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

// Returns true if `candidate` is strictly newer than `installed`.
// Parses MAJOR.MINOR.PATCH numerically so "0.2.15" > "0.2.7" correctly.
// ESP32OTAPull uses String::compareTo() which is lexicographic and breaks on
// double-digit patch/minor numbers — we bypass it by always passing "0.0.0"
// as the installed version and doing our own comparison here.
static bool semverIsNewer(const char* installed, const char* candidate) {
    int iMaj = 0, iMin = 0, iPat = 0;
    int cMaj = 0, cMin = 0, cPat = 0;
    sscanf(installed,  "%d.%d.%d", &iMaj, &iMin, &iPat);
    sscanf(candidate,  "%d.%d.%d", &cMaj, &cMin, &cPat);
    if (cMaj != iMaj) return cMaj > iMaj;
    if (cMin != iMin) return cMin > iMin;
    return cPat > iPat;
}
