/* SGP41 tests. The important one is compensation: the driver must put the live
 * RH and T on the wire, in the datasheet's tick encoding, with a correct CRC
 * on each -- and must fall back to the documented defaults rather than sending
 * garbage when the SHT31 has not reported.
 */
#include <math.h>
#include <string.h>
#include "aeris/crc8.h"
#include "aeris/sgp41.h"
#include "room_model.h"
#include "test.h"
#include "virtual_i2c.h"

void suite_sgp41(void)
{
    SUITE("sgp41");

    /* ---- tick encoding ------------------------------------------------ */
    /* The datasheet's own defaults are the test vectors: 50 %RH is 0x8000 and
     * 25 C is 0x6666. */
    CHECK_EQ_I(sgp41_encode_rh(50.0f),   SGP41_DEFAULT_RH_TICKS);
    CHECK_EQ_I(sgp41_encode_temp(25.0f), SGP41_DEFAULT_T_TICKS);
    CHECK_EQ_I(sgp41_encode_rh(0.0f),    0);
    CHECK_EQ_I(sgp41_encode_rh(100.0f),  65535);
    CHECK_EQ_I(sgp41_encode_temp(-45.0f), 0);
    CHECK_EQ_I(sgp41_encode_temp(130.0f), 65535);

    /* Out-of-range input clamps instead of wrapping. */
    CHECK_EQ_I(sgp41_encode_rh(140.0f), 65535);
    CHECK_EQ_I(sgp41_encode_rh(-20.0f), 0);

    /* NaN -- the honest representation of "the SHT31 has not answered yet" --
     * must produce the datasheet default, not 0 (which the sensor would read
     * as bone-dry air and over-correct against). */
    float nan_f = nanf("");
    CHECK_EQ_I(sgp41_encode_rh(nan_f),   SGP41_DEFAULT_RH_TICKS);
    CHECK_EQ_I(sgp41_encode_temp(nan_f), SGP41_DEFAULT_T_TICKS);

    /* ---- the wire format, via the sensor model ----------------------- */
    room_t room; room_init(&room, 77);
    vbus_t vb;   vbus_init(&vb, &room);
    sgp41_t d;   sgp41_init(&d, vbus_hal(&vb), 0);
    CHECK_EQ_I(d.addr, 0x59);

    CHECK_EQ_I(sgp41_probe(&d), AERIS_OK);
    CHECK_EQ_I(d.serial, 0x0011223344ull);
    CHECK_EQ_I(sgp41_self_test(&d), AERIS_OK);

    /* Conditioning uses the defaults by design. */
    uint16_t voc0 = 0;
    CHECK_EQ_I(sgp41_condition_step(&d, &voc0), AERIS_OK);
    CHECK_EQ_I(vb.sgp41.last_rh_ticks, SGP41_DEFAULT_RH_TICKS);
    CHECK_EQ_I(vb.sgp41.last_t_ticks,  SGP41_DEFAULT_T_TICKS);
    CHECK(voc0 > 0);

    /* Conditioning finishes only after the datasheet's 10 s, not on the first
     * call. */
    CHECK(!sgp41_is_conditioned(&d));
    aeris_delay_ms(vbus_hal(&vb), SGP41_CONDITION_TOTAL_MS + 100);
    CHECK_EQ_I(sgp41_condition_step(&d, &voc0), AERIS_OK);
    CHECK(sgp41_is_conditioned(&d));

    /* A live measurement must put the supplied RH/T on the wire. */
    sgp41_raw_t raw;
    CHECK_EQ_I(sgp41_measure(&d, 71.5f, 9.25f, &raw), AERIS_OK);
    CHECK_EQ_I(vb.sgp41.last_rh_ticks, sgp41_encode_rh(71.5f));
    CHECK_EQ_I(vb.sgp41.last_t_ticks,  sgp41_encode_temp(9.25f));
    CHECK(vb.sgp41.last_rh_ticks != SGP41_DEFAULT_RH_TICKS);
    CHECK(raw.sraw_voc > 0 && raw.sraw_nox > 0);

    /* ---- compensation actually helps --------------------------------- */
    /* The sensor model's humidity cross-sensitivity acts on the error between
     * the compensation value sent and the true RH. Hold the room steady, sweep
     * its humidity, and compare the spread of raw ticks when compensating
     * correctly against the spread when sending the 50 %RH default. This is
     * the measurement behind the compensation claim. */
    float worst_comp = 0.0f, worst_default = 0.0f;
    float first_comp = 0.0f, first_default = 0.0f;
    const float rh_sweep[] = { 30.0f, 45.0f, 60.0f, 75.0f };
    for (int i = 0; i < 4; i++) {
        room.rh     = rh_sweep[i];
        room.voc_ppb = 100.0f;      /* identical air chemistry each time */

        sgp41_raw_t a, b;
        CHECK_EQ_I(sgp41_measure(&d, room.rh, room.temp_c, &a), AERIS_OK);
        CHECK_EQ_I(sgp41_measure(&d, 50.0f,   25.0f,       &b), AERIS_OK);
        if (i == 0) { first_comp = (float)a.sraw_voc; first_default = (float)b.sraw_voc; }
        float dc = fabsf((float)a.sraw_voc - first_comp);
        float dd = fabsf((float)b.sraw_voc - first_default);
        if (dc > worst_comp)    worst_comp = dc;
        if (dd > worst_default) worst_default = dd;
    }
    CHECK_MSG(worst_comp < worst_default * 0.25f,
              "live compensation should hold the raw signal far steadier across "
              "a 30-75 %%RH sweep: compensated spread %.0f ticks vs "
              "uncompensated %.0f ticks", (double)worst_comp, (double)worst_default);

    /* ---- framing rules ----------------------------------------------- */
    CHECK_EQ_I(sgp41_heater_off(&d), AERIS_OK);

    vbus_set_present(&vb, SGP41_I2C_ADDR, false);
    CHECK_EQ_I(sgp41_measure(&d, 50.0f, 25.0f, &raw), AERIS_ERR_IO);
    CHECK(d.io_errors > 0);
}
