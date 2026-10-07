#include <string.h>
#include "aeris/aqi.h"
#include "test.h"

void suite_aqi(void)
{
    SUITE("aqi");

    /* Clean air on every channel. */
    aeris_aqi_t a = aeris_aqi_compute(420.0f, 2.0f, 100.0f, 100.0f);
    CHECK(a.score >= 85);
    CHECK_EQ_I(a.band, AERIS_BAND_GOOD);

    /* The ASHRAE 1000 ppm comfort ceiling should land in "fair". */
    a = aeris_aqi_compute(1000.0f, 2.0f, 100.0f, 100.0f);
    CHECK_EQ_I(a.band, AERIS_BAND_FAIR);
    CHECK_STR(a.driver, "co2");

    /* Worst channel wins: perfect CO2 does not excuse heavy smoke. */
    a = aeris_aqi_compute(430.0f, 80.0f, 100.0f, 100.0f);
    CHECK_STR(a.driver, "pm2_5");
    CHECK(a.band == AERIS_BAND_POOR || a.band == AERIS_BAND_BAD);

    a = aeris_aqi_compute(430.0f, 2.0f, 400.0f, 100.0f);
    CHECK_STR(a.driver, "voc");
    a = aeris_aqi_compute(430.0f, 2.0f, 100.0f, 400.0f);
    CHECK_STR(a.driver, "nox");

    /* Monotonic in CO2: more is never scored better. */
    uint8_t prev = 255;
    for (float co2 = 400.0f; co2 <= 5000.0f; co2 += 50.0f) {
        aeris_aqi_t x = aeris_aqi_compute(co2, 1.0f, 100.0f, 100.0f);
        CHECK_MSG(x.score <= prev, "score rose as CO2 went up at %.0f ppm", (double)co2);
        prev = x.score;
    }

    /* Monotonic in PM2.5. */
    prev = 255;
    for (float pm = 0.0f; pm <= 200.0f; pm += 2.0f) {
        aeris_aqi_t x = aeris_aqi_compute(420.0f, pm, 100.0f, 100.0f);
        CHECK(x.score <= prev);
        prev = x.score;
    }

    /* Beyond the last breakpoint the score must pin at 0, not go negative or
     * wrap through the uint8. */
    a = aeris_aqi_compute(50000.0f, 5000.0f, 500.0f, 500.0f);
    CHECK_EQ_I(a.score, 0);
    CHECK_EQ_I(a.band, AERIS_BAND_BAD);

    /* Below the first breakpoint the score must pin at 100. */
    a = aeris_aqi_compute(300.0f, 0.0f, 0.0f, 0.0f);
    CHECK_EQ_I(a.score, 100);

    CHECK_STR(aeris_band_name(AERIS_BAND_GOOD), "good");
    CHECK_STR(aeris_band_name(AERIS_BAND_BAD),  "bad");
}
