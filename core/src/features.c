#include <math.h>
#include <string.h>
#include "aeris/features.h"

/* Slope horizons in samples at AERIS_SAMPLE_PERIOD_MS. */
#define LAG_1MIN  (60000u  / AERIS_SAMPLE_PERIOD_MS)   /* 12 */
#define LAG_5MIN  (300000u / AERIS_SAMPLE_PERIOD_MS)   /* 60 */

void aeris_window_init(aeris_window_t *w)
{
    memset(w, 0, sizeof *w);
}

void aeris_window_push(aeris_window_t *w, const aeris_obs_t *o)
{
    w->slot[w->head] = *o;
    w->head = (uint16_t)((w->head + 1u) % AERIS_WINDOW_SLOTS);
    if (w->count < AERIS_WINDOW_SLOTS) w->count++;
}

const aeris_obs_t *aeris_window_at(const aeris_window_t *w, uint16_t age_back)
{
    if (age_back >= w->count) return NULL;
    uint16_t idx = (uint16_t)((w->head + AERIS_WINDOW_SLOTS - 1u - age_back)
                              % AERIS_WINDOW_SLOTS);
    return &w->slot[idx];
}

const aeris_obs_t *aeris_window_latest(const aeris_window_t *w)
{
    return aeris_window_at(w, 0);
}

bool aeris_window_full_enough(const aeris_window_t *w)
{
    return w->count > LAG_5MIN;
}

float aeris_absolute_humidity(float temp_c, float rh_percent)
{
    /* Magnus saturation vapour pressure (hPa), then the ideal-gas form of
     * absolute humidity. Constants are the Magnus coefficients for water
     * over the 0-60 C range we care about. */
    float es = 6.112f * expf((17.67f * temp_c) / (temp_c + 243.5f));
    float e  = es * (rh_percent / 100.0f);
    return 216.7f * (e / (temp_c + 273.15f));
}

/* Mean of a field over the most recent n samples. */
static float mean_of(const aeris_window_t *w, uint16_t n,
                     float (*get)(const aeris_obs_t *))
{
    if (n > w->count) n = w->count;
    if (n == 0) return 0.0f;
    float acc = 0.0f;
    uint16_t used = 0;
    for (uint16_t i = 0; i < n; i++) {
        const aeris_obs_t *o = aeris_window_at(w, i);
        if (!o || !o->valid) continue;
        acc += get(o);
        used++;
    }
    return used ? acc / (float)used : 0.0f;
}

/* Difference between now and lag samples ago, expressed per minute so the
 * number stays interpretable if the cadence ever changes. */
static float slope_per_min(const aeris_window_t *w, uint16_t lag,
                           float (*get)(const aeris_obs_t *))
{
    const aeris_obs_t *now = aeris_window_at(w, 0);
    const aeris_obs_t *old = aeris_window_at(w, lag);
    if (!now || !old || !now->valid || !old->valid) return 0.0f;
    float minutes = (float)lag * (float)AERIS_SAMPLE_PERIOD_MS / 60000.0f;
    if (minutes <= 0.0f) return 0.0f;
    return (get(now) - get(old)) / minutes;
}

/* Sample standard deviation over the most recent n samples. Short-horizon
 * CO2 variance is what separates "two people talking" from "door left open":
 * a steady source wobbles, a draught does not. */
static float stddev_of(const aeris_window_t *w, uint16_t n,
                       float (*get)(const aeris_obs_t *))
{
    if (n > w->count) n = w->count;
    if (n < 2) return 0.0f;
    float mu = mean_of(w, n, get);
    float acc = 0.0f;
    uint16_t used = 0;
    for (uint16_t i = 0; i < n; i++) {
        const aeris_obs_t *o = aeris_window_at(w, i);
        if (!o || !o->valid) continue;
        float d = get(o) - mu;
        acc += d * d;
        used++;
    }
    if (used < 2) return 0.0f;
    return sqrtf(acc / (float)(used - 1));
}

static float f_co2(const aeris_obs_t *o) { return o->co2_ppm; }
static float f_voc(const aeris_obs_t *o) { return o->voc_index; }
static float f_tmp(const aeris_obs_t *o) { return o->temperature_c; }
static float f_ah (const aeris_obs_t *o) { return o->abs_humidity_gm3; }
static float f_pm (const aeris_obs_t *o) { return o->pm2_5_ugm3; }

static const char *const k_names[AERIS_N_FEATURES] = {
    "co2_ppm",          "co2_slope_1min",   "co2_slope_5min",  "co2_sd_1min",
    "voc_index",        "voc_slope_1min",   "voc_slope_5min",
    "nox_index",
    "temp_c",           "temp_slope_5min",
    "rh_pct",           "abs_humidity",     "ah_slope_5min",
    "pm2_5",
};

const char *aeris_feature_name(int i)
{
    if (i < 0 || i >= AERIS_N_FEATURES) return "?";
    return k_names[i];
}

bool aeris_extract_features(const aeris_window_t *w, float *out)
{
    if (!aeris_window_full_enough(w)) return false;
    const aeris_obs_t *now = aeris_window_latest(w);
    if (!now || !now->valid) return false;

    out[0]  = now->co2_ppm;
    out[1]  = slope_per_min(w, LAG_1MIN, f_co2);
    out[2]  = slope_per_min(w, LAG_5MIN, f_co2);
    out[3]  = stddev_of(w, LAG_1MIN, f_co2);
    out[4]  = now->voc_index;
    out[5]  = slope_per_min(w, LAG_1MIN, f_voc);
    out[6]  = slope_per_min(w, LAG_5MIN, f_voc);
    out[7]  = now->nox_index;
    out[8]  = now->temperature_c;
    out[9]  = slope_per_min(w, LAG_5MIN, f_tmp);
    out[10] = now->humidity_rh;
    out[11] = now->abs_humidity_gm3;
    out[12] = slope_per_min(w, LAG_5MIN, f_ah);
    out[13] = mean_of(w, LAG_1MIN, f_pm);
    return true;
}
