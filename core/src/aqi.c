#include "aeris/aqi.h"

/* Each channel maps to a 0..100 sub-score by piecewise-linear interpolation
 * over (value, score) breakpoints. Scores are "higher is better". */
typedef struct { float v; float s; } bp_t;

static float interp(const bp_t *bp, int n, float v)
{
    if (v <= bp[0].v) return bp[0].s;
    for (int i = 1; i < n; i++) {
        if (v <= bp[i].v) {
            float t = (v - bp[i - 1].v) / (bp[i].v - bp[i - 1].v);
            return bp[i - 1].s + t * (bp[i].s - bp[i - 1].s);
        }
    }
    return bp[n - 1].s;
}

/* CO2: 400 outdoor baseline, 1000 is the ASHRAE 62.1 comfort ceiling,
 * 1400+ is where measured cognitive-performance effects appear. */
static const bp_t k_co2[] = {
    {  400.f, 100.f }, {  700.f, 85.f }, { 1000.f, 60.f },
    { 1400.f,  35.f }, { 2000.f, 15.f }, { 5000.f,  0.f },
};
/* PM2.5: WHO 2021 24-h guideline is 15 ug/m3; interim target 1 is 75. */
static const bp_t k_pm[] = {
    { 0.f, 100.f }, { 5.f, 90.f }, { 15.f, 65.f },
    { 35.f, 40.f }, { 75.f, 15.f }, { 150.f, 0.f },
};
/* Gas indices: 100 is the sensor's own notion of clean air. */
static const bp_t k_gas[] = {
    { 0.f, 100.f }, { 100.f, 90.f }, { 150.f, 70.f },
    { 250.f, 45.f }, { 350.f, 20.f }, { 500.f, 0.f },
};

#define NBP(a) ((int)(sizeof(a) / sizeof((a)[0])))

aeris_aqi_t aeris_aqi_compute(float co2_ppm, float pm2_5, float voc_index,
                              float nox_index)
{
    float s_co2 = interp(k_co2, NBP(k_co2), co2_ppm);
    float s_pm  = interp(k_pm,  NBP(k_pm),  pm2_5);
    float s_voc = interp(k_gas, NBP(k_gas), voc_index);
    float s_nox = interp(k_gas, NBP(k_gas), nox_index);

    /* Worst channel wins: a room with perfect CO2 and 80 ug/m3 of smoke is
     * not "mostly fine on average". */
    float worst = s_co2;
    const char *driver = "co2";
    if (s_pm  < worst) { worst = s_pm;  driver = "pm2_5"; }
    if (s_voc < worst) { worst = s_voc; driver = "voc";   }
    if (s_nox < worst) { worst = s_nox; driver = "nox";   }

    aeris_aqi_t out;
    out.score  = (uint8_t)(worst + 0.5f);
    out.driver = driver;
    out.band   = (worst >= 80.f) ? AERIS_BAND_GOOD
               : (worst >= 60.f) ? AERIS_BAND_FAIR
               : (worst >= 35.f) ? AERIS_BAND_POOR
                                 : AERIS_BAND_BAD;
    return out;
}

const char *aeris_band_name(aeris_band_t b)
{
    switch (b) {
    case AERIS_BAND_GOOD: return "good";
    case AERIS_BAND_FAIR: return "fair";
    case AERIS_BAND_POOR: return "poor";
    case AERIS_BAND_BAD:  return "bad";
    }
    return "unknown";
}
