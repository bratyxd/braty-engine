/* br_test.h - the whole test framework: BR_CHECK and a failure count.
 *
 *   int main(void) { BR_CHECK(1 + 1 == 2); return br_test_finish(); }
 */
#ifndef BR_TEST_H
#define BR_TEST_H

#include <stdio.h>

static int br_test_failures = 0;
static int br_test_checks = 0;

static void br_test_check(int ok, const char *expr, const char *file, int line)
{
    br_test_checks++;
    if (!ok) {
        br_test_failures++;
        printf("%s:%d: CHECK FAILED: %s\n", file, line, expr);
    }
}

/* Records a failure (with file and line) when cond is false, and keeps going. */
#define BR_CHECK(cond) br_test_check((cond) ? 1 : 0, #cond, __FILE__, __LINE__)

/* Prints a summary; returns the process exit code (0 = all passed). */
static int br_test_finish(void)
{
    printf("%d checks, %d failures\n", br_test_checks, br_test_failures);
    return br_test_failures == 0 ? 0 : 1;
}

#endif
