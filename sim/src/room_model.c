#include <math.h>
#include <string.h>
#include "room_model.h"

/* One adult at rest exhales roughly 18 L of CO2 per hour. */
#define CO2_PER_PERSON_M3_S (0.018f / 3600.0f)

void room_init(room_t *r, uint32_t seed)
{
    memset(r, 0, sizeof *r);
    r->volume_m3       = 38.0f;      /* a 15 m2 room, 2.5 m ceiling */
    r->outdoor_co2_ppm = 425.0f;
    r->outdoor_temp_c  = 8.0f;     /* a cool day: 5.4 g/m3 absolute */
    r->outdoor_rh      = 65.0f;
    r->outdoor_pm2_5   = 6.0f;

    r->co2_ppm    = 470.0f;
    r->voc_ppb    = 90.0f;
    r->nox_ppb    = 8.0f;
    r->pm2_5_ugm3 = 7.0f;
    r->temp_c     = 21.5f;
    r->rh         = 44.0f;

    r->ach   = 0.5f;                 /* a closed modern room */
    r->structure_temp_c = r->temp_c;
    r->surface_ah_gm3   = 8.0f;      /* 21.5 C at 44 %RH */
    r->label = EV_BASELINE;
    r->rng   = seed ? seed : 0x5EEDu;
}

float room_rand(room_t *r)
{
    /* xorshift32: small, fast, and reproducible across platforms, which a
     * libc rand() is not. */
    uint32_t x = r->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    r->rng = x;
    return (float)(x >> 8) / 16777216.0f;
}

float room_randn(room_t *r)
{
    /* Irwin-Hall approximation: sum of 6 uniforms, centred and scaled.
     * Plenty for sensor noise and avoids needing logf/cosf per sample. */
    float s = 0.0f;
    for (int i = 0; i < 6; i++) s += room_rand(r);
    return (s - 3.0f) * 0.7071f;
}

void room_set_event(room_t *r, room_event_t ev)
{
    r->label            = ev;
    r->occupants        = 0;
    r->ach              = 0.5f;
    r->gas_hob          = false;
    r->voc_source_ppb_s = 0.0f;
    r->pm_source_ugm3_s = 0.0f;
    r->heat_w           = 0.0f;
    r->moisture_g_s     = 0.0f;

    switch (ev) {
    case EV_BASELINE:
        /* Empty room, door shut. Everything decays toward outdoor. */
        break;

    case EV_OCCUPANCY:
        /* One or two people reading or working: CO2 climbs, a little moisture
         * and body-VOC, no particulates to speak of. */
        r->occupants        = 1 + (room_rand(r) < 0.45f ? 1 : 0);
        r->voc_source_ppb_s = 0.03f;
        r->heat_w           = 80.0f * (float)r->occupants;   /* sensible only */
        r->moisture_g_s     = 0.012f * (float)r->occupants;  /* ~43 g/h each */
        break;

    case EV_COOKING:
        /* Frying: strong PM and VOC, real heat and moisture, and on a gas hob
         * a large NOx source. CO2 only rises if the hob burns gas. */
        r->gas_hob          = room_rand(r) < 0.5f;
        r->occupants        = 1;
        r->voc_source_ppb_s = 1.6f + 1.4f * room_rand(r);
        /* Sized from the steady state it implies, not picked by feel: with the
         * loss rate below (vent + deposition, about 3.0e-4 /s) a source of
         * 0.05-0.13 ug/m3/s settles around 170-430 ug/m3, which is the range
         * published chamber studies report for pan-frying in a small kitchen.
         * An earlier guess of 0.55 implied 1800 ug/m3 and was nonsense. */
        r->pm_source_ugm3_s = 0.05f + 0.08f * room_rand(r);
        r->heat_w           = 900.0f + 700.0f * room_rand(r);
        /* Most of a hob's water vapour goes up the extractor or condenses on
         * cold surfaces. 0.10-0.20 g/s reaching the room air settles 2-4 g/m3
         * above baseline, which is what a kitchen hygrometer actually shows. */
        r->moisture_g_s     = 0.10f + 0.10f * room_rand(r);
        r->ach              = 0.8f;   /* extractor fan half-heartedly on */
        break;

    case EV_VENTILATION:
        /* Window open. The giveaway is not CO2 alone -- it is CO2 falling
         * while absolute humidity moves toward outdoor and temperature drops.
         */
        r->ach = 3.5f + 3.0f * room_rand(r);
        break;

    case EV_VOLATILE:
        /* Solvent: spray cleaner, paint, nail polish remover. Very large VOC
         * with no particulates, no CO2 and no heat -- the combination no other
         * class produces. */
        r->voc_source_ppb_s = 4.0f + 6.0f * room_rand(r);
        r->occupants        = 1;
        break;

    default: break;
    }
}

