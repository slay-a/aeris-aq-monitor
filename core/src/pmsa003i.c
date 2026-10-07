#include <string.h>
#include "aeris/pmsa003i.h"

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

aeris_err_t pmsa003i_parse(const uint8_t *f, pmsa003i_sample_t *out)
{
    if (f[0] != PMSA003I_MAGIC_0 || f[1] != PMSA003I_MAGIC_1) {
        return AERIS_ERR_FRAME;
    }
    if (be16(&f[2]) != PMSA003I_FRAME_BODY) {
        return AERIS_ERR_FRAME;
    }

    /* Checksum is a plain 16-bit sum of bytes 0..29, stored in 30..31. */
    uint16_t sum = 0;
    for (int i = 0; i < PMSA003I_FRAME_LEN - 2; i++) sum = (uint16_t)(sum + f[i]);
    if (sum != be16(&f[30])) return AERIS_ERR_CRC;

    out->pm1_0_std = be16(&f[4]);
    out->pm2_5_std = be16(&f[6]);
    out->pm10_std  = be16(&f[8]);
    out->pm1_0_env = be16(&f[10]);
    out->pm2_5_env = be16(&f[12]);
    out->pm10_env  = be16(&f[14]);
    out->n_0_3     = be16(&f[16]);
    out->n_0_5     = be16(&f[18]);
    out->n_1_0     = be16(&f[20]);
    out->n_2_5     = be16(&f[22]);
    out->n_5_0     = be16(&f[24]);
    out->n_10_0    = be16(&f[26]);
    return AERIS_OK;
}

void pmsa003i_init(pmsa003i_t *d, const aeris_i2c_t *bus, uint8_t addr)
{
    memset(d, 0, sizeof *d);
    d->bus  = bus;
    d->addr = addr ? addr : PMSA003I_I2C_ADDR;
}

aeris_err_t pmsa003i_read(pmsa003i_t *d, pmsa003i_sample_t *out)
{
    uint8_t frame[PMSA003I_FRAME_LEN];
    aeris_err_t e = aeris_i2c_read(d->bus, d->addr, frame, sizeof frame);
    if (e != AERIS_OK) { d->io_errors++; return e; }

    e = pmsa003i_parse(frame, out);
    if (e != AERIS_OK) d->frame_errors++;
    return e;
}
