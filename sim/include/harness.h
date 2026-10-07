/* harness.h - room + virtual bus + the real application core, wired together.
 *
 * Shared by every simulator mode so there is exactly one place where the
 * firmware's core is connected to the fake hardware.
 */
#ifndef HARNESS_H
#define HARNESS_H

#include <stdbool.h>
#include <stdint.h>
#include "aeris/app.h"
#include "room_model.h"
#include "scenario.h"
#include "virtual_i2c.h"

typedef struct {
    room_t      room;
    vbus_t      bus;
    scenario_t  sc;
    aeris_app_t app;
} harness_t;

void harness_init(harness_t *h, uint32_t seed);

/* One application step. Advances virtual time by the delay the app asked for,
 * stepping the room physics across it -- the room does not wait for I2C.
 * Returns false when the scenario has run out of events. */
bool harness_tick(harness_t *h);

#endif /* HARNESS_H */
