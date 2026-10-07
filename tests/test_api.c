#include <stdio.h>
#include <string.h>
#include "aeris/api.h"
#include "harness.h"
#include "json_check.h"
#include "test.h"

/* Find "key": in a flat-enough document and return the char after the colon.
 * Enough for spot-checking values without pulling in a real JSON parser. */
static const char *field(const char *doc, const char *key)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(doc, pat);
    return p ? p + strlen(pat) : NULL;
}

void suite_api(void)
{
    SUITE("api");

    /* Drive the real app far enough to have readings, history and features. */
    harness_t h;
    harness_init(&h, 0x5150u);
    scenario_clear(&h.sc);
    scenario_add(&h.sc, EV_OCCUPANCY, 40u * 60000u);
    scenario_add(&h.sc, EV_COOKING,   20u * 60000u);
    while (harness_tick(&h)) { }

    aeris_snapshot_t s;
    aeris_app_snapshot(&h.app, &s);
    CHECK(s.features_valid);

    static char buf[AERIS_API_HISTORY_BUF];

    /* ---- /api/live --------------------------------------------------- */
    size_t n = aeris_api_live(&s, buf, sizeof buf);
    CHECK_MSG(n > 0, "live response overflowed its buffer");
    CHECK_MSG(json_check(buf) < 0, "live response is not valid JSON at offset %ld\n%s",
              json_check(buf), buf);
    CHECK(strstr(buf, "\"co2_ppm\"") != NULL);
    CHECK(strstr(buf, "\"air_quality\"") != NULL);
    CHECK(strstr(buf, "\"advice\"") != NULL);
    CHECK(strstr(buf, "\"probabilities\"") != NULL);
    /* The declared buffer size must leave real headroom, since the firmware
     * sizes its stack allocation from it. */
    CHECK_MSG(n < AERIS_API_LIVE_BUF * 9 / 10,
              "live response %zu B is too close to its %d B budget",
              n, AERIS_API_LIVE_BUF);
    /* Every class must appear in the probability object. */
    for (int c = 0; c < AERIS_N_CLASSES; c++) {
        char pat[64];
        snprintf(pat, sizeof pat, "\"%s\":", aeris_class_name(c));
        CHECK_MSG(strstr(buf, pat) != NULL, "class %s missing from /api/live",
                  aeris_class_name(c));
    }

    /* ---- /api/history ------------------------------------------------ */
    n = aeris_api_history(&h.app.hist, buf, sizeof buf);
    CHECK_MSG(n > 0, "history response overflowed its %d B buffer",
              AERIS_API_HISTORY_BUF);
    CHECK_MSG(json_check(buf) < 0, "history response is not valid JSON at offset %ld",
              json_check(buf));
    CHECK(strstr(buf, "\"period_s\":30") != NULL);
    const char *cnt = field(buf, "count");
    CHECK(cnt != NULL);
    CHECK(h.app.hist.count > 50);

    /* A completely full ring is the worst case for the buffer; make sure the
     * budget covers it rather than only covering today's partial run. */
    aeris_history_t full;
    aeris_history_init(&full);
    for (int i = 0; i < AERIS_HISTORY_SLOTS + 10; i++) {
        aeris_history_maybe_push(&full, (uint32_t)i * AERIS_HISTORY_PERIOD_MS + 1,
                                 65535.0f, 500.0f, 500.0f, 999.9f, -40.0f, 100, 4);
    }
    CHECK_EQ_I(full.count, AERIS_HISTORY_SLOTS);
    n = aeris_api_history(&full, buf, sizeof buf);
    CHECK_MSG(n > 0, "a full history ring of worst-case values overflows the buffer");
    CHECK_MSG(json_check(buf) < 0, "full-ring history is not valid JSON");
    CHECK_MSG(n < AERIS_API_HISTORY_BUF * 9 / 10,
              "worst-case history is %zu B against a %d B budget",
              n, AERIS_API_HISTORY_BUF);

    /* An empty ring must still be valid JSON with empty arrays. */
    aeris_history_t empty;
    aeris_history_init(&empty);
    n = aeris_api_history(&empty, buf, sizeof buf);
    CHECK(n > 0);
    CHECK_MSG(json_check(buf) < 0, "empty history is not valid JSON");
    CHECK(strstr(buf, "\"count\":0") != NULL);
    CHECK(strstr(buf, "\"co2\":[]") != NULL);

    /* ---- /api/status ------------------------------------------------- */
    n = aeris_api_status(&s, &h.app, "aeris-test", buf, sizeof buf);
    CHECK(n > 0);
    CHECK_MSG(json_check(buf) < 0, "status response is not valid JSON at offset %ld",
              json_check(buf));
    CHECK(strstr(buf, "\"firmware\":\"aeris-test\"") != NULL);
    CHECK(strstr(buf, "\"scd40\"") != NULL);
    CHECK(strstr(buf, "\"pmsa003i\"") != NULL);
    CHECK(strstr(buf, "\"param_bytes\"") != NULL);
    CHECK_MSG(n < AERIS_API_STATUS_BUF * 9 / 10,
              "status response %zu B against a %d B budget", n, AERIS_API_STATUS_BUF);

    /* ---- /api/debug -------------------------------------------------- */
    n = aeris_api_debug(&s, buf, sizeof buf);
    CHECK(n > 0);
    CHECK_MSG(json_check(buf) < 0, "debug response is not valid JSON at offset %ld",
              json_check(buf));
    for (int i = 0; i < AERIS_N_FEATURES; i++) {
        char pat[64];
        snprintf(pat, sizeof pat, "\"%s\":", aeris_feature_name(i));
        CHECK_MSG(strstr(buf, pat) != NULL, "feature %s missing from /api/debug",
                  aeris_feature_name(i));
    }
    CHECK_MSG(n < AERIS_API_DEBUG_BUF * 9 / 10,
              "debug response %zu B against a %d B budget", n, AERIS_API_DEBUG_BUF);

    /* ---- a too-small buffer must fail cleanly, not emit broken JSON --- */
    char small[64];
    CHECK_EQ_I(aeris_api_live(&s, small, sizeof small), 0);
    CHECK_EQ_I(aeris_api_history(&h.app.hist, small, sizeof small), 0);
    CHECK_EQ_I(aeris_api_status(&s, &h.app, "x", small, sizeof small), 0);
    CHECK_EQ_I(aeris_api_debug(&s, small, sizeof small), 0);

    /* ---- a fresh device, before any reading, must still serve valid JSON */
    harness_t fresh;
    harness_init(&fresh, 1);
    aeris_snapshot_t s0;
    aeris_app_snapshot(&fresh.app, &s0);
    n = aeris_api_live(&s0, buf, sizeof buf);
    CHECK(n > 0);
    CHECK_MSG(json_check(buf) < 0, "pre-boot live response is not valid JSON");
    /* NaN readings must be null, which is what the dashboard expects. */
    CHECK(strstr(buf, "\"co2_ppm\":null") != NULL);
}
