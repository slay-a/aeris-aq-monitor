/* pmsa003i.h - Plantower PMSA003I laser particulate sensor.
 *
 * Unlike the Sensirion parts this one is not command/response: it pushes a
 * fixed 32-byte frame that you read whenever you like. Framing is a 0x424D
 * magic, a length field, 13 big-endian data words and a 16-bit additive
 * checksum over the preceding 30 bytes -- all three are validated here,
 * because a short read on a shared bus otherwise decodes as a PM2.5 spike.
 */
#ifndef AERIS_PMSA003I_H
#define AERIS_PMSA003I_H

#include <stdint.h>
#include "aeris/aeris_err.h"
#include "aeris/i2c_hal.h"

#define PMSA003I_I2C_ADDR   0x12
#define PMSA003I_FRAME_LEN  32
#define PMSA003I_MAGIC_0    0x42
#define PMSA003I_MAGIC_1    0x4D
#define PMSA003I_FRAME_BODY 28   /* length field value for a valid frame */

typedef struct {
    const aeris_i2c_t *bus;
    uint8_t  addr;
    uint32_t frame_errors;
    uint32_t io_errors;
} pmsa003i_t;

typedef struct {
    uint16_t pm1_0_std,  pm2_5_std,  pm10_std;    /* ug/m3, CF=1 */
    uint16_t pm1_0_env,  pm2_5_env,  pm10_env;    /* ug/m3, atmospheric */
    uint16_t n_0_3, n_0_5, n_1_0, n_2_5, n_5_0, n_10_0;  /* per 0.1 L */
} pmsa003i_sample_t;

void        pmsa003i_init(pmsa003i_t *d, const aeris_i2c_t *bus, uint8_t addr);
aeris_err_t pmsa003i_read(pmsa003i_t *d, pmsa003i_sample_t *out);

/* Frame decode split out so the test suite can feed it captured bytes. */
aeris_err_t pmsa003i_parse(const uint8_t *frame, pmsa003i_sample_t *out);

#endif /* AERIS_PMSA003I_H */
