/* The test harness every package shares: MaryUI's lp_test.h under Mary's own
 * names. Each <pkg>/tests/test_<x>.c defines cases with MARY_TEST(name) and runs
 * them from main() with MARY_RUN(name); the exit status is the number of failed
 * cases. Case names follow the Swift twin's test names where one exists, so the
 * two suites can be read side by side. */
#ifndef MARY_TEST_H
#define MARY_TEST_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int mary_test_failures = 0;
static int mary_test_case_failed = 0;

#define MARY_TEST(name) static void name(void)
#define MARY_RUN(name) do { mary_test_case_failed = 0; name(); \
    printf("%s %s\n", mary_test_case_failed ? "FAIL" : "ok  ", #name); \
    if (mary_test_case_failed) mary_test_failures++; } while (0)
#define MARY_FAIL(fmt, ...) do { mary_test_case_failed = 1; \
    fprintf(stderr, "  %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); } while (0)
#define MARY_ASSERT(cond) do { if (!(cond)) MARY_FAIL("assertion failed: %s", #cond); } while (0)
#define MARY_ASSERT_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) MARY_FAIL("%s = %lld, expected %lld", #a, _a, _b); } while (0)
#define MARY_ASSERT_STR(a, b) do { const char *_a = (a), *_b = (b); \
    if (!_a || !_b || strcmp(_a, _b) != 0) MARY_FAIL("%s = \"%s\", expected \"%s\"", #a, _a ? _a : "(null)", _b ? _b : "(null)"); } while (0)
#define MARY_ASSERT_NEAR(a, b, eps) do { double _a = (a), _b = (b); \
    if (fabs(_a - _b) > (eps)) MARY_FAIL("%s = %g, expected %g (±%g)", #a, _a, _b, (double)(eps)); } while (0)
#define MARY_TEST_MAIN_END() return mary_test_failures

/* A fixture path inside the source tree: `make test` exports MARY_SRC. */
static inline const char *mary_test_path(const char *relative, char *out, size_t n) {
    const char *root = getenv("MARY_SRC");
    snprintf(out, n, "%s/%s", root && *root ? root : ".", relative);
    return out;
}

#endif
