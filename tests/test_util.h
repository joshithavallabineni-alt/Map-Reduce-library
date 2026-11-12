/*
 * test_util.h - tiny helpers shared by all test programs.
 *
 *   CHECK(description, condition)          -> PASS if the condition is true
 *   CHECK_EQ(description, expected, actual) -> PASS if the two numbers are equal
 *                                              (always prints both values)
 *   TEST_SECTION(title)                     -> prints a heading
 *   TEST_FINISH()                           -> prints the summary; use as the
 *                                              last line:  return TEST_FINISH();
 *
 * The program's exit code is 0 when every check passed, 1 otherwise.
 * Include this header in exactly one .c file per program.
 */
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>

static int tests_run = 0;
static int tests_failed = 0;

#define TEST_SECTION(title) printf("\n-- %s\n", title)

#define CHECK(description, condition)                                        \
    do {                                                                     \
        tests_run++;                                                         \
        if (condition) {                                                     \
            printf("  [PASS] %s\n", description);                            \
        } else {                                                             \
            printf("  [FAIL] %s  (%s, line %d)\n", description, __FILE__,    \
                   __LINE__);                                                \
            tests_failed++;                                                  \
        }                                                                    \
    } while (0)

#define CHECK_EQ(description, expected, actual)                              \
    do {                                                                     \
        long long expected_value = (long long)(expected);                    \
        long long actual_value = (long long)(actual);                        \
        tests_run++;                                                         \
        if (expected_value == actual_value) {                                \
            printf("  [PASS] %s (expected %lld, got %lld)\n", description,    \
                   expected_value, actual_value);                            \
        } else {                                                             \
            printf("  [FAIL] %s (expected %lld, got %lld) (%s, line %d)\n",   \
                   description, expected_value, actual_value, __FILE__,      \
                   __LINE__);                                                \
            tests_failed++;                                                  \
        }                                                                    \
    } while (0)

#define TEST_FINISH()                                                        \
    (printf("\n%d of %d checks passed\n", tests_run - tests_failed,           \
            tests_run),                                                      \
     tests_failed == 0 ? 0 : 1)

#endif /* TEST_UTIL_H */
