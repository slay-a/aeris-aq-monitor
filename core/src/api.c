#include <stdio.h>
#include <string.h>
#include "aeris/api.h"
#include "aeris/json.h"

size_t aeris_api_live(const aeris_snapshot_t *s, char *buf, size_t cap)
{
    aeris_json_t j;
    aeris_json_init(&j, buf, cap);

    aeris_json_obj_open(&j);
    aeris_json_u32(&j, "uptime_s", s->uptime_ms / 1000u);              aeris_json_comma(&j);
    aeris_json_str(&j, "phase", aeris_phase_name(s->phase));           aeris_json_comma(&j);

    aeris_json_key(&j, "readings");
    aeris_json_obj_open(&j);
    aeris_json_f(&j, "co2_ppm",       s->co2_ppm,       0); aeris_json_comma(&j);
    aeris_json_f(&j, "voc_index",     s->voc_index,     0); aeris_json_comma(&j);
    aeris_json_f(&j, "nox_index",     s->nox_index,     0); aeris_json_comma(&j);
    aeris_json_f(&j, "temperature_c", s->temperature_c, 2); aeris_json_comma(&j);
    aeris_json_f(&j, "humidity_pct",  s->humidity_rh,   1); aeris_json_comma(&j);
    aeris_json_f(&j, "abs_humidity_gm3", s->abs_humidity_gm3, 2); aeris_json_comma(&j);
    aeris_json_f(&j, "pm1_0_ugm3",    s->pm1_0_ugm3,    1); aeris_json_comma(&j);
    aeris_json_f(&j, "pm2_5_ugm3",    s->pm2_5_ugm3,    1); aeris_json_comma(&j);
    aeris_json_f(&j, "pm10_ugm3",     s->pm10_ugm3,     1);
    aeris_json_obj_close(&j);
    aeris_json_comma(&j);

    aeris_json_key(&j, "air_quality");
    aeris_json_obj_open(&j);
    aeris_json_u32(&j, "score", s->aqi.score);                      aeris_json_comma(&j);
    aeris_json_str(&j, "band", aeris_band_name(s->aqi.band));        aeris_json_comma(&j);
    aeris_json_str(&j, "driver", s->aqi.driver ? s->aqi.driver : "");
    aeris_json_obj_close(&j);
    aeris_json_comma(&j);

    aeris_json_key(&j, "event");
    aeris_json_obj_open(&j);
    aeris_json_str(&j, "label", aeris_class_name(s->event));            aeris_json_comma(&j);
    aeris_json_i32(&j, "class_id", s->event);                           aeris_json_comma(&j);
    aeris_json_f(&j, "confidence", s->event_confidence, 3);             aeris_json_comma(&j);
    aeris_json_u32(&j, "age_s", s->event_age_ms / 1000u);               aeris_json_comma(&j);
    aeris_json_str(&j, "advice", aeris_advice_for(s->event, s->aqi.band));    aeris_json_comma(&j);
    aeris_json_key(&j, "probabilities");
    aeris_json_obj_open(&j);
    for (int c = 0; c < AERIS_N_CLASSES; c++) {
        if (c) aeris_json_comma(&j);
        aeris_json_f(&j, aeris_class_name(c), s->class_prob[c], 4);
    }
    aeris_json_obj_close(&j);
    aeris_json_obj_close(&j);

    aeris_json_obj_close(&j);
    return aeris_json_ok(&j) ? j.len : 0;
}

size_t aeris_api_history(const aeris_history_t *h, char *buf, size_t cap)
{
    aeris_json_t j;
    aeris_json_init(&j, buf, cap);

    aeris_json_obj_open(&j);
    aeris_json_u32(&j, "period_s", AERIS_HISTORY_PERIOD_MS / 1000u);
    aeris_json_comma(&j);
    aeris_json_u32(&j, "count", h->count);
    aeris_json_comma(&j);

    /* Column-oriented: parallel arrays cost about 40 % fewer bytes than an
     * array of objects, which matters when the whole response has to fit in
     * one chunked send from a 400 kB-RAM part. */
    static const char *const keys[] = {
        "t_s", "co2", "voc", "nox", "pm2_5", "temp_c", "rh", "event"
    };
    for (int k = 0; k < 8; k++) {
        if (k) aeris_json_comma(&j);
        aeris_json_key(&j, keys[k]);
        aeris_json_arr_open(&j);
        for (uint16_t i = 0; i < h->count; i++) {
            const aeris_hist_rec_t *r = aeris_history_get(h, i);
            if (!r) break;
            if (i) aeris_json_comma(&j);
            char tmp[16];
            switch (k) {
            case 0: snprintf(tmp, sizeof tmp, "%lu", (unsigned long)r->t_s); break;
            case 1: snprintf(tmp, sizeof tmp, "%u", r->co2); break;
            case 2: snprintf(tmp, sizeof tmp, "%u", r->voc); break;
            case 3: snprintf(tmp, sizeof tmp, "%u", r->nox); break;
            case 4: snprintf(tmp, sizeof tmp, "%.1f", r->pm2_5_x10 / 10.0); break;
            case 5: snprintf(tmp, sizeof tmp, "%.1f", r->temp_cx10 / 10.0); break;
            case 6: snprintf(tmp, sizeof tmp, "%u", r->rh); break;
            default: snprintf(tmp, sizeof tmp, "%u", r->event); break;
            }
            aeris_json_raw(&j, tmp);
        }
        aeris_json_arr_close(&j);
    }
    aeris_json_obj_close(&j);
    return aeris_json_ok(&j) ? j.len : 0;
}

