/* Protocol-level tests for the SCD40 driver, run against the virtual sensor.
 * These are the tests that justify calling the driver "register-level": they
 * assert on the bytes that go out, the delays that must precede a read, and
 * the CRC that must be checked on the way back.
 */
#include <string.h>
#include "aeris/scd40.h"
#include "aeris/crc8.h"
#include "room_model.h"
#include "test.h"
#include "virtual_i2c.h"

/* A recording bus that captures the last write so the framing can be asserted
 * byte for byte, independent of the sensor model. */
typedef struct {
    uint8_t  last_write[16];
    size_t   last_len;
    uint8_t  canned[16];
    size_t   canned_len;
    uint32_t now;
    uint32_t delays;
    uint32_t total_delay_ms;
    bool     fail_read;
} rec_t;

static aeris_err_t rec_write(void *ctx, uint8_t addr, const uint8_t *b, size_t n)
{
    rec_t *r = ctx; (void)addr;
    if (n > sizeof r->last_write) return AERIS_ERR_ARG;
    memcpy(r->last_write, b, n);
    r->last_len = n;
    return AERIS_OK;
}
static aeris_err_t rec_read(void *ctx, uint8_t addr, uint8_t *b, size_t n)
{
    rec_t *r = ctx; (void)addr;
    if (r->fail_read) return AERIS_ERR_IO;
    if (n > r->canned_len) return AERIS_ERR_IO;
    memcpy(b, r->canned, n);
    return AERIS_OK;
}
static void rec_delay(void *ctx, uint32_t ms)
{
    rec_t *r = ctx; r->now += ms; r->delays++; r->total_delay_ms += ms;
}
static uint32_t rec_now(void *ctx) { return ((rec_t *)ctx)->now; }

static void rec_bus(rec_t *r, aeris_i2c_t *bus)
{
    memset(r, 0, sizeof *r);
    bus->write = rec_write; bus->read = rec_read;
    bus->delay_ms = rec_delay; bus->now_ms = rec_now; bus->ctx = r;
}

