/* test.h - a micro test framework. No dependencies, no magic: each suite is a
 * function, tests/main.c lists them, and failures print the file and line.
 */
#ifndef AERIS_TEST_H
#define AERIS_TEST_H

#include <math.h>
#include <stdio.h>
#include <string.h>

extern int g_tests_run, g_tests_failed;
extern const char *g_suite;

#define CHECK(cond) do {                                                      \
    g_tests_run++;                                                            \
    if (!(cond)) {                                                            \
        g_tests_failed++;                                                     \
        printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
    }                                                                         \
} while (0)

#define CHECK_MSG(cond, ...) do {                                             \
    g_tests_run++;                                                            \
    if (!(cond)) {                                                            \
        g_tests_failed++;                                                      \
        printf("  FAIL %s:%d  ", __FILE__, __LINE__);                         \
        printf(__VA_ARGS__);                                                  \
        printf("\n");                                                         \
    }                                                                         \
} while (0)

#define CHECK_EQ_I(a, b) do {                                                 \
    long _a = (long)(a), _b = (long)(b);                                      \
    CHECK_MSG(_a == _b, "%s == %s  (%ld vs %ld)", #a, #b, _a, _b);            \
} while (0)

#define CHECK_NEAR(a, b, tol) do {                                            \
    double _a = (double)(a), _b = (double)(b);                                \
    CHECK_MSG(fabs(_a - _b) <= (tol), "%s ~= %s  (%g vs %g, tol %g)",         \
              #a, #b, _a, _b, (double)(tol));                                 \
} while (0)

#define CHECK_STR(a, b) do {                                                  \
    const char *_a = (a), *_b = (b);                                          \
    CHECK_MSG(_a && _b && !strcmp(_a, _b), "%s == %s  (\"%s\" vs \"%s\")",    \
              #a, #b, _a ? _a : "(null)", _b ? _b : "(null)");               \
} while (0)

#define SUITE(name) do { g_suite = name; printf("%s\n", name); } while (0)

void suite_crc8(void);
void suite_scd40(void);
void suite_sgp41(void);
void suite_sht31(void);
void suite_pmsa003i(void);
void suite_gas_index(void);
void suite_features(void);
void suite_json(void);
void suite_api(void);
void suite_history(void);
void suite_classifier(void);
void suite_aqi(void);
void suite_model(void);
void suite_integration(void);

#endif /* AERIS_TEST_H */
