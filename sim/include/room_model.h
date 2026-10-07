/* room_model.h - a physical model of one room, used to drive the virtual
 * sensors. Nothing in here is a sensor: it produces true concentrations, and
 * the virtual sensor models add their own transfer functions and noise.
 *
 * CO2 is a stirred-tank mass balance:
 *     dC/dt = n * G / V - (ACH / 3600) * (C - C_out)
 * where G is per-person CO2 generation (m3/s at STP), V the room volume and
 * ACH the air changes per hour. That single equation is why CO2 is such a good
 * occupancy proxy and such a bad cooking proxy -- an electric hob adds VOC and
 * particulates but no CO2 at all.
 *
 * VOC, NOx and PM2.5 each get a source term plus first-order decay, with the
 * ventilation rate acting on all of them.
 */
#ifndef ROOM_MODEL_H
#define ROOM_MODEL_H

#include <stdbool.h>
#include <stdint.h>

/* Must stay index-aligned with AERIS_CLASS_NAMES in model_params.h. */
typedef enum {
    EV_BASELINE = 0,
    EV_OCCUPANCY,
    EV_COOKING,
    EV_VENTILATION,
    EV_VOLATILE,
    EV_N_CLASSES,
} room_event_t;

typedef struct {
    /* Room constants. */
    float volume_m3;
    float outdoor_co2_ppm;
    float outdoor_temp_c;
    float outdoor_rh;
    float outdoor_pm2_5;

    /* The structure -- walls, floor, furniture -- and the moisture buffered in
     * those surfaces. Both are what stop a room behaving like a sealed box of
     * air, and leaving them out is what made the first version of this model
     * reach 46 C and 81 %RH after twenty minutes of frying. */
    float structure_temp_c;
    float surface_ah_gm3;

    /* State. */
    float co2_ppm;
    float voc_ppb;          /* lumped TVOC-equivalent */
    float nox_ppb;
    float pm2_5_ugm3;
    float temp_c;
    float rh;

    /* Current forcing. */
    int   occupants;
    float ach;              /* air changes per hour */
    bool  gas_hob;          /* gas cooking also makes NOx */
    float voc_source_ppb_s;
    float pm_source_ugm3_s;
    float heat_w;           /* sensible heat into the room */
    float moisture_g_s;

    room_event_t label;
    uint32_t     t_ms;
    uint32_t     rng;
} room_t;

void room_init(room_t *r, uint32_t seed);

/* Apply the forcing that defines an event. Called by the scenario driver. */
void room_set_event(room_t *r, room_event_t ev);

/* Advance the physics by dt_ms. */
void room_step(room_t *r, uint32_t dt_ms);

/* Uniform in [0,1) from the room's own PRNG, so a seed reproduces a run
 * exactly -- a simulator you cannot replay is not much use for debugging. */
float room_rand(room_t *r);
float room_randn(room_t *r);

const char *room_event_name(room_event_t ev);

#endif /* ROOM_MODEL_H */
