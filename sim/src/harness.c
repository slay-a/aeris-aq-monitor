#include <time.h>
#include "harness.h"

/* Real monotonic microseconds, so the inference timing the dashboard reports is
 * a genuine measurement rather than zero. It is host time, not ESP32-C3 time --
 * docs/BRINGUP.md step 9 records the on-target figure. */
static uint32_t host_us(void *ctx)
{
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull);
}

void harness_init(harness_t *h, uint32_t seed)
{
    room_init(&h->room, seed);
    vbus_init(&h->bus, &h->room);
    aeris_config_t cfg = aeris_config_default();
    aeris_app_init(&h->app, vbus_hal(&h->bus), &cfg);
    aeris_app_set_us_clock(&h->app, host_us, NULL);
    scenario_clear(&h->sc);
}

bool harness_tick(harness_t *h)
{
    if (!scenario_update(&h->sc, &h->room, h->bus.now_ms)) return false;

    uint32_t wait = aeris_app_step(&h->app);
    h->bus.now_ms += wait;
    room_step(&h->room, wait);

    vbus_pace(&h->bus, wait);
    return true;
}
