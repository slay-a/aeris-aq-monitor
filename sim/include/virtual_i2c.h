/* virtual_i2c.h - a virtual I2C bus with four device models.
 *
 * The models answer the real opcodes with real CRC-8s and enforce the real
 * timing rules: read before the execution delay has elapsed and you get a
 * NACK, exactly as the hardware does. That is the point of the exercise -- if
 * a driver forgets a delay or mis-frames an argument, the simulator fails in
 * the same place the bench would.
 *
 * Virtual time: delay_ms() advances the clock instead of sleeping, unless
 * realtime_scale is non-zero, in which case it also sleeps so the HTTP
 * dashboard can be watched live.
 */
#ifndef VIRTUAL_I2C_H
#define VIRTUAL_I2C_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "aeris/i2c_hal.h"
#include "room_model.h"

typedef struct {
    bool     present;
    bool     measuring;
    bool     idle_ok;          /* commands needing idle are accepted      */
    uint16_t pending_cmd;
    uint32_t cmd_ready_at_ms;  /* reads before this NACK                  */
    size_t   reply_len;
    uint8_t  reply[32];
    uint32_t last_measure_ms;
    uint64_t serial;
    float    temp_offset_c;
    uint16_t altitude_m;
    bool     asc;
    /* SGP41 holds the last compensation it was given so the harness can
     * assert that live RH/T actually reached the sensor. */
    uint16_t last_rh_ticks, last_t_ticks;
    uint32_t measure_count;
} vdev_t;

typedef struct {
    room_t  *room;
    uint32_t now_ms;
    /* Wall-clock pacing. 0 runs as fast as the CPU allows (batch modes);
     * N makes virtual time run N times faster than real time, so --serve 60
     * shows a minute of room behaviour every second. */
    uint32_t time_speedup;

    vdev_t scd40, sgp41, sht31, pms;

    /* Sub-millisecond sleep remainder carried between delays, so a long run at
     * a high speed-up does not drift by accumulating truncated sleeps. */
    uint64_t sleep_debt_ns;

    /* Fault injection. */
    uint32_t nack_every;       /* every Nth transaction NACKs, 0 = never  */
    uint32_t corrupt_every;    /* every Nth reply gets a flipped bit      */
    uint32_t txn_count;
    uint32_t injected_nacks;
    uint32_t injected_corruptions;

    uint32_t rng;
    aeris_i2c_t hal;
} vbus_t;

void vbus_init(vbus_t *b, room_t *room);
/* Convenience: make a sensor absent to test degraded operation. */
void vbus_set_present(vbus_t *b, uint8_t addr, bool present);

const aeris_i2c_t *vbus_hal(vbus_t *b);

/* Sleep for the real time dt_ms of virtual time should take at the
 * configured speed-up. A no-op when time_speedup is 0. */
void vbus_pace(vbus_t *b, uint32_t dt_ms);

#endif /* VIRTUAL_I2C_H */
