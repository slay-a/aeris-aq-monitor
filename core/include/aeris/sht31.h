/* sht31.h - Sensirion SHT31 temperature / humidity sensor.
 *
 * Runs in single-shot high-repeatability mode with clock stretching disabled:
 * the SGP41 shares this bus and holding SCL for 15 ms would stall its own
 * timing-sensitive reads. The driver waits the 15 ms itself instead.
 */
#ifndef AERIS_SHT31_H
#define AERIS_SHT31_H

#include <stdbool.h>
#include <stdint.h>
#include "aeris/aeris_err.h"
#include "aeris/i2c_hal.h"

#define SHT31_I2C_ADDR_A 0x44   /* ADDR pin low  */
#define SHT31_I2C_ADDR_B 0x45   /* ADDR pin high */

typedef enum {
    SHT31_CMD_MEAS_HIGHREP_NOSTRETCH = 0x2400,
    SHT31_CMD_SOFT_RESET             = 0x30A2,
    SHT31_CMD_HEATER_ON              = 0x306D,
    SHT31_CMD_HEATER_OFF             = 0x3066,
    SHT31_CMD_READ_STATUS            = 0xF32D,
    SHT31_CMD_CLEAR_STATUS           = 0x3041,
} sht31_cmd_t;

#define SHT31_T_MEAS_HIGHREP_MS 15
#define SHT31_T_RESET_MS        2

typedef struct {
    const aeris_i2c_t *bus;
    uint8_t  addr;
    uint32_t crc_errors;
    uint32_t io_errors;
} sht31_t;

typedef struct {
    float temperature_c;
    float humidity_rh;
} sht31_sample_t;

void        sht31_init(sht31_t *d, const aeris_i2c_t *bus, uint8_t addr);
aeris_err_t sht31_soft_reset(sht31_t *d);
aeris_err_t sht31_read_status(sht31_t *d, uint16_t *status);
aeris_err_t sht31_measure(sht31_t *d, sht31_sample_t *out);
aeris_err_t sht31_heater(sht31_t *d, bool on);

float sht31_decode_temperature(uint16_t raw);
float sht31_decode_humidity(uint16_t raw);

#endif /* AERIS_SHT31_H */
