#include <math.h>
#include <string.h>
#include "aeris/features.h"
#include "test.h"

static aeris_obs_t mk(uint32_t t_ms, float co2, float temp, float rh)
{
    aeris_obs_t o;
    memset(&o, 0, sizeof o);
    o.t_ms = t_ms;
    o.co2_ppm = co2;
    o.voc_index = 100.0f;
    o.nox_index = 100.0f;
    o.temperature_c = temp;
    o.humidity_rh = rh;
    o.abs_humidity_gm3 = aeris_absolute_humidity(temp, rh);
    o.pm2_5_ugm3 = 5.0f;
    o.valid = true;
    return o;
}

void suite_features(void)
{
    SUITE("features");

    /* ---- absolute humidity against published values ------------------ */
    /* 20 C / 50 %RH is 8.65 g/m3; 25 C / 60 %RH is 13.8 g/m3. */
    CHECK_NEAR(aeris_absolute_humidity(20.0f, 50.0f),  8.65f, 0.15f);
    CHECK_NEAR(aeris_absolute_humidity(25.0f, 60.0f), 13.80f, 0.20f);
    CHECK_NEAR(aeris_absolute_humidity(0.0f,  100.0f), 4.85f, 0.15f);
    CHECK_NEAR(aeris_absolute_humidity(20.0f,  0.0f),  0.0f,  0.01f);

    /* The physical point of the feature: cold outdoor air at high RH holds
     * less water than warm indoor air at moderate RH, so opening a window
     * lowers absolute humidity while raising relative humidity. */
    CHECK(aeris_absolute_humidity(11.0f, 72.0f) <
          aeris_absolute_humidity(21.5f, 44.0f));

    /* ---- ring buffer semantics --------------------------------------- */
    aeris_window_t w;
    aeris_window_init(&w);
    CHECK(aeris_window_latest(&w) == NULL);
    CHECK(!aeris_window_full_enough(&w));

    for (int i = 0; i < 5; i++) {
        aeris_obs_t o = mk((uint32_t)i * 5000u, 400.0f + (float)i, 21.0f, 45.0f);
        aeris_window_push(&w, &o);
    }
    CHECK_EQ_I(w.count, 5);
    CHECK_NEAR(aeris_window_at(&w, 0)->co2_ppm, 404.0f, 0.001f);  /* newest */
    CHECK_NEAR(aeris_window_at(&w, 4)->co2_ppm, 400.0f, 0.001f);  /* oldest */
    CHECK(aeris_window_at(&w, 5) == NULL);

    /* Overfilling must drop the oldest, not corrupt the order. */
    aeris_window_init(&w);
    for (int i = 0; i < AERIS_WINDOW_SLOTS + 20; i++) {
        aeris_obs_t o = mk((uint32_t)i * 5000u, (float)i, 21.0f, 45.0f);
        aeris_window_push(&w, &o);
    }
    CHECK_EQ_I(w.count, AERIS_WINDOW_SLOTS);
    CHECK_NEAR(aeris_window_at(&w, 0)->co2_ppm,
               (float)(AERIS_WINDOW_SLOTS + 19), 0.001f);
    CHECK_NEAR(aeris_window_at(&w, AERIS_WINDOW_SLOTS - 1)->co2_ppm,
               (float)20, 0.001f);

    /* ---- slopes ------------------------------------------------------ */
    /* A known ramp: +10 ppm per 5 s sample is +120 ppm/min. */
    aeris_window_init(&w);
    for (int i = 0; i < AERIS_WINDOW_SLOTS; i++) {
        aeris_obs_t o = mk((uint32_t)i * 5000u, 400.0f + 10.0f * (float)i, 21.0f, 45.0f);
        aeris_window_push(&w, &o);
    }
    float f[AERIS_N_FEATURES];
    CHECK(aeris_extract_features(&w, f));
    CHECK_NEAR(f[1], 120.0f, 0.5f);     /* co2_slope_1min */
    CHECK_NEAR(f[2], 120.0f, 0.5f);     /* co2_slope_5min */
    CHECK_NEAR(f[0], 400.0f + 10.0f * (float)(AERIS_WINDOW_SLOTS - 1), 0.5f);

    /* A flat signal has zero slope and zero variance. */
    aeris_window_init(&w);
    for (int i = 0; i < AERIS_WINDOW_SLOTS; i++) {
        aeris_obs_t o = mk((uint32_t)i * 5000u, 650.0f, 21.0f, 45.0f);
        aeris_window_push(&w, &o);
    }
    CHECK(aeris_extract_features(&w, f));
    CHECK_NEAR(f[1], 0.0f, 0.001f);
    CHECK_NEAR(f[3], 0.0f, 0.001f);     /* co2_sd_1min */

    /* Feature names must cover the vector exactly, with no gaps -- the
     * training exporter keys off these. */
    for (int i = 0; i < AERIS_N_FEATURES; i++) {
        CHECK(strcmp(aeris_feature_name(i), "?") != 0);
        CHECK(strlen(aeris_feature_name(i)) > 2);
    }
    CHECK_STR(aeris_feature_name(-1), "?");
    CHECK_STR(aeris_feature_name(AERIS_N_FEATURES), "?");
    /* No duplicates. */
    for (int i = 0; i < AERIS_N_FEATURES; i++) {
        for (int j = i + 1; j < AERIS_N_FEATURES; j++) {
            CHECK_MSG(strcmp(aeris_feature_name(i), aeris_feature_name(j)) != 0,
                      "duplicate feature name at %d and %d: %s",
                      i, j, aeris_feature_name(i));
        }
    }

    /* Too little history must refuse rather than return noise. */
    aeris_window_init(&w);
    for (int i = 0; i < 10; i++) {
        aeris_obs_t o = mk((uint32_t)i * 5000u, 500.0f, 21.0f, 45.0f);
        aeris_window_push(&w, &o);
    }
    CHECK(!aeris_extract_features(&w, f));

    /* An invalid newest sample must also refuse: the alternative is feeding
     * the model a feature vector built on a failed read. */
    aeris_window_init(&w);
    for (int i = 0; i < AERIS_WINDOW_SLOTS; i++) {
        aeris_obs_t o = mk((uint32_t)i * 5000u, 500.0f, 21.0f, 45.0f);
        if (i == AERIS_WINDOW_SLOTS - 1) o.valid = false;
        aeris_window_push(&w, &o);
    }
    CHECK(!aeris_extract_features(&w, f));
}
