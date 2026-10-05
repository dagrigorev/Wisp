#pragma once
/*
 * test_helpers.h — Minimal unit-test framework for Wisp.
 * MSVC-compatible: uses C++ static-initializer trick for auto-registration.
 * Test files are compiled as C++ via /TP in CMakeLists.
 */
#ifndef WISP_TEST_HELPERS_H
#define WISP_TEST_HELPERS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wisp_bool.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*TestFn)(void);

typedef struct {
    const char *suite;
    const char *name;
    TestFn      fn;
} TestCase;

#define MAX_TESTS 1024
extern TestCase g_tests[MAX_TESTS];
extern int      g_test_count;
extern int      g_pass_count;
extern int      g_fail_count;

void wisp_register_test(const char *suite, const char *name, TestFn fn);
int  run_all_tests(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ── Assertions ─────────────────────────────────────────────────────────── */

#define ASSERT(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "  FAIL  %s:%d: assertion failed: %s\n", \
                __FILE__, __LINE__, #expr); \
        g_fail_count++; return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    long _a = (long)(a), _b = (long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "  FAIL  %s:%d: %s == %s  (%ld != %ld)\n", \
                __FILE__, __LINE__, #a, #b, _a, _b); \
        g_fail_count++; return; \
    } \
} while(0)

#define ASSERT_STR_EQ(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if (!_a || !_b || strcmp(_a, _b) != 0) { \
        fprintf(stderr, "  FAIL  %s:%d: strcmp(%s, %s)  (\"%s\" != \"%s\")\n", \
                __FILE__, __LINE__, #a, #b, _a ? _a : "(null)", _b ? _b : "(null)"); \
        g_fail_count++; return; \
    } \
} while(0)

#define ASSERT_NULL(p)     ASSERT((p) == NULL)
#define ASSERT_NOT_NULL(p) ASSERT((p) != NULL)
#define ASSERT_TRUE(x)     ASSERT(x)
#define ASSERT_FALSE(x)    ASSERT(!(x))

/* ── Auto-registration via C++ static initializer ─────────────────────── */

#ifdef __cplusplus
#define TEST(suite_name, test_name) \
    extern "C" void _test_##suite_name##_##test_name(void); \
    namespace { \
        struct _Reg_##suite_name##_##test_name { \
            _Reg_##suite_name##_##test_name() { \
                wisp_register_test(#suite_name, #test_name, \
                                  _test_##suite_name##_##test_name); \
            } \
        } _reg_##suite_name##_##test_name; \
    } \
    extern "C" void _test_##suite_name##_##test_name(void)
#else
#error "Test files must be compiled as C++ (use /TP flag)"
#endif

#endif /* WISP_TEST_HELPERS_H */
