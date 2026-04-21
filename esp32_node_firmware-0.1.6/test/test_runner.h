#pragma once
// =============================================================================
// test_runner.h  —  Minimal zero-dependency test harness
//
// No external libraries. Compile with any C++11 compiler:
//   g++ -std=c++11 -o run_tests test_all.cpp && ./run_tests
//   cl /std:c++14 test_all.cpp /Fe:run_tests.exe && run_tests.exe  (MSVC)
// =============================================================================

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

static int _tests_run    = 0;
static int _tests_passed = 0;
static int _tests_failed = 0;

#define TEST_ASSERT(cond) do { \
    _tests_run++; \
    if (cond) { \
        _tests_passed++; \
    } else { \
        _tests_failed++; \
        fprintf(stderr, "  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    } \
} while(0)

#define TEST_ASSERT_NEAR(a, b, tol) do { \
    _tests_run++; \
    double _diff = fabs((double)(a) - (double)(b)); \
    if (_diff <= (tol)) { \
        _tests_passed++; \
    } else { \
        _tests_failed++; \
        fprintf(stderr, "  FAIL  %s:%d  |%g - %g| = %g > %g\n", \
                __FILE__, __LINE__, (double)(a), (double)(b), _diff, (double)(tol)); \
    } \
} while(0)

#define RUN_SUITE(name, fn) do { \
    printf("[ %s ]\n", name); \
    fn(); \
} while(0)

static int test_summary() {
    printf("\n%d tests: %d passed, %d failed\n",
           _tests_run, _tests_passed, _tests_failed);
    return (_tests_failed == 0) ? 0 : 1;
}
