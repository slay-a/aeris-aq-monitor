#include "aeris/history.h"
#include "test.h"

void suite_history(void)
{
    SUITE("history");

    aeris_history_t h;
    aeris_history_init(&h);
    CHECK_EQ_I(h.count, 0);
    CHECK(aeris_history_get(&h, 0) == NULL);

    /* The first sample stores immediately; the next must wait out the period. */
    CHECK(aeris_history_maybe_push(&h, 1000, 500, 100, 100, 5, 21.5f, 45, 0));
    CHECK_EQ_I(h.count, 1);
    CHECK(!aeris_history_maybe_push(&h, 2000, 510, 100, 100, 5, 21.5f, 45, 0));
    CHECK_EQ_I(h.count, 1);
    CHECK(aeris_history_maybe_push(&h, 1000 + AERIS_HISTORY_PERIOD_MS,
                                   520, 100, 100, 5, 21.5f, 45, 1));
    CHECK_EQ_I(h.count, 2);

    /* Quantization round trips within the stated resolution. */
    const aeris_hist_rec_t *r = aeris_history_get(&h, 0);
    CHECK_EQ_I(r->co2, 500);
    CHECK_EQ_I(r->temp_cx10, 215);
    CHECK_EQ_I(r->rh, 45);
    CHECK_EQ_I(r->pm2_5_x10, 50);
    CHECK_EQ_I(r->t_s, 1);

    /* Oldest-first ordering across a wrap. */
    aeris_history_init(&h);
    for (int i = 0; i < AERIS_HISTORY_SLOTS + 30; i++) {
        uint32_t t = (uint32_t)i * AERIS_HISTORY_PERIOD_MS + 1;
        CHECK(aeris_history_maybe_push(&h, t, (float)(400 + i), 100, 100, 1,
                                      20.0f, 50, 0));
    }
    CHECK_EQ_I(h.count, AERIS_HISTORY_SLOTS);
    const aeris_hist_rec_t *oldest = aeris_history_get(&h, 0);
    const aeris_hist_rec_t *newest = aeris_history_get(&h, AERIS_HISTORY_SLOTS - 1);
    CHECK_EQ_I(oldest->co2, 400 + 30);
    CHECK_EQ_I(newest->co2, 400 + AERIS_HISTORY_SLOTS + 29);
    CHECK(aeris_history_get(&h, AERIS_HISTORY_SLOTS) == NULL);
    /* Timestamps must be monotonically increasing in iteration order. */
    uint32_t prev = 0;
    for (uint16_t i = 0; i < h.count; i++) {
        const aeris_hist_rec_t *x = aeris_history_get(&h, i);
        CHECK(x->t_s >= prev);
        prev = x->t_s;
    }

    /* Out-of-range and NaN inputs must clamp, not wrap into a spike on the
     * chart. */
    aeris_history_init(&h);
    CHECK(aeris_history_maybe_push(&h, 1, 99999.0f, 99999.0f, -5.0f, 1e9f,
                                   -300.0f, 250.0f, 3));
    r = aeris_history_get(&h, 0);
    CHECK_EQ_I(r->co2, 65535);
    CHECK_EQ_I(r->nox, 0);
    CHECK_EQ_I(r->pm2_5_x10, 65535);
    CHECK_EQ_I(r->temp_cx10, -3000);     /* -300 C scales to -3000, in range */
    CHECK_EQ_I(r->rh, 100);

    /* A value that genuinely exceeds int16 must clamp rather than wrap into a
     * positive spike on the chart. */
    aeris_history_init(&h);
    CHECK(aeris_history_maybe_push(&h, 1, 500, 100, 100, 5, -9000.0f, 50, 0));
    CHECK_EQ_I(aeris_history_get(&h, 0)->temp_cx10, -32768);
    aeris_history_init(&h);
    CHECK(aeris_history_maybe_push(&h, 1, 500, 100, 100, 5, 9000.0f, 50, 0));
    CHECK_EQ_I(aeris_history_get(&h, 0)->temp_cx10, 32767);

    aeris_history_init(&h);
    float nan_f = nanf("");
    CHECK(aeris_history_maybe_push(&h, 1, nan_f, nan_f, nan_f, nan_f,
                                   nan_f, nan_f, 0));
    r = aeris_history_get(&h, 0);
    CHECK_EQ_I(r->co2, 0);
    CHECK_EQ_I(r->temp_cx10, 0);
    CHECK_EQ_I(r->rh, 0);
}