void room_step(room_t *r, uint32_t dt_ms)
{
    float dt = (float)dt_ms / 1000.0f;
    if (dt <= 0.0f) return;

    float k_vent = r->ach / 3600.0f;          /* 1/s */

    /* --- CO2 ----------------------------------------------------------- */
    float gen_ppm_s = (float)r->occupants * CO2_PER_PERSON_M3_S
                    / r->volume_m3 * 1.0e6f;
    if (r->gas_hob) gen_ppm_s += 0.9f;        /* combustion CO2 */
    r->co2_ppm += dt * (gen_ppm_s - k_vent * (r->co2_ppm - r->outdoor_co2_ppm));
    if (r->co2_ppm < r->outdoor_co2_ppm - 20.0f) r->co2_ppm = r->outdoor_co2_ppm - 20.0f;

    /* --- VOC: source, ventilation, plus surface adsorption ------------- */
    float k_voc_surface = 1.0f / 1800.0f;     /* 30 min half-life indoors */
    r->voc_ppb += dt * (r->voc_source_ppb_s
                        - (k_vent + k_voc_surface) * (r->voc_ppb - 20.0f));
    if (r->voc_ppb < 5.0f) r->voc_ppb = 5.0f;

    /* --- NOx: only gas combustion makes it indoors --------------------- */
    float nox_src = r->gas_hob ? 0.55f : 0.0f;
    float k_nox_surface = 1.0f / 900.0f;      /* NO2 deposits quickly */
    r->nox_ppb += dt * (nox_src - (k_vent + k_nox_surface) * (r->nox_ppb - 3.0f));
    if (r->nox_ppb < 0.5f) r->nox_ppb = 0.5f;

    /* --- PM2.5: source, ventilation, deposition ------------------------ */
    /* 0.3 air changes per hour equivalent: measured indoor deposition rates
     * for accumulation-mode (0.1-2.5 um) particles sit around 0.2-0.5 /h. */
    float k_settle = 0.30f / 3600.0f;
    r->pm2_5_ugm3 += dt * (r->pm_source_ugm3_s
                           - (k_vent + k_settle) * r->pm2_5_ugm3
                           + k_vent * r->outdoor_pm2_5);
    if (r->pm2_5_ugm3 < 0.5f) r->pm2_5_ugm3 = 0.5f;

    /* --- Temperature: internal gain, air exchange, and the structure ---- */
    /* 1.2 kg/m3 * 1005 J/kg/K is the heat capacity of the room's *air*, but the
     * air is only a small part of what a room stores heat in. Walls, floor and
     * furniture are convectively coupled to it with a time constant of roughly
     * a quarter of an hour, and together they are why a 1.5 kW hob raises a
     * kitchen by a couple of degrees rather than forty. The 12x multiplier and
     * the 900 s coupling are the usual lumped-capacitance approximation. */
    const float STRUCT_MASS_RATIO = 12.0f;
    const float TAU_STRUCT_S      = 900.0f;
    float mcp = r->volume_m3 * 1.2f * 1005.0f * STRUCT_MASS_RATIO;
    r->temp_c += dt * (r->heat_w / mcp
                       - k_vent * (r->temp_c - r->outdoor_temp_c)
                       - (r->temp_c - r->structure_temp_c) / TAU_STRUCT_S);

    /* --- Humidity: track absolute, then convert back ------------------- */
    float es = 6.112f * expf((17.67f * r->temp_c) / (r->temp_c + 243.5f));
    float ah = 216.7f * (es * (r->rh / 100.0f)) / (r->temp_c + 273.15f);
    float es_o = 6.112f * expf((17.67f * r->outdoor_temp_c) / (r->outdoor_temp_c + 243.5f));
    float ah_o = 216.7f * (es_o * (r->outdoor_rh / 100.0f)) / (r->outdoor_temp_c + 273.15f);
    /* Surfaces buffer moisture the same way the structure buffers heat: a
     * 15-minute exchange with whatever the walls and soft furnishings hold.
     * Without it, twenty minutes of cooking drives the room to saturation. */
    const float TAU_SURFACE_S = 900.0f;
    ah += dt * (r->moisture_g_s / r->volume_m3
                - k_vent * (ah - ah_o)
                - (ah - r->surface_ah_gm3) / TAU_SURFACE_S);
    if (ah < 0.5f) ah = 0.5f;
    float rh = 100.0f * (ah * (r->temp_c + 273.15f)) / (216.7f * es);
    if (rh < 5.0f)  rh = 5.0f;
    if (rh > 99.0f) rh = 99.0f;
    r->rh = rh;

    r->t_ms += dt_ms;
}

const char *room_event_name(room_event_t ev)
{
    switch (ev) {
    case EV_BASELINE:    return "baseline";
    case EV_OCCUPANCY:   return "occupancy";
    case EV_COOKING:     return "cooking";
    case EV_VENTILATION: return "ventilation";
    case EV_VOLATILE:    return "volatile";
    default:             return "?";
    }
}
