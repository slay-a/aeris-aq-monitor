/* features.h - rolling window over fused samples, and the feature vector the
 * classifier consumes.
 *
 * The window is a fixed-size ring in static RAM: no allocation anywhere in
 * the firmware. At the 5 s sampling cadence, 72 slots is six minutes of
 * history, which is what the slowest feature (the 5-minute CO2 slope) needs.
 */
#ifndef AERIS_FEATURES_H
#define AERIS_FEATURES_H

#include <stdbool.h>
#include <stdint.h>

#define AERIS_WINDOW_SLOTS   72
#define AERIS_SAMPLE_PERIOD_MS 5000
#define AERIS_N_FEATURES     14

/* One fused observation: every sensor reduced to the quantities the model
 * actually uses. */
typedef struct {
    uint32_t t_ms;
    float co2_ppm;
    float voc_index;
    float nox_index;
    float temperature_c;
    float humidity_rh;
    float abs_humidity_gm3;   /* derived: see aeris_absolute_humidity()   */
    float pm2_5_ugm3;
    bool  valid;
} aeris_obs_t;

typedef struct {
    aeris_obs_t slot[AERIS_WINDOW_SLOTS];
    uint16_t head;      /* next write position */
    uint16_t count;      /* valid slots, saturating at AERIS_WINDOW_SLOTS */
} aeris_window_t;

void               aeris_window_init(aeris_window_t *w);
void               aeris_window_push(aeris_window_t *w, const aeris_obs_t *o);
/* age_back = 0 is the newest sample. Returns NULL past the end of history. */
const aeris_obs_t *aeris_window_at(const aeris_window_t *w, uint16_t age_back);
const aeris_obs_t *aeris_window_latest(const aeris_window_t *w);
bool               aeris_window_full_enough(const aeris_window_t *w);

/* Magnus-formula absolute humidity in g/m3. Pulling T and RH into a single
 * physical quantity gives the model a feature that does not change when a
 * window opens on a cold day -- which is exactly the confusion that made the
 * first version mislabel ventilation as occupancy. */
float aeris_absolute_humidity(float temp_c, float rh_percent);

/* Fill out[AERIS_N_FEATURES] from the window. Returns false if there is not
 * yet enough history for the slopes to mean anything. */
bool aeris_extract_features(const aeris_window_t *w, float *out);

/* Human-readable names, index-aligned with the vector. Used by the training
 * exporter and the /api/debug endpoint so the two can never drift. */
const char *aeris_feature_name(int i);

#endif /* AERIS_FEATURES_H */