void suite_scd40(void)
{
    SUITE("scd40");

    /* ---- conversions against the datasheet's worked example ----------- */
    /* Datasheet 3.5.2: raw 0x6667 -> 25.0 C, raw 0x5EB9 -> 37.0 %RH. */
    CHECK_NEAR(scd40_decode_temperature(0x6667), 25.0f, 0.02f);
    CHECK_NEAR(scd40_decode_humidity(0x5EB9),    37.0f, 0.02f);
    CHECK_NEAR(scd40_decode_temperature(0x0000), -45.0f, 0.001f);
    CHECK_NEAR(scd40_decode_temperature(0xFFFF), 130.0f, 0.001f);
    CHECK_NEAR(scd40_decode_humidity(0xFFFF),    100.0f, 0.001f);

    /* Offset is a span, not an absolute: ticks = C * 2^16 / 175. */
    CHECK_EQ_I(scd40_encode_temp_offset(4.0f), 1498);
    CHECK_EQ_I(scd40_encode_temp_offset(0.0f), 0);
    /* Out-of-range input must clamp, not wrap into a huge offset. */
    CHECK_EQ_I(scd40_encode_temp_offset(-5.0f), 0);
    CHECK_EQ_I(scd40_encode_temp_offset(1000.0f), 65535);

    /* ---- command framing --------------------------------------------- */
    rec_t rec; aeris_i2c_t bus; rec_bus(&rec, &bus);
    scd40_t d; scd40_init(&d, &bus, 0);
    CHECK_EQ_I(d.addr, 0x62);

    (void)scd40_start_periodic(&d);
    CHECK_EQ_I(rec.last_len, 2);
    CHECK_EQ_I(rec.last_write[0], 0x21);
    CHECK_EQ_I(rec.last_write[1], 0xB1);

    /* An argument command is opcode + word + CRC-8 of that word only. */
    (void)scd40_set_temperature_offset(&d, 4.0f);
    CHECK_EQ_I(rec.last_len, 5);
    CHECK_EQ_I(rec.last_write[0], 0x24);
    CHECK_EQ_I(rec.last_write[1], 0x1D);
    CHECK_EQ_I((rec.last_write[2] << 8) | rec.last_write[3], 1498);
    CHECK_EQ_I(rec.last_write[4], aeris_crc8(&rec.last_write[2], 2));

    /* Ambient pressure is sent in hectopascals, not pascals. */
    (void)scd40_set_ambient_pressure(&d, 98700);
    CHECK_EQ_I((rec.last_write[2] << 8) | rec.last_write[3], 987);
    CHECK_EQ_I(scd40_set_ambient_pressure(&d, 0xFFFFFFFF), AERIS_ERR_ARG);

    /* Stop must wait the full 500 ms the part needs, or the next command is
     * swallowed. This assertion is why the delay is in the driver and not in
     * the caller. */
    rec.total_delay_ms = 0;
    (void)scd40_stop_periodic(&d);
    CHECK_EQ_I(rec.total_delay_ms, SCD40_T_EXEC_STOP_MS);

    /* ---- reading, and CRC enforcement -------------------------------- */
    /* Canned reply: 500 ppm, 25.0 C, 37.0 %RH, all with correct CRCs. */
    rec.canned_len = 0;
    rec.canned_len += aeris_put_word_crc(&rec.canned[0], 500);
    rec.canned_len += aeris_put_word_crc(&rec.canned[3], 0x6667);
    rec.canned_len += aeris_put_word_crc(&rec.canned[6], 0x5EB9);

    scd40_sample_t s;
    CHECK_EQ_I(scd40_read_measurement(&d, &s), AERIS_OK);
    CHECK_EQ_I(s.co2_ppm, 500);
    CHECK_NEAR(s.temperature_c, 25.0f, 0.02f);
    CHECK_NEAR(s.humidity_rh,   37.0f, 0.02f);

    /* Flip a data bit: the driver must reject it and count it, not hand a
     * plausible 756 ppm upstream. */
    uint32_t before = d.crc_errors;
    rec.canned[1] ^= 0x04;
    CHECK_EQ_I(scd40_read_measurement(&d, &s), AERIS_ERR_CRC);
    CHECK_EQ_I(d.crc_errors, before + 1);
    rec.canned[1] ^= 0x04;

    /* A CRC-valid but impossible CO2 word is still rejected: 0 means the part
     * has not calibrated yet. */
    aeris_put_word_crc(&rec.canned[0], 0);
    CHECK_EQ_I(scd40_read_measurement(&d, &s), AERIS_ERR_RANGE);
    aeris_put_word_crc(&rec.canned[0], 40001);
    CHECK_EQ_I(scd40_read_measurement(&d, &s), AERIS_ERR_RANGE);
    aeris_put_word_crc(&rec.canned[0], 500);

    /* Bus failure must propagate, and be counted separately from CRC. */
    before = d.io_errors;
    rec.fail_read = true;
    CHECK_EQ_I(scd40_read_measurement(&d, &s), AERIS_ERR_IO);
    CHECK_EQ_I(d.io_errors, before + 1);
    rec.fail_read = false;

    /* data_ready looks at the low 11 bits only. */
    bool ready = true;
    aeris_put_word_crc(&rec.canned[0], 0x8000);   /* high bit set, low clear */
    rec.canned_len = 3;
    CHECK_EQ_I(scd40_data_ready(&d, &ready), AERIS_OK);
    CHECK(!ready);
    aeris_put_word_crc(&rec.canned[0], 0x8001);
    CHECK_EQ_I(scd40_data_ready(&d, &ready), AERIS_OK);
    CHECK(ready);

    /* ---- against the full sensor model ------------------------------- */
    room_t room; room_init(&room, 1234);
    vbus_t vb;   vbus_init(&vb, &room);
    scd40_t v;   scd40_init(&v, vbus_hal(&vb), 0);

    CHECK_EQ_I(scd40_probe(&v), AERIS_OK);
    CHECK_EQ_I(v.serial, 0x1A2B3C4D5E6Full);
    CHECK_EQ_I(scd40_self_test(&v), AERIS_OK);

    /* The model refuses the serial-number command while measuring, exactly as
     * the part does; the driver's probe stops the sensor first, so this must
     * still succeed on a warm start. */
    CHECK_EQ_I(scd40_start_periodic(&v), AERIS_OK);
    CHECK_EQ_I(scd40_probe(&v), AERIS_OK);

    /* Offset round trip through the sensor. */
    CHECK_EQ_I(scd40_set_temperature_offset(&v, 4.0f), AERIS_OK);
    float back = 0.0f;
    CHECK_EQ_I(scd40_get_temperature_offset(&v, &back), AERIS_OK);
    CHECK_NEAR(back, 4.0f, 0.01f);

    /* ASC round trip. */
    CHECK_EQ_I(scd40_set_asc_enabled(&v, false), AERIS_OK);
    bool asc = true;
    CHECK_EQ_I(scd40_get_asc_enabled(&v, &asc), AERIS_OK);
    CHECK(!asc);

    /* A real measurement, after waiting out the 5 s interval. */
    CHECK_EQ_I(scd40_start_periodic(&v), AERIS_OK);
    aeris_delay_ms(vbus_hal(&vb), SCD40_MEASURE_INTERVAL_MS + 10);
    bool rdy = false;
    CHECK_EQ_I(scd40_data_ready(&v, &rdy), AERIS_OK);
    CHECK(rdy);
    scd40_sample_t vs;
    CHECK_EQ_I(scd40_read_measurement(&v, &vs), AERIS_OK);
    CHECK_MSG(vs.co2_ppm > 380 && vs.co2_ppm < 700,
              "co2 from model out of plausible range: %u", vs.co2_ppm);
    /* Having set a 4 C offset against the model's 4.3 C self-heating, the
     * reported temperature should land near the true room temperature. */
    CHECK_NEAR(vs.temperature_c, room.temp_c + 0.3f, 0.6f);

    /* After consuming the sample the flag must clear. */
    CHECK_EQ_I(scd40_data_ready(&v, &rdy), AERIS_OK);
    CHECK(!rdy);

    /* An absent part must report an I/O error, not hang or return stale data. */
    vbus_set_present(&vb, SCD40_I2C_ADDR, false);
    CHECK_EQ_I(scd40_probe(&v), AERIS_ERR_IO);
}
