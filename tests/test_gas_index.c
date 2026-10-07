#include "aeris/features.h"
#include "aeris/gas_index.h"
#include "test.h"

void suite_gas_index(void)
{
    SUITE("gas_index");

    gas_index_t g;
    gas_index_init(&g, AERIS_SAMPLE_PERIOD_MS, 600.0f, 43200.0f, 45.0f);

    /* Before warm-up the index must read nominal rather than a wild number
     * derived from two samples. */
    CHECK_NEAR(gas_index_update(&g, 30000), GAS_INDEX_NOMINAL, 0.01f);
    CHECK(!gas_index_ready(&g));

    /* Hold a steady signal through warm-up: the index should settle at nominal
     * and the baseline should equal the signal. */
    float idx = 0.0f;
    for (int i = 0; i < 200; i++) idx = gas_index_update(&g, 30000);
    CHECK(gas_index_ready(&g));
    CHECK_NEAR(idx, GAS_INDEX_NOMINAL, 2.0f);
    CHECK_NEAR(g.baseline, 30000.0f, 5.0f);

    /* A falling raw signal means gas is present, so the index must rise. */
    idx = gas_index_update(&g, 29000);
    CHECK_MSG(idx > GAS_INDEX_NOMINAL + 20.0f,
              "a 1000-tick drop should raise the index well above nominal, got %.1f",
              (double)idx);

    /* Sustained gas must not be absorbed into the baseline within the event.
     * A 20-minute event at 5 s cadence is 240 samples; with a 12 h away-side
     * time constant the index must still be clearly elevated at the end. */
    for (int i = 0; i < 240; i++) idx = gas_index_update(&g, 29000);
    CHECK_MSG(idx > GAS_INDEX_NOMINAL + 10.0f,
              "the baseline absorbed a 20 min event; index fell to %.1f", (double)idx);

    /* Recovery should be prompt once the signal returns. */
    for (int i = 0; i < 300; i++) idx = gas_index_update(&g, 30000);
    CHECK_MSG(idx < GAS_INDEX_NOMINAL + 8.0f,
              "index did not come back down after recovery: %.1f", (double)idx);

    /* The index must stay inside its stated bounds even for absurd inputs. */
    gas_index_t h;
    gas_index_init(&h, AERIS_SAMPLE_PERIOD_MS, 600.0f, 43200.0f, 45.0f);
    for (int i = 0; i < 200; i++) gas_index_update(&h, 30000);
    float lo = gas_index_update(&h, 0);
    float hi = gas_index_update(&h, 65535);
    CHECK(lo <= GAS_INDEX_MAX && lo >= GAS_INDEX_MIN);
    CHECK(hi <= GAS_INDEX_MAX && hi >= GAS_INDEX_MIN);

    /* A constant signal must never produce a divide-by-zero or NaN, even
     * though the deviation is identically zero and the MAD tends to its floor. */
    gas_index_t k;
    gas_index_init(&k, AERIS_SAMPLE_PERIOD_MS, 600.0f, 43200.0f, 45.0f);
    float v = 0.0f;
    for (int i = 0; i < 2000; i++) v = gas_index_update(&k, 25000);
    CHECK_MSG(v == v, "constant input produced NaN");
    CHECK_NEAR(v, GAS_INDEX_NOMINAL, 1.0f);

    /* Changing the sampling period must change alpha, not the time constant:
     * the same wall-clock event should give a similar index either way. */
    gas_index_t slow, fast;
    gas_index_init(&slow, 10000, 600.0f, 43200.0f, 45.0f);
    gas_index_init(&fast,  5000, 600.0f, 43200.0f, 45.0f);
    for (int i = 0; i < 400; i++) { gas_index_update(&slow, 30000); gas_index_update(&fast, 30000); }
    /* 600 s of gas: 60 samples at 10 s, 120 at 5 s. */
    float s_idx = 0.0f, f_idx = 0.0f;
    for (int i = 0; i < 60;  i++) s_idx = gas_index_update(&slow, 29000);
    for (int i = 0; i < 120; i++) f_idx = gas_index_update(&fast, 29000);
    CHECK_MSG(fabs((double)s_idx - (double)f_idx) < 25.0,
              "cadence changed the response: %.1f vs %.1f", (double)s_idx, (double)f_idx);
}
