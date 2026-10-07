/* app.h - the sampling state machine and the published snapshot.
 *
 * Everything above the drivers lives here, and nothing here knows about
 * FreeRTOS, lwIP or the ESP-IDF. That is deliberate: the firmware's sensor
 * task is a dozen lines that call aeris_app_step() in a loop, and the host
 * simulator calls the identical function against virtual sensors. The logic
 * under test is the logic that ships.
 *
 * Concurrency contract: aeris_app_step() is called from exactly one task.
 * Other tasks (HTTP, LED) only ever call aeris_app_snapshot(), which copies
 * under a lock supplied by the platform through aeris_app_set_lock().
 */
#ifndef AERIS_APP_H
#define AERIS_APP_H

#include <stdbool.h>
#include <stdint.h>
#include "aeris/aqi.h"
#include "aeris/classifier.h"
#include "aeris/features.h"
#include "aeris/gas_index.h"
#include "aeris/history.h"
#include "aeris/i2c_hal.h"
#include "aeris/model.h"
#include "aeris/pmsa003i.h"
#include "aeris/scd40.h"
#include "aeris/sgp41.h"
#include "aeris/sht31.h"

typedef enum {
    AERIS_PHASE_BOOT = 0,
    AERIS_PHASE_PROBE,
    AERIS_PHASE_CONDITION,   /* SGP41 10 s burn-in                        */
    AERIS_PHASE_WARMUP,      /* sampling, but history too short to decide */
    AERIS_PHASE_RUN,
    AERIS_PHASE_FAULT,       /* no sensor answered at all                 */
} aeris_phase_t;

/* What the HTTP layer and the LED read. Plain values, no pointers, so it can
 * be memcpy'd out under a lock. */
typedef struct {
    uint32_t uptime_ms;
    aeris_phase_t phase;

    float co2_ppm;
    float voc_index;
    float nox_index;
    float temperature_c;
    float humidity_rh;
    float abs_humidity_gm3;
    float pm1_0_ugm3, pm2_5_ugm3, pm10_ugm3;
    uint16_t sraw_voc, sraw_nox;

    aeris_aqi_t aqi;

    int      event;
    float    event_confidence;
    uint32_t event_age_ms;
    float    class_prob[AERIS_N_CLASSES];
    uint32_t inference_us;

    bool scd40_present, sgp41_present, sht31_present, pmsa003i_present;
    uint32_t samples_total, sample_errors, crc_errors;
    uint32_t sgp41_condition_remaining_ms;
    bool features_valid;
    float features[AERIS_N_FEATURES];
} aeris_snapshot_t;

typedef void (*aeris_lock_fn)(void *ctx, bool acquire);
/* Microsecond clock, used only to time inference for the status page.
 * Optional: NULL leaves inference_us at 0. */
typedef uint32_t (*aeris_us_fn)(void *ctx);

typedef struct {
    const aeris_i2c_t *bus;

    scd40_t    scd;
    sgp41_t    sgp;
    sht31_t    sht;
    pmsa003i_t pms;

    gas_index_t voc_gi, nox_gi;
    aeris_window_t     win;
    aeris_classifier_t cls;
    aeris_history_t    hist;

    aeris_phase_t phase;
    uint32_t      phase_since_ms;
    uint32_t      boot_ms;

    /* Last good readings, carried forward when one sensor misses a cycle so a
     * single NACK does not punch a hole in the feature window. */
    float last_temp_c, last_rh;
    bool  have_rht;

    aeris_snapshot_t snap;

    aeris_lock_fn lock_fn;
    void         *lock_ctx;
    aeris_us_fn   us_fn;
    void         *us_ctx;

    uint16_t probe_attempts;

    /* Calibration knobs captured at init and applied during PROBE, which is
     * the only phase where the SCD40 accepts them (it must be idle). */
    float    cfg_temp_offset_c;
    uint16_t cfg_altitude_m;
    bool     cfg_asc;
} aeris_app_t;

typedef struct {
    float    scd40_temp_offset_c;   /* self-heating offset, see docs      */
    uint16_t altitude_m;
    bool     scd40_asc_enabled;
    float    classifier_alpha;
    float    classifier_margin;
    uint32_t classifier_dwell_ms;
} aeris_config_t;

aeris_config_t aeris_config_default(void);

void aeris_app_init(aeris_app_t *a, const aeris_i2c_t *bus,
                    const aeris_config_t *cfg);
void aeris_app_set_lock(aeris_app_t *a, aeris_lock_fn fn, void *ctx);
void aeris_app_set_us_clock(aeris_app_t *a, aeris_us_fn fn, void *ctx);

/* Advance one cycle. Call every AERIS_SAMPLE_PERIOD_MS once running; during
 * CONDITION it wants to be called about once a second. Returns the number of
 * milliseconds the caller should wait before the next call. */
uint32_t aeris_app_step(aeris_app_t *a);

/* Thread-safe copy of the published state. */
void aeris_app_snapshot(aeris_app_t *a, aeris_snapshot_t *out);

const char *aeris_phase_name(aeris_phase_t p);

#endif /* AERIS_APP_H */
