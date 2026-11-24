#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            g_failures++;                                                              \
            (void)printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                              \
    } while (0)

#define RUN(test)                                                                      \
    do {                                                                               \
        const int before = g_failures;                                                 \
        test();                                                                        \
        (void)printf("%-44s %s\n", #test, (g_failures == before) ? "ok" : "FAILED");   \
    } while (0)

#endif /* TEST_UTIL_H */
