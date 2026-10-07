#include <math.h>
#include <string.h>
#include "aeris/app.h"

#define NAN_F (0.0f / 0.0f)

aeris_config_t aeris_config_default(void)
{
    aeris_config_t c;
    /* 4.0 C is the offset measured on this board: the SCD40 sits 12 mm from
     * the regulator and reads high. Re-measure per build -- docs/BRINGUP.md
     * step 6 has the procedure. */
    c.scd40_temp_offset_c = 4.0f;
    c.altitude_m          = 0;
    c.scd40_asc_enabled   = true;
    /* alpha 0.25 at 5 s is a ~17 s probability time constant; margin and
     * dwell were tuned on the recorded sessions in ml/data/. */
    c.classifier_alpha    = 0.25f;
    c.classifier_margin   = 0.12f;
    c.classifier_dwell_ms = 20000;
    return c;
}

static void set_phase(aeris_app_t *a, aeris_phase_t p)
{
    a->phase = p;
    a->phase_since_ms = aeris_now_ms(a->bus);
    a->snap.phase = p;
}

static void lock(aeris_app_t *a, bool acquire)
{
    if (a->lock_fn) a->lock_fn(a->lock_ctx, acquire);
}

void aeris_app_init(aeris_app_t *a, const aeris_i2c_t *bus,
                    const aeris_config_t *cfg)
{
    memset(a, 0, sizeof *a);
    a->bus = bus;

    aeris_config_t c = cfg ? *cfg : aeris_config_default();

    scd40_init(&a->scd, bus, SCD40_I2C_ADDR);
    sgp41_init(&a->sgp, bus, SGP41_I2C_ADDR);
    sht31_init(&a->sht, bus, SHT31_I2C_ADDR_A);
    pmsa003i_init(&a->pms, bus, PMSA003I_I2C_ADDR);

    /* VOC baseline: 12 h time constant when the signal is running away from
     * baseline, 10 min when recovering. NOx baselines far more slowly -- it
     * is a near-fixed offset in Sensirion's own treatment -- so it gets a
     * 24 h away-constant and a lower gain. */
    gas_index_init(&a->voc_gi, AERIS_SAMPLE_PERIOD_MS, 600.0f,  43200.0f, 45.0f);
    gas_index_init(&a->nox_gi, AERIS_SAMPLE_PERIOD_MS, 3600.0f, 86400.0f, 30.0f);

    aeris_window_init(&a->win);
    aeris_history_init(&a->hist);
    aeris_classifier_init(&a->cls, c.classifier_alpha, c.classifier_margin,
                          c.classifier_dwell_ms);

    a->boot_ms = aeris_now_ms(bus);
    a->last_temp_c = NAN_F;
    a->last_rh     = NAN_F;

    a->snap.co2_ppm       = NAN_F;
    a->snap.voc_index     = NAN_F;
    a->snap.nox_index     = NAN_F;
    a->snap.temperature_c = NAN_F;
    a->snap.humidity_rh   = NAN_F;
    a->snap.abs_humidity_gm3 = NAN_F;
    a->snap.pm1_0_ugm3 = NAN_F;
    a->snap.pm2_5_ugm3 = NAN_F;
    a->snap.pm10_ugm3  = NAN_F;

    a->snap.sgp41_condition_remaining_ms = SGP41_CONDITION_TOTAL_MS;

    /* The SCD40 only accepts these while idle, so PROBE applies them. */
    a->cfg_temp_offset_c = c.scd40_temp_offset_c;
    a->cfg_altitude_m    = c.altitude_m;
    a->cfg_asc           = c.scd40_asc_enabled;

    set_phase(a, AERIS_PHASE_BOOT);
}

void aeris_app_set_lock(aeris_app_t *a, aeris_lock_fn fn, void *ctx)
{
    a->lock_fn = fn; a->lock_ctx = ctx;
}

void aeris_app_set_us_clock(aeris_app_t *a, aeris_us_fn fn, void *ctx)
{
    a->us_fn = fn; a->us_ctx = ctx;
}

/* ---- phases ----------------------------------------------------------- */

static void phase_probe(aeris_app_t *a)
{
    a->probe_attempts++;

    a->snap.sht31_present = (sht31_soft_reset(&a->sht) == AERIS_OK);
    if (a->snap.sht31_present) {
        sht31_sample_t s;
        a->snap.sht31_present = (sht31_measure(&a->sht, &s) == AERIS_OK);
    }

    a->snap.sgp41_present = (sgp41_probe(&a->sgp) == AERIS_OK);

    a->snap.scd40_present = (scd40_probe(&a->scd) == AERIS_OK);
    if (a->snap.scd40_present) {
        /* Order matters: offset and altitude are only accepted while idle,
         * and scd40_probe() leaves the part idle. */
        (void)scd40_set_temperature_offset(&a->scd, a->cfg_temp_offset_c);
        (void)scd40_set_sensor_altitude(&a->scd, a->cfg_altitude_m);
        (void)scd40_set_asc_enabled(&a->scd, a->cfg_asc);
        (void)scd40_start_periodic(&a->scd);
    }

    pmsa003i_sample_t p;
    a->snap.pmsa003i_present = (pmsa003i_read(&a->pms, &p) == AERIS_OK);

    if (!a->snap.scd40_present && !a->snap.sgp41_present &&
        !a->snap.sht31_present && !a->snap.pmsa003i_present) {
        /* Nothing on the bus. Retry a few times -- a cold SCD40 can NACK its
         * first transaction -- then latch a fault the LED can show. */
        if (a->probe_attempts >= 3) set_phase(a, AERIS_PHASE_FAULT);
        return;
    }

    set_phase(a, a->snap.sgp41_present ? AERIS_PHASE_CONDITION
                                       : AERIS_PHASE_WARMUP);
}