size_t aeris_api_status(const aeris_snapshot_t *s, const aeris_app_t *a,
                        const char *fw_version, char *buf, size_t cap)
{
    aeris_json_t j;
    aeris_json_init(&j, buf, cap);

    aeris_json_obj_open(&j);
    aeris_json_str(&j, "firmware", fw_version ? fw_version : "dev");
    aeris_json_comma(&j);
    aeris_json_str(&j, "phase", aeris_phase_name(s->phase));
    aeris_json_comma(&j);
    aeris_json_u32(&j, "uptime_s", s->uptime_ms / 1000u);
    aeris_json_comma(&j);

    aeris_json_key(&j, "sensors");
    aeris_json_obj_open(&j);
    aeris_json_key(&j, "scd40");
    aeris_json_obj_open(&j);
    aeris_json_bool(&j, "present", s->scd40_present);     aeris_json_comma(&j);
    aeris_json_u32(&j, "crc_errors", a->scd.crc_errors);  aeris_json_comma(&j);
    aeris_json_u32(&j, "io_errors",  a->scd.io_errors);
    aeris_json_obj_close(&j); aeris_json_comma(&j);
    aeris_json_key(&j, "sgp41");
    aeris_json_obj_open(&j);
    aeris_json_bool(&j, "present", s->sgp41_present);     aeris_json_comma(&j);
    aeris_json_bool(&j, "conditioned", a->sgp.conditioned); aeris_json_comma(&j);
    aeris_json_u32(&j, "crc_errors", a->sgp.crc_errors);  aeris_json_comma(&j);
    aeris_json_u32(&j, "io_errors",  a->sgp.io_errors);
    aeris_json_obj_close(&j); aeris_json_comma(&j);
    aeris_json_key(&j, "sht31");
    aeris_json_obj_open(&j);
    aeris_json_bool(&j, "present", s->sht31_present);     aeris_json_comma(&j);
    aeris_json_u32(&j, "crc_errors", a->sht.crc_errors);  aeris_json_comma(&j);
    aeris_json_u32(&j, "io_errors",  a->sht.io_errors);
    aeris_json_obj_close(&j); aeris_json_comma(&j);
    aeris_json_key(&j, "pmsa003i");
    aeris_json_obj_open(&j);
    aeris_json_bool(&j, "present", s->pmsa003i_present);  aeris_json_comma(&j);
    aeris_json_u32(&j, "frame_errors", a->pms.frame_errors); aeris_json_comma(&j);
    aeris_json_u32(&j, "io_errors",  a->pms.io_errors);
    aeris_json_obj_close(&j);
    aeris_json_obj_close(&j);
    aeris_json_comma(&j);

    aeris_json_key(&j, "model");
    aeris_json_obj_open(&j);
    aeris_json_u32(&j, "param_bytes", aeris_model_size_bytes());  aeris_json_comma(&j);
    aeris_json_u32(&j, "n_features", AERIS_N_FEATURES);           aeris_json_comma(&j);
    aeris_json_u32(&j, "n_classes", AERIS_N_CLASSES);             aeris_json_comma(&j);
    aeris_json_u32(&j, "inference_us", s->inference_us);
    aeris_json_obj_close(&j);
    aeris_json_comma(&j);

    aeris_json_key(&j, "counters");
    aeris_json_obj_open(&j);
    aeris_json_u32(&j, "samples", s->samples_total);        aeris_json_comma(&j);
    aeris_json_u32(&j, "sample_errors", s->sample_errors);  aeris_json_comma(&j);
    aeris_json_u32(&j, "crc_errors", s->crc_errors);        aeris_json_comma(&j);
    aeris_json_u32(&j, "event_transitions", a->cls.transitions);
    aeris_json_obj_close(&j);

    aeris_json_obj_close(&j);
    return aeris_json_ok(&j) ? j.len : 0;
}

size_t aeris_api_debug(const aeris_snapshot_t *s, char *buf, size_t cap)
{
    aeris_json_t j;
    aeris_json_init(&j, buf, cap);
    aeris_json_obj_open(&j);
    aeris_json_bool(&j, "features_valid", s->features_valid);
    aeris_json_comma(&j);
    aeris_json_u32(&j, "sraw_voc", s->sraw_voc); aeris_json_comma(&j);
    aeris_json_u32(&j, "sraw_nox", s->sraw_nox); aeris_json_comma(&j);
    aeris_json_key(&j, "features");
    aeris_json_obj_open(&j);
    for (int i = 0; i < AERIS_N_FEATURES; i++) {
        if (i) aeris_json_comma(&j);
        aeris_json_f(&j, aeris_feature_name(i), s->features[i], 4);
    }
    aeris_json_obj_close(&j);
    aeris_json_obj_close(&j);
    return aeris_json_ok(&j) ? j.len : 0;
}
