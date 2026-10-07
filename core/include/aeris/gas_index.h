/* gas_index.h - raw SGP41 ticks -> a human-scaled gas index.
 *
 * This is my own implementation, not Sensirion's Gas Index Algorithm. The
 * reason to write one at all: SRAW ticks are only meaningful relative to the
 * sensor's own slow-drifting baseline, so a dashboard that plots raw ticks
 * shows sensor ageing, not air quality.
 *
 * Model: track the baseline of the raw signal with an exponentially-weighted
 * mean and an EW mean-absolute-deviation for scale. Both adapt asymmetrically
 * -- fast when the signal returns toward baseline, slow when it runs away
 * from it -- so a 30-minute cooking event does not get absorbed into the
 * baseline and erased.
 *
 * Sign convention from the SGP41 datasheet: the raw signal *falls* as the
 * target gas concentration rises, so index = 100 + gain * (baseline - sraw).
 * 100 is "typical clean indoor air" on both channels.
 */
#ifndef AERIS_GAS_INDEX_H
#define AERIS_GAS_INDEX_H

#include <stdbool.h>
#include <stdint.h>

#define GAS_INDEX_MIN      1.0f
#define GAS_INDEX_MAX    500.0f
#define GAS_INDEX_NOMINAL 100.0f

typedef struct {
    float    baseline;        /* EW mean of sraw                           */
    float    mad;             /* EW mean absolute deviation (scale)        */
    float    gain;            /* index units per MAD of deviation          */
    float    alpha_toward;    /* EW rate when sample moves toward baseline */
    float    alpha_away;      /* EW rate when sample runs away from it     */
    uint32_t samples;
    uint16_t warmup_samples;  /* below this, output is pinned to nominal   */
    bool     initialised;
} gas_index_t;

/* sample_period_ms lets the time constants be expressed in seconds rather
 * than in samples, so changing the sampling cadence does not silently change
 * how fast the baseline moves. */
void  gas_index_init(gas_index_t *g, uint32_t sample_period_ms,
                     float tau_toward_s, float tau_away_s, float gain);
float gas_index_update(gas_index_t *g, uint16_t sraw);
bool  gas_index_ready(const gas_index_t *g);

#endif /* AERIS_GAS_INDEX_H */
