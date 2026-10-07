#include <math.h>
#include <string.h>
#include "aeris/gas_index.h"

/* alpha for an EW filter with time constant tau, sampled every dt. */
static float alpha_for(float tau_s, float dt_s)
{
    if (tau_s <= 0.0f) return 1.0f;
    float a = 1.0f - expf(-dt_s / tau_s);
    if (a < 1e-5f) a = 1e-5f;
    if (a > 1.0f)  a = 1.0f;
    return a;
}

void gas_index_init(gas_index_t *g, uint32_t sample_period_ms,
                    float tau_toward_s, float tau_away_s, float gain)
{
    memset(g, 0, sizeof *g);
    float dt = (float)sample_period_ms / 1000.0f;
    g->alpha_toward   = alpha_for(tau_toward_s, dt);
    g->alpha_away     = alpha_for(tau_away_s,   dt);
    g->gain           = gain;
    /* Two minutes of settling before the index is trusted. */
    g->warmup_samples = (uint16_t)(120000u / (sample_period_ms ? sample_period_ms : 1));
    if (g->warmup_samples < 4) g->warmup_samples = 4;
}

bool gas_index_ready(const gas_index_t *g)
{
    return g->initialised && g->samples >= g->warmup_samples;
}

float gas_index_update(gas_index_t *g, uint16_t sraw)
{
    float x = (float)sraw;

    if (!g->initialised) {
        g->baseline    = x;
        /* A non-zero seed scale; the SGP41's tick noise floor is a few LSB,
         * and starting at 0 would make the first index infinite. */
        g->mad         = 16.0f;
        g->initialised = true;
        g->samples     = 1;
        return GAS_INDEX_NOMINAL;
    }

    float dev = g->baseline - x;          /* positive => gas present */

    /* Deviation away from baseline (gas rising) must not drag the baseline
     * with it, so it adapts on the slow constant. Returning toward baseline
     * is the sensor recovering, which we want to track promptly. */
    float alpha = (dev > 0.0f) ? g->alpha_away : g->alpha_toward;
    g->baseline += alpha * (x - g->baseline);

    float abs_dev = fabsf(dev);
    g->mad += g->alpha_toward * (abs_dev - g->mad);
    if (g->mad < 4.0f) g->mad = 4.0f;     /* floor: keeps the index finite */

    g->samples++;
    if (g->samples < g->warmup_samples) return GAS_INDEX_NOMINAL;

    float index = GAS_INDEX_NOMINAL + g->gain * (dev / g->mad);
    if (index < GAS_INDEX_MIN) index = GAS_INDEX_MIN;
    if (index > GAS_INDEX_MAX) index = GAS_INDEX_MAX;
    return index;
}
