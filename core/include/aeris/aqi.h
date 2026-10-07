/* aqi.h - a single 0-100 score, plus the usual banding.
 *
 * Deliberately not the regulatory US AQI: that is a PM/ozone outdoor index and
 * says nothing about CO2, which indoors is the number that actually tracks how
 * the room feels. This is a worst-of-channels score over CO2, PM2.5, VOC and
 * NOx with thresholds from ASHRAE 62.1 (CO2) and the WHO 2021 24-hour PM2.5
 * guideline, documented in docs/ML.md so the numbers are auditable.
 */
#ifndef AERIS_AQI_H
#define AERIS_AQI_H

#include <stdint.h>

typedef enum {
    AERIS_BAND_GOOD = 0,
    AERIS_BAND_FAIR,
    AERIS_BAND_POOR,
    AERIS_BAND_BAD,
} aeris_band_t;

typedef struct {
    uint8_t      score;       /* 0 worst .. 100 best */
    aeris_band_t band;
    const char  *driver;      /* which channel set the score */
} aeris_aqi_t;

aeris_aqi_t aeris_aqi_compute(float co2_ppm, float pm2_5, float voc_index,
                              float nox_index);
const char *aeris_band_name(aeris_band_t b);

#endif /* AERIS_AQI_H */
