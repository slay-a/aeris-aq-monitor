#include <string.h>
#include "aeris/scd40.h"
#include "aeris/crc8.h"

/* ---- command framing ---------------------------------------------------- */

/* Send a bare 16-bit opcode. */
static aeris_err_t cmd_send(scd40_t *d, uint16_t cmd)
{
    uint8_t tx[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    aeris_err_t e = aeris_i2c_write(d->bus, d->addr, tx, sizeof tx);
    if (e != AERIS_OK) d->io_errors++;
    return e;
}

/* Send an opcode followed by one argument word and its CRC-8. */
static aeris_err_t cmd_send_arg(scd40_t *d, uint16_t cmd, uint16_t arg)
{
    uint8_t tx[5];
    tx[0] = (uint8_t)(cmd >> 8);
    tx[1] = (uint8_t)(cmd & 0xFF);
    aeris_put_word_crc(&tx[2], arg);
    aeris_err_t e = aeris_i2c_write(d->bus, d->addr, tx, sizeof tx);
    if (e != AERIS_OK) d->io_errors++;
    return e;
}

/* Send an opcode, wait the datasheet execution time, then read n_words
 * [msb, lsb, crc] triplets and validate every CRC. */
static aeris_err_t cmd_read(scd40_t *d, uint16_t cmd, uint32_t delay_ms,
                            uint16_t *words, size_t n_words)
{
    uint8_t rx[9];                       /* longest read we issue: 3 words */
    if (n_words * 3 > sizeof rx) return AERIS_ERR_ARG;

    aeris_err_t e = cmd_send(d, cmd);
    if (e != AERIS_OK) return e;

    aeris_delay_ms(d->bus, delay_ms);

    e = aeris_i2c_read(d->bus, d->addr, rx, n_words * 3);
    if (e != AERIS_OK) { d->io_errors++; return e; }

    e = aeris_take_words_crc(rx, words, n_words);
    if (e == AERIS_ERR_CRC) d->crc_errors++;
    return e;
}

/* Write-with-argument then read a reply: used only by forced recalibration. */
static aeris_err_t cmd_write_read(scd40_t *d, uint16_t cmd, uint16_t arg,
                                  uint32_t delay_ms, uint16_t *word)
{
    aeris_err_t e = cmd_send_arg(d, cmd, arg);
    if (e != AERIS_OK) return e;
    aeris_delay_ms(d->bus, delay_ms);

    uint8_t rx[3];
    e = aeris_i2c_read(d->bus, d->addr, rx, sizeof rx);
    if (e != AERIS_OK) { d->io_errors++; return e; }
    e = aeris_take_words_crc(rx, word, 1);
    if (e == AERIS_ERR_CRC) d->crc_errors++;
    return e;
}

/* ---- conversions ------------------------------------------------------- */
/* T[C]  = -45 + 175 * raw / 2^16-1
 * RH[%] =       100 * raw / 2^16-1                       (datasheet 3.5.2) */

float scd40_decode_temperature(uint16_t raw)
{
    return -45.0f + 175.0f * ((float)raw / 65535.0f);
}

float scd40_decode_humidity(uint16_t raw)
{
    return 100.0f * ((float)raw / 65535.0f);
}

/* Offset is sent in the same ticks as temperature, but as a span not an
 * absolute: offset_ticks = offset_C * 2^16 / 175. */
uint16_t scd40_encode_temp_offset(float offset_c)
{
    if (offset_c < 0.0f) offset_c = 0.0f;
    if (offset_c > 175.0f) offset_c = 175.0f;
    float ticks = offset_c * 65536.0f / 175.0f;
    if (ticks > 65535.0f) ticks = 65535.0f;
    return (uint16_t)(ticks + 0.5f);
}

/* ---- public API -------------------------------------------------------- */

void scd40_init(scd40_t *d, const aeris_i2c_t *bus, uint8_t addr)
{
    memset(d, 0, sizeof *d);
    d->bus  = bus;
    d->addr = addr ? addr : SCD40_I2C_ADDR;
}

aeris_err_t scd40_probe(scd40_t *d)
{
    /* The serial number command is only valid while idle. Stopping an
     * already-idle sensor is harmless, and it means a warm reset of the ESP32
     * mid-measurement still lands us in a known state. */
    (void)scd40_stop_periodic(d);

    uint16_t w[3];
    aeris_err_t e = cmd_read(d, SCD40_CMD_GET_SERIAL_NUMBER,
                             SCD40_T_EXEC_SHORT_MS, w, 3);
    if (e != AERIS_OK) return e;

    d->serial = ((uint64_t)w[0] << 32) | ((uint64_t)w[1] << 16) | w[2];
    return AERIS_OK;
}

aeris_err_t scd40_start_periodic(scd40_t *d)
{
    aeris_err_t e = cmd_send(d, SCD40_CMD_START_PERIODIC_MEASUREMENT);
    if (e == AERIS_OK) {
        aeris_delay_ms(d->bus, SCD40_T_EXEC_SHORT_MS);
        d->measuring = true;
    }
    return e;
}

aeris_err_t scd40_stop_periodic(scd40_t *d)
{
    aeris_err_t e = cmd_send(d, SCD40_CMD_STOP_PERIODIC_MEASUREMENT);
    /* 500 ms is not advisory: the part ignores traffic until it completes. */
    aeris_delay_ms(d->bus, SCD40_T_EXEC_STOP_MS);
    if (e == AERIS_OK) d->measuring = false;
    return e;
}

aeris_err_t scd40_data_ready(scd40_t *d, bool *ready)
{
    uint16_t w;
    aeris_err_t e = cmd_read(d, SCD40_CMD_GET_DATA_READY_STATUS,
                             SCD40_T_EXEC_SHORT_MS, &w, 1);
    if (e != AERIS_OK) return e;
    /* Only the low 11 bits carry the flag; all-zero means "nothing new". */
    *ready = (w & 0x07FFu) != 0u;
    return AERIS_OK;
}

aeris_err_t scd40_read_measurement(scd40_t *d, scd40_sample_t *out)
{
    uint16_t w[3];
    aeris_err_t e = cmd_read(d, SCD40_CMD_READ_MEASUREMENT,
                             SCD40_T_EXEC_SHORT_MS, w, 3);
    if (e != AERIS_OK) return e;

    out->co2_ppm       = w[0];
    out->temperature_c = scd40_decode_temperature(w[1]);
    out->humidity_rh   = scd40_decode_humidity(w[2]);

    /* A CO2 word of 0 is the sensor's way of saying "not calibrated yet",
     * and 40000 ppm is the top of its range. Both are CRC-valid but useless
     * as model input, so they are rejected here rather than downstream. */
    if (out->co2_ppm == 0 || out->co2_ppm > 40000) return AERIS_ERR_RANGE;
    return AERIS_OK;
}

aeris_err_t scd40_set_temperature_offset(scd40_t *d, float offset_c)
{
    aeris_err_t e = cmd_send_arg(d, SCD40_CMD_SET_TEMPERATURE_OFFSET,
                                 scd40_encode_temp_offset(offset_c));
    if (e == AERIS_OK) aeris_delay_ms(d->bus, SCD40_T_EXEC_SHORT_MS);
    return e;
}

aeris_err_t scd40_get_temperature_offset(scd40_t *d, float *offset_c)
{
    uint16_t w;
    aeris_err_t e = cmd_read(d, SCD40_CMD_GET_TEMPERATURE_OFFSET,
                             SCD40_T_EXEC_SHORT_MS, &w, 1);
    if (e != AERIS_OK) return e;
    *offset_c = 175.0f * ((float)w / 65536.0f);
    return AERIS_OK;
}

aeris_err_t scd40_set_sensor_altitude(scd40_t *d, uint16_t metres)
{
    aeris_err_t e = cmd_send_arg(d, SCD40_CMD_SET_SENSOR_ALTITUDE, metres);
    if (e == AERIS_OK) aeris_delay_ms(d->bus, SCD40_T_EXEC_SHORT_MS);
    return e;
}

aeris_err_t scd40_set_ambient_pressure(scd40_t *d, uint32_t pressure_pa)
{
    /* Sent in units of 100 Pa, so the whole range fits a 16-bit word. */
    uint32_t hpa = pressure_pa / 100u;
    if (hpa > 0xFFFFu) return AERIS_ERR_ARG;
    aeris_err_t e = cmd_send_arg(d, SCD40_CMD_SET_AMBIENT_PRESSURE,
                                 (uint16_t)hpa);
    if (e == AERIS_OK) aeris_delay_ms(d->bus, SCD40_T_EXEC_SHORT_MS);
    return e;
}

aeris_err_t scd40_set_asc_enabled(scd40_t *d, bool enabled)
{
    aeris_err_t e = cmd_send_arg(d, SCD40_CMD_SET_ASC_ENABLED,
                                 enabled ? 1u : 0u);
    if (e == AERIS_OK) aeris_delay_ms(d->bus, SCD40_T_EXEC_SHORT_MS);
    return e;
}

aeris_err_t scd40_get_asc_enabled(scd40_t *d, bool *enabled)
{
    uint16_t w;
    aeris_err_t e = cmd_read(d, SCD40_CMD_GET_ASC_ENABLED,
                             SCD40_T_EXEC_SHORT_MS, &w, 1);
    if (e != AERIS_OK) return e;
    *enabled = (w & 0x00FFu) != 0u;
    return AERIS_OK;
}

aeris_err_t scd40_forced_recalibration(scd40_t *d, uint16_t target_ppm,
                                       int32_t *correction_ppm)
{
    /* FRC requires 3 minutes of periodic measurement first, then a stop. */
    aeris_err_t e = scd40_stop_periodic(d);
    if (e != AERIS_OK) return e;

    uint16_t w;
    e = cmd_write_read(d, SCD40_CMD_PERFORM_FORCED_RECALIBRATION, target_ppm,
                       SCD40_T_EXEC_FRC_MS, &w);
    if (e != AERIS_OK) return e;

    if (w == 0xFFFF) return AERIS_ERR_SELFTEST;  /* sensor refused the FRC */
    if (correction_ppm) *correction_ppm = (int32_t)w - 0x8000;
    return AERIS_OK;
}

aeris_err_t scd40_persist_settings(scd40_t *d)
{
    aeris_err_t e = cmd_send(d, SCD40_CMD_PERSIST_SETTINGS);
    if (e == AERIS_OK) aeris_delay_ms(d->bus, 800);
    return e;
}

aeris_err_t scd40_self_test(scd40_t *d)
{
    uint16_t w;
    aeris_err_t e = cmd_read(d, SCD40_CMD_PERFORM_SELF_TEST,
                             SCD40_T_EXEC_SELF_TEST_MS, &w, 1);
    if (e != AERIS_OK) return e;
    return (w == 0) ? AERIS_OK : AERIS_ERR_SELFTEST;
}

aeris_err_t scd40_reinit(scd40_t *d)
{
    aeris_err_t e = scd40_stop_periodic(d);
    if (e != AERIS_OK) return e;
    e = cmd_send(d, SCD40_CMD_REINIT);
    if (e == AERIS_OK) aeris_delay_ms(d->bus, SCD40_T_EXEC_REINIT_MS);
    return e;
}