static void phase_condition(aeris_app_t *a)
{
    uint16_t voc_raw = 0;
    (void)sgp41_condition_step(&a->sgp, &voc_raw);

    uint32_t elapsed = aeris_now_ms(a->bus) - a->phase_since_ms;
    uint32_t remaining = (elapsed >= SGP41_CONDITION_TOTAL_MS)
                       ? 0 : (SGP41_CONDITION_TOTAL_MS - elapsed);
    a->snap.sgp41_condition_remaining_ms = remaining;

    if (remaining == 0 || sgp41_is_conditioned(&a->sgp)) {
        a->sgp.conditioned = true;
        set_phase(a, AERIS_PHASE_WARMUP);
    }
}

/* One full sampling cycle. Reads are ordered so the SGP41 gets the freshest
 * possible RH/T: SHT31 first, SGP41 immediately after. */
static void phase_sample(aeris_app_t *a)
{
    aeris_obs_t obs;
    memset(&obs, 0, sizeof obs);
    obs.t_ms = aeris_now_ms(a->bus);
    bool any = false;

    /* --- SHT31: temperature and humidity ------------------------------- */
    if (a->snap.sht31_present) {
        sht31_sample_t s;
        if (sht31_measure(&a->sht, &s) == AERIS_OK) {
            a->last_temp_c = s.temperature_c;
            a->last_rh     = s.humidity_rh;
            a->have_rht    = true;
            any = true;
        } else {
            a->snap.sample_errors++;
        }
    }

    /* --- SGP41: VOC/NOx, compensated with the reading we just took ----- */
    if (a->snap.sgp41_present) {
        sgp41_raw_t raw;
        /* When the SHT31 is absent or stale these are NaN, and the driver
         * substitutes the datasheet defaults rather than garbage ticks. */
        float rh_in = a->have_rht ? a->last_rh     : NAN_F;
        float t_in  = a->have_rht ? a->last_temp_c : NAN_F;
        if (sgp41_measure(&a->sgp, rh_in, t_in, &raw) == AERIS_OK) {
            a->snap.sraw_voc = raw.sraw_voc;
            a->snap.sraw_nox = raw.sraw_nox;
            obs.voc_index = gas_index_update(&a->voc_gi, raw.sraw_voc);
            obs.nox_index = gas_index_update(&a->nox_gi, raw.sraw_nox);
            any = true;
        } else {
            a->snap.sample_errors++;
            obs.voc_index = a->snap.voc_index;
            obs.nox_index = a->snap.nox_index;
        }
    } else {
        obs.voc_index = GAS_INDEX_NOMINAL;
        obs.nox_index = GAS_INDEX_NOMINAL;
    }

    /* --- SCD40: CO2 ---------------------------------------------------- */
    float co2 = a->snap.co2_ppm;
    if (a->snap.scd40_present) {
        bool ready = false;
        if (scd40_data_ready(&a->scd, &ready) == AERIS_OK && ready) {
            scd40_sample_t s;
            aeris_err_t e = scd40_read_measurement(&a->scd, &s);
            if (e == AERIS_OK) {
                co2 = (float)s.co2_ppm;
                /* The SCD40 reports T/RH too, but its on-die sensor sits next
                 * to a heated optical cavity. The SHT31 is the authority; the
                 * SCD40's copy is only a fallback. */
                if (!a->have_rht) {
                    a->last_temp_c = s.temperature_c;
                    a->last_rh     = s.humidity_rh;
                    a->have_rht    = true;
                }
                any = true;
            } else {
                a->snap.sample_errors++;
            }
        }
    }
    obs.co2_ppm = co2;

    /* --- PMSA003I: particulates ---------------------------------------- */
    if (a->snap.pmsa003i_present) {
        pmsa003i_sample_t p;
        if (pmsa003i_read(&a->pms, &p) == AERIS_OK) {
            a->snap.pm1_0_ugm3 = (float)p.pm1_0_env;
            a->snap.pm2_5_ugm3 = (float)p.pm2_5_env;
            a->snap.pm10_ugm3  = (float)p.pm10_env;
            any = true;
        } else {
            a->snap.sample_errors++;
        }
    }
    obs.pm2_5_ugm3 = (a->snap.pm2_5_ugm3 == a->snap.pm2_5_ugm3)
                   ? a->snap.pm2_5_ugm3 : 0.0f;

    obs.temperature_c = a->have_rht ? a->last_temp_c : NAN_F;
    obs.humidity_rh   = a->have_rht ? a->last_rh     : NAN_F;
    obs.abs_humidity_gm3 = a->have_rht
        ? aeris_absolute_humidity(a->last_temp_c, a->last_rh) : NAN_F;

    /* A cycle counts as valid only if CO2 and RH/T are both real, because
     * every feature slope depends on them. */
    obs.valid = any && a->have_rht && (obs.co2_ppm == obs.co2_ppm);

    aeris_window_push(&a->win, &obs);
    a->snap.samples_total++;

    /* --- inference ----------------------------------------------------- */
    float feats[AERIS_N_FEATURES];
    bool  have_feats = aeris_extract_features(&a->win, feats);
    aeris_infer_t inf;
    memset(&inf, 0, sizeof inf);

    if (have_feats) {
        uint32_t t0 = a->us_fn ? a->us_fn(a->us_ctx) : 0;
        aeris_model_infer(feats, &inf);
        uint32_t t1 = a->us_fn ? a->us_fn(a->us_ctx) : 0;
        inf.inference_us = t1 - t0;
        aeris_classifier_update(&a->cls, &inf, obs.t_ms);
        if (a->phase == AERIS_PHASE_WARMUP) set_phase(a, AERIS_PHASE_RUN);
    }

    aeris_aqi_t aqi = aeris_aqi_compute(
        obs.co2_ppm == obs.co2_ppm ? obs.co2_ppm : 400.0f,
        obs.pm2_5_ugm3, obs.voc_index, obs.nox_index);

    /* --- publish ------------------------------------------------------- */
    lock(a, true);
    a->snap.uptime_ms        = obs.t_ms - a->boot_ms;
    a->snap.co2_ppm          = obs.co2_ppm;
    a->snap.voc_index        = obs.voc_index;
    a->snap.nox_index        = obs.nox_index;
    a->snap.temperature_c    = obs.temperature_c;
    a->snap.humidity_rh      = obs.humidity_rh;
    a->snap.abs_humidity_gm3 = obs.abs_humidity_gm3;
    a->snap.aqi              = aqi;
    a->snap.features_valid   = have_feats;
    if (have_feats) {
        memcpy(a->snap.features, feats, sizeof feats);
        memcpy(a->snap.class_prob, a->cls.smooth, sizeof a->snap.class_prob);
        a->snap.event            = aeris_classifier_state(&a->cls);
        a->snap.event_confidence = aeris_classifier_confidence(&a->cls);
        a->snap.event_age_ms     = aeris_classifier_state_age_ms(&a->cls, obs.t_ms);
        a->snap.inference_us     = inf.inference_us;
    }
    a->snap.crc_errors = a->scd.crc_errors + a->sgp.crc_errors +
                         a->sht.crc_errors + a->pms.frame_errors;
    lock(a, false);

    aeris_history_maybe_push(&a->hist, obs.t_ms, obs.co2_ppm, obs.voc_index,
                             obs.nox_index, obs.pm2_5_ugm3,
                             obs.temperature_c, obs.humidity_rh,
                             (uint8_t)a->snap.event);
}

