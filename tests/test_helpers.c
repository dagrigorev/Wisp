#include "test_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#if defined(_MSC_VER)
#include <crtdbg.h>
#endif

TestCase g_tests[MAX_TESTS];
int      g_test_count = 0;
int      g_pass_count = 0;
int      g_fail_count = 0;

/* Called by C test files via the WISP_TEST_REGISTER macro */
void wisp_register_test(const char *suite, const char *name, TestFn fn) {
    if (g_test_count < MAX_TESTS) {
        g_tests[g_test_count].suite = suite;
        g_tests[g_test_count].name  = name;
        g_tests[g_test_count].fn    = fn;
        g_test_count++;
    } else {
        /* Dropping a test silently would report a partial run as a pass. */
        fprintf(stderr, "MAX_TESTS (%d) exceeded registering %s::%s\n",
                MAX_TESTS, suite, name);
        exit(1);
    }
}

int run_all_tests(void) {
    printf("Running %d test(s)...\n", g_test_count);
    for (int i = 0; i < g_test_count; i++) {
        int fail_before = g_fail_count;
        printf("  [ RUN ] %s::%s\n", g_tests[i].suite, g_tests[i].name);
        g_tests[i].fn();
        if (g_fail_count == fail_before) {
            printf("  [ OK  ] %s::%s\n", g_tests[i].suite, g_tests[i].name);
            g_pass_count++;
        }
    }
    printf("\n%d passed, %d failed\n", g_pass_count, g_fail_count);
    return g_fail_count > 0 ? 1 : 0;
}

int main(void) {
    /* A failing assert must kill the process, not open a window.
     *
     * MSVC's debug CRT answers abort() and a failed assert with a modal
     * dialog that waits for a click. Under ctest nothing clicks it, so the
     * run never finishes and the whole suite looks hung rather than failed
     * -- which is exactly what happened when a restored assert started
     * firing. Send both to stderr and let the process die. */
#if defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    return run_all_tests();
}
