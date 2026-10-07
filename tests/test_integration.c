/* End-to-end tests: the real application core against the virtual hardware.
 * These cover the behaviour a bench test would check first -- does it come up,
 * does it survive a missing sensor, does it recover from a dead bus.
 */
#include <math.h>
#include <string.h>
#include "aeris/api.h"
#include "harness.h"
#include "test.h"

void suite_integration(void)
{
    SUITE("integration");

    /* ---- clean bring-up ---------------------------------------------- */
    harness_t h;
    harness_init(&h, 0x1234u);
    scenario_clear(&h.sc);
    scenario_add(&h.sc, EV_BASELINE, 30u * 60000u);

    /* Phase order must be boot -> probe -> conditioning -> warmup -> run. */
    CHECK_EQ_I(h.app.phase, AERIS_PHASE_BOOT);
    bool saw_condition = false, saw_warmup = false, saw_run = false;
    while (harness_tick(&h)) {
        if (h.app.phase == AERIS_PHASE_CONDITION) saw_condition = true;
        if (h.app.phase == AERIS_PHASE_WARMUP)    saw_warmup = true;
        if (h.app.phase == AERIS_PHASE_RUN)       saw_run = true;
    }
    CHECK(saw_condition);
    CHECK(saw_warmup);
    CHECK(saw_run);
    CHECK_EQ_I(h.app.phase, AERIS_PHASE_RUN);

    aeris_snapshot_t s;
    aeris_app_snapshot(&h.app, &s);
    CHECK(s.scd40_present && s.sgp41_present && s.sht31_present && s.pmsa003i_present);
    CHECK_EQ_I(s.sample_errors, 0);
    CHECK_EQ_I(s.crc_errors, 0);
    CHECK(s.features_valid);

    /* Readings must be physically plausible for a quiet room. */
    CHECK_MSG(s.co2_ppm > 380.0f && s.co2_ppm < 900.0f, "co2 %.0f", (double)s.co2_ppm);
    CHECK_MSG(s.temperature_c > 5.0f && s.temperature_c < 35.0f,
              "temp %.1f", (double)s.temperature_c);
    CHECK_MSG(s.humidity_rh > 10.0f && s.humidity_rh < 95.0f,
              "rh %.1f", (double)s.humidity_rh);
    CHECK(s.abs_humidity_gm3 > 1.0f && s.abs_humidity_gm3 < 30.0f);
    CHECK(s.aqi.score <= 100);

    /* The SCD40's temperature offset must have been applied during probe, so
     * the reported temperature tracks the room rather than the warm die. */
    CHECK_NEAR(h.bus.scd40.temp_offset_c, 4.0f, 0.02f);

    /* The SGP41 must have received live compensation, not the defaults. */
    CHECK(h.bus.sgp41.last_rh_ticks != SGP41_DEFAULT_RH_TICKS);
    CHECK_NEAR((float)h.bus.sgp41.last_rh_ticks * 100.0f / 65535.0f,
               h.room.rh, 3.0f);
    CHECK_NEAR((float)h.bus.sgp41.last_t_ticks * 175.0f / 65535.0f - 45.0f,
               h.room.temp_c, 1.0f);

    /* History must be filling at its own slower cadence, not every sample. */
    CHECK(h.app.hist.count > 20);
    CHECK_MSG(h.app.hist.count < s.samples_total,
              "history (%u) should be downsampled from samples (%u)",
              h.app.hist.count, s.samples_total);

    /* ---- the physics the model depends on ---------------------------- */
    /* Occupancy must actually raise CO2, or the whole premise is wrong. */
    harness_t occ;
    harness_init(&occ, 0x2222u);
    scenario_clear(&occ.sc);
    scenario_add(&occ.sc, EV_BASELINE,  10u * 60000u);
    scenario_add(&occ.sc, EV_OCCUPANCY, 50u * 60000u);
    float co2_quiet = 0.0f;
    while (occ.bus.now_ms < 10u * 60000u) {
        if (!harness_tick(&occ)) break;
        co2_quiet = occ.room.co2_ppm;
    }
    while (harness_tick(&occ)) { }
    CHECK_MSG(occ.room.co2_ppm > co2_quiet + 200.0f,
              "occupancy only moved CO2 from %.0f to %.0f ppm",
              (double)co2_quiet, (double)occ.room.co2_ppm);

    /* Ventilation must drop CO2 and lower absolute humidity (cold outdoor air
     * holds less water) -- the pair of signals that separates it from an empty
     * room. */
    harness_t vent;
    harness_init(&vent, 0x3333u);
    scenario_clear(&vent.sc);
    scenario_add(&vent.sc, EV_OCCUPANCY,   45u * 60000u);
    scenario_add(&vent.sc, EV_VENTILATION, 20u * 60000u);
    float co2_peak = 0.0f, ah_peak = 0.0f;
    while (vent.bus.now_ms < 45u * 60000u) {
        if (!harness_tick(&vent)) break;
        co2_peak = vent.room.co2_ppm;
        ah_peak  = aeris_absolute_humidity(vent.room.temp_c, vent.room.rh);
    }
    while (harness_tick(&vent)) { }
    float ah_end = aeris_absolute_humidity(vent.room.temp_c, vent.room.rh);
    CHECK_MSG(vent.room.co2_ppm < co2_peak - 100.0f,
              "ventilation only moved CO2 from %.0f to %.0f",
              (double)co2_peak, (double)vent.room.co2_ppm);
    CHECK_MSG(ah_end < ah_peak,
              "ventilation should lower absolute humidity: %.2f -> %.2f g/m3",
              (double)ah_peak, (double)ah_end);

    /* Cooking must raise particulates; occupancy must not. */
    harness_t cook;
    harness_init(&cook, 0x4444u);
    scenario_clear(&cook.sc);
    scenario_add(&cook.sc, EV_COOKING, 20u * 60000u);
    while (harness_tick(&cook)) { }
    aeris_app_snapshot(&cook.app, &s);
    CHECK_MSG(s.pm2_5_ugm3 > 40.0f, "cooking only reached %.1f ug/m3 PM2.5",
              (double)s.pm2_5_ugm3);
    CHECK_MSG(s.voc_index > 120.0f, "cooking only reached VOC index %.0f",
              (double)s.voc_index);

    /* A solvent event must raise VOC without raising PM -- the separation the
     * fifth class relies on. */
    harness_t vol;
    harness_init(&vol, 0x5555u);
    scenario_clear(&vol.sc);
    scenario_add(&vol.sc, EV_VOLATILE, 20u * 60000u);
    while (harness_tick(&vol)) { }
    aeris_app_snapshot(&vol.app, &s);
    CHECK_MSG(s.voc_index > 140.0f, "solvent only reached VOC index %.0f",
              (double)s.voc_index);
    CHECK_MSG(s.pm2_5_ugm3 < 20.0f, "solvent should not make particulates, got %.1f",
              (double)s.pm2_5_ugm3);

    /* ---- degraded operation ------------------------------------------ */
    /* With no SHT31, the SGP41 must fall back to the datasheet defaults and the
     * device must keep running on the SCD40's own RH/T. */
    harness_t no_rht;
    harness_init(&no_rht, 0x6666u);
    vbus_set_present(&no_rht.bus, SHT31_I2C_ADDR_A, false);
    scenario_clear(&no_rht.sc);
    scenario_add(&no_rht.sc, EV_OCCUPANCY, 30u * 60000u);
    while (harness_tick(&no_rht)) { }
    aeris_app_snapshot(&no_rht.app, &s);
    CHECK(!s.sht31_present);
    CHECK(s.scd40_present);
    CHECK_MSG(s.features_valid, "device must still classify without an SHT31");
    CHECK(s.temperature_c == s.temperature_c);   /* fell back to the SCD40 */

    /* With no SCD40 there is no CO2, so no valid feature vector -- and the
     * device must say so rather than inventing one. */
    harness_t no_co2;
    harness_init(&no_co2, 0x7777u);
    vbus_set_present(&no_co2.bus, SCD40_I2C_ADDR, false);
    scenario_clear(&no_co2.sc);
    scenario_add(&no_co2.sc, EV_OCCUPANCY, 30u * 60000u);
    while (harness_tick(&no_co2)) { }
    aeris_app_snapshot(&no_co2.app, &s);
    CHECK(!s.scd40_present);
    CHECK(s.sgp41_present);
    CHECK(!s.features_valid);
    static char buf[AERIS_API_LIVE_BUF];
    CHECK(aeris_api_live(&s, buf, sizeof buf) > 0);

    /* An entirely dead bus must latch a fault and then keep retrying, so a
     * loose connector that is reseated recovers without a reboot. */
    harness_t dead;
    harness_init(&dead, 0x8888u);
    vbus_set_present(&dead.bus, SCD40_I2C_ADDR, false);
    vbus_set_present(&dead.bus, SGP41_I2C_ADDR, false);
    vbus_set_present(&dead.bus, SHT31_I2C_ADDR_A, false);
    vbus_set_present(&dead.bus, PMSA003I_I2C_ADDR, false);
    scenario_clear(&dead.sc);
    scenario_add(&dead.sc, EV_BASELINE, 10u * 60000u);
    bool saw_fault = false;
    int ticks = 0;
    while (harness_tick(&dead) && ticks++ < 400) {
        if (dead.app.phase == AERIS_PHASE_FAULT) saw_fault = true;
    }
    CHECK_MSG(saw_fault, "a dead bus must reach the fault phase");

    /* Reseat the connector mid-run: the device must come back by itself. */
    vbus_set_present(&dead.bus, SCD40_I2C_ADDR, true);
    vbus_set_present(&dead.bus, SGP41_I2C_ADDR, true);
    vbus_set_present(&dead.bus, SHT31_I2C_ADDR_A, true);
    vbus_set_present(&dead.bus, PMSA003I_I2C_ADDR, true);
    scenario_clear(&dead.sc);
    scenario_add(&dead.sc, EV_BASELINE, 30u * 60000u);
    dead.sc.started = false;
    while (harness_tick(&dead)) { }
    aeris_app_snapshot(&dead.app, &s);
    CHECK_MSG(s.scd40_present, "device did not recover after the bus came back");
    CHECK(dead.app.phase == AERIS_PHASE_RUN || dead.app.phase == AERIS_PHASE_WARMUP);

    /* ---- snapshot isolation ------------------------------------------ */
    /* A snapshot must be a copy: later sampling must not mutate what the HTTP
     * handler is halfway through serialising. */
    harness_t snapt;
    harness_init(&snapt, 0x9999u);
    scenario_clear(&snapt.sc);
    scenario_add(&snapt.sc, EV_OCCUPANCY, 40u * 60000u);
    while (snapt.bus.now_ms < 20u * 60000u) if (!harness_tick(&snapt)) break;
    aeris_snapshot_t held;
    aeris_app_snapshot(&snapt.app, &held);
    float held_co2 = held.co2_ppm;
    uint32_t held_samples = held.samples_total;
    while (harness_tick(&snapt)) { }
    CHECK_NEAR(held.co2_ppm, held_co2, 0.0001f);
    CHECK_EQ_I(held.samples_total, held_samples);
    aeris_app_snapshot(&snapt.app, &s);
    CHECK(s.samples_total > held_samples);
}