uint32_t aeris_app_step(aeris_app_t *a)
{
    switch (a->phase) {
    case AERIS_PHASE_BOOT:
        set_phase(a, AERIS_PHASE_PROBE);
        return 10;

    case AERIS_PHASE_PROBE:
        phase_probe(a);
        return (a->phase == AERIS_PHASE_PROBE) ? 1000 : 10;

    case AERIS_PHASE_CONDITION:
        phase_condition(a);
        /* 1 Hz during burn-in: the datasheet specifies conditioning as a
         * sequence of 1 s commands, not one long one. */
        return 1000;

    case AERIS_PHASE_WARMUP:
    case AERIS_PHASE_RUN:
        phase_sample(a);
        return AERIS_SAMPLE_PERIOD_MS;

    case AERIS_PHASE_FAULT:
        /* Keep retrying: a hot-plugged sensor or a brown-out recovery should
         * bring the device back without a reboot. */
        set_phase(a, AERIS_PHASE_PROBE);
        a->probe_attempts = 0;
        return 5000;
    }
    return AERIS_SAMPLE_PERIOD_MS;
}

void aeris_app_snapshot(aeris_app_t *a, aeris_snapshot_t *out)
{
    lock(a, true);
    *out = a->snap;
    lock(a, false);
}

const char *aeris_phase_name(aeris_phase_t p)
{
    switch (p) {
    case AERIS_PHASE_BOOT:      return "boot";
    case AERIS_PHASE_PROBE:     return "probe";
    case AERIS_PHASE_CONDITION: return "conditioning";
    case AERIS_PHASE_WARMUP:    return "warmup";
    case AERIS_PHASE_RUN:       return "run";
    case AERIS_PHASE_FAULT:     return "fault";
    }
    return "unknown";
}
