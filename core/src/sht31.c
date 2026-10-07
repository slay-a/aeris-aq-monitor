#include <string.h>
#include "aeris/sht31.h"
#include "aeris/crc8.h"

static aeris_err_t cmd_send(sht31_t *d, uint16_t cmd)
{
    uint8_t tx[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    aeris_err_t e = aeris_i2c_write(d->bus, d->addr, tx, sizeof tx);
    if (e != AERIS_OK) d->io_errors++;
    return e;
}

/* T[C]  = -45 + 175 * raw / 2^16-1
 * RH[%] =       100 * raw / 2^16-1                   (SHT3x datasheet 4.13) */
float sht31_decode_temperature(uint16_t raw)
{
    return -45.0f + 175.0f * ((float)raw / 65535.0f);
}
float sht31_decode_humidity(uint16_t raw)
{
    return 100.0f * ((float)raw / 65535.0f);
}

void sht31_init(sht31_t *d, const aeris_i2c_t *bus, uint8_t addr)
{
    memset(d, 0, sizeof *d);
    d->bus  = bus;
    d->addr = addr ? addr : SHT31_I2C_ADDR_A;
}

aeris_err_t sht31_soft_reset(sht31_t *d)
{
    aeris_err_t e = cmd_send(d, SHT31_CMD_SOFT_RESET);
    if (e == AERIS_OK) aeris_delay_ms(d->bus, SHT31_T_RESET_MS);
    return e;
}

aeris_err_t sht31_read_status(sht31_t *d, uint16_t *status)
{
    aeris_err_t e = cmd_send(d, SHT31_CMD_READ_STATUS);
    if (e != AERIS_OK) return e;
    aeris_delay_ms(d->bus, 1);

    uint8_t rx[3];
    e = aeris_i2c_read(d->bus, d->addr, rx, sizeof rx);
    if (e != AERIS_OK) { d->io_errors++; return e; }

    e = aeris_take_words_crc(rx, status, 1);
    if (e == AERIS_ERR_CRC) d->crc_errors++;
    return e;
}

aeris_err_t sht31_measure(sht31_t *d, sht31_sample_t *out)
{
    aeris_err_t e = cmd_send(d, SHT31_CMD_MEAS_HIGHREP_NOSTRETCH);
    if (e != AERIS_OK) return e;

    /* No clock stretching in this mode, so the wait is ours to honour. */
    aeris_delay_ms(d->bus, SHT31_T_MEAS_HIGHREP_MS);

    uint8_t rx[6];
    e = aeris_i2c_read(d->bus, d->addr, rx, sizeof rx);
    if (e != AERIS_OK) { d->io_errors++; return e; }

    uint16_t w[2];
    e = aeris_take_words_crc(rx, w, 2);
    if (e != AERIS_OK) { if (e == AERIS_ERR_CRC) d->crc_errors++; return e; }

    out->temperature_c = sht31_decode_temperature(w[0]);
    out->humidity_rh   = sht31_decode_humidity(w[1]);
    return AERIS_OK;
}

aeris_err_t sht31_heater(sht31_t *d, bool on)
{
    aeris_err_t e = cmd_send(d, on ? SHT31_CMD_HEATER_ON
                                   : SHT31_CMD_HEATER_OFF);
    if (e == AERIS_OK) aeris_delay_ms(d->bus, 1);
    return e;
}
