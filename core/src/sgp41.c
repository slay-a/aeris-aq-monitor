#include <math.h>
#include <string.h>
#include "aeris/sgp41.h"
#include "aeris/crc8.h"

static aeris_err_t cmd_send(sgp41_t *d, uint16_t cmd)
{
    uint8_t tx[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    aeris_err_t e = aeris_i2c_write(d->bus, d->addr, tx, sizeof tx);
    if (e != AERIS_OK) d->io_errors++;
    return e;
}

/* Opcode + two compensation words (each with CRC) + delay + n_words read.
 * Both measure_raw_signals and execute_conditioning use this exact shape;
 * they differ only in opcode and in how many words come back. */
static aeris_err_t cmd_measure(sgp41_t *d, uint16_t cmd,
                               uint16_t rh_ticks, uint16_t t_ticks,
                               uint32_t delay_ms,
                               uint16_t *words, size_t n_words)
{
    uint8_t tx[8];
    tx[0] = (uint8_t)(cmd >> 8);
    tx[1] = (uint8_t)(cmd & 0xFF);
    aeris_put_word_crc(&tx[2], rh_ticks);
    aeris_put_word_crc(&tx[5], t_ticks);

    aeris_err_t e = aeris_i2c_write(d->bus, d->addr, tx, sizeof tx);
    if (e != AERIS_OK) { d->io_errors++; return e; }

    aeris_delay_ms(d->bus, delay_ms);

    uint8_t rx[6];
    if (n_words * 3 > sizeof rx) return AERIS_ERR_ARG;
    e = aeris_i2c_read(d->bus, d->addr, rx, n_words * 3);
    if (e != AERIS_OK) { d->io_errors++; return e; }

    e = aeris_take_words_crc(rx, words, n_words);
    if (e == AERIS_ERR_CRC) d->crc_errors++;
    return e;
}

static aeris_err_t cmd_read(sgp41_t *d, uint16_t cmd, uint32_t delay_ms,
                            uint16_t *words, size_t n_words)
{
    aeris_err_t e = cmd_send(d, cmd);
    if (e != AERIS_OK) return e;
    aeris_delay_ms(d->bus, delay_ms);

    uint8_t rx[9];
    if (n_words * 3 > sizeof rx) return AERIS_ERR_ARG;
    e = aeris_i2c_read(d->bus, d->addr, rx, n_words * 3);
    if (e != AERIS_OK) { d->io_errors++; return e; }

    e = aeris_take_words_crc(rx, words, n_words);
    if (e == AERIS_ERR_CRC) d->crc_errors++;
    return e;
}

/* ---- compensation encoding -------------------------------------------- */

uint16_t sgp41_encode_rh(float rh_percent)
{
    if (!(rh_percent == rh_percent)) return SGP41_DEFAULT_RH_TICKS; /* NaN */
    if (rh_percent < 0.0f)   rh_percent = 0.0f;
    if (rh_percent > 100.0f) rh_percent = 100.0f;
    return (uint16_t)(rh_percent * 65535.0f / 100.0f + 0.5f);
}

uint16_t sgp41_encode_temp(float temp_c)
{
    if (!(temp_c == temp_c)) return SGP41_DEFAULT_T_TICKS;          /* NaN */
    if (temp_c < -45.0f) temp_c = -45.0f;
    if (temp_c > 130.0f) temp_c = 130.0f;
    return (uint16_t)((temp_c + 45.0f) * 65535.0f / 175.0f + 0.5f);
}

/* ---- public API ------------------------------------------------------- */

void sgp41_init(sgp41_t *d, const aeris_i2c_t *bus, uint8_t addr)
{
    memset(d, 0, sizeof *d);
    d->bus  = bus;
    d->addr = addr ? addr : SGP41_I2C_ADDR;
    d->conditioning_started_ms = 0;
}

aeris_err_t sgp41_probe(sgp41_t *d)
{
    uint16_t w[3];
    aeris_err_t e = cmd_read(d, SGP41_CMD_GET_SERIAL_NUMBER,
                             SGP41_T_SHORT_MS, w, 3);
    if (e != AERIS_OK) return e;
    d->serial = ((uint64_t)w[0] << 32) | ((uint64_t)w[1] << 16) | w[2];
    return AERIS_OK;
}

aeris_err_t sgp41_self_test(sgp41_t *d)
{
    uint16_t w;
    aeris_err_t e = cmd_read(d, SGP41_CMD_EXECUTE_SELF_TEST,
                             SGP41_T_SELF_TEST_MS, &w, 1);
    if (e != AERIS_OK) return e;
    /* Low two bits flag the VOC and NOx pixels; the high byte is fixed. */
    return ((w & 0x0003u) == 0u) ? AERIS_OK : AERIS_ERR_SELFTEST;
}

aeris_err_t sgp41_condition_step(sgp41_t *d, uint16_t *voc_out)
{
    uint32_t now = aeris_now_ms(d->bus);
    if (d->conditioning_started_ms == 0) d->conditioning_started_ms = now;

    uint16_t w;
    aeris_err_t e = cmd_measure(d, SGP41_CMD_EXECUTE_CONDITIONING,
                                SGP41_DEFAULT_RH_TICKS, SGP41_DEFAULT_T_TICKS,
                                SGP41_T_CONDITION_MS, &w, 1);
    if (e != AERIS_OK) return e;
    if (voc_out) *voc_out = w;

    if (now - d->conditioning_started_ms >= SGP41_CONDITION_TOTAL_MS) {
        d->conditioned = true;
    }
    return AERIS_OK;
}

bool sgp41_is_conditioned(const sgp41_t *d) { return d->conditioned; }

aeris_err_t sgp41_measure(sgp41_t *d, float rh_percent, float temp_c,
                          sgp41_raw_t *out)
{
    uint16_t w[2];
    aeris_err_t e = cmd_measure(d, SGP41_CMD_MEASURE_RAW_SIGNALS,
                                sgp41_encode_rh(rh_percent),
                                sgp41_encode_temp(temp_c),
                                SGP41_T_MEASURE_MS, w, 2);
    if (e != AERIS_OK) return e;
    out->sraw_voc = w[0];
    out->sraw_nox = w[1];
    return AERIS_OK;
}

aeris_err_t sgp41_heater_off(sgp41_t *d)
{
    aeris_err_t e = cmd_send(d, SGP41_CMD_TURN_HEATER_OFF);
    if (e == AERIS_OK) aeris_delay_ms(d->bus, SGP41_T_SHORT_MS);
    return e;
}
