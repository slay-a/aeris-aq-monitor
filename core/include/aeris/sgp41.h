/* sgp41.h - Sensirion SGP41 VOC/NOx metal-oxide sensor, register level.
 *
 * The SGP41 returns raw resistance ticks, not an index. Its sensitivity is a
 * strong function of humidity and a weaker one of temperature, so every
 * measure_raw_signals command carries the *current* RH and T as arguments.
 * Feeding it the SHT31's live readings instead of the datasheet defaults
 * (50 %RH / 25 C) is what keeps the VOC signal from tracking the weather.
 *
 * Startup: the datasheet requires 10 s of execute_conditioning before the
 * first measurement, during which only SRAW_VOC is meaningful.
 */
#ifndef AERIS_SGP41_H
#define AERIS_SGP41_H

#include <stdbool.h>
#include <stdint.h>
#include "aeris/aeris_err.h"
#include "aeris/i2c_hal.h"

#define SGP41_I2C_ADDR 0x59

typedef enum {
    SGP41_CMD_EXECUTE_CONDITIONING = 0x2612,
    SGP41_CMD_MEASURE_RAW_SIGNALS  = 0x2619,
    SGP41_CMD_EXECUTE_SELF_TEST    = 0x280E,
    SGP41_CMD_TURN_HEATER_OFF      = 0x3615,
    SGP41_CMD_GET_SERIAL_NUMBER    = 0x3682,
} sgp41_cmd_t;

#define SGP41_T_MEASURE_MS       50
#define SGP41_T_CONDITION_MS     50
#define SGP41_T_SELF_TEST_MS     320
#define SGP41_T_SHORT_MS         1
#define SGP41_CONDITION_TOTAL_MS 10000   /* datasheet-mandated burn-in */

/* Datasheet default compensation words, used during conditioning and as the
 * fallback when the SHT31 has not produced a reading yet. */
#define SGP41_DEFAULT_RH_TICKS 0x8000    /* 50 %RH  */
#define SGP41_DEFAULT_T_TICKS  0x6666    /* 25 C    */

typedef struct {
    const aeris_i2c_t *bus;
    uint8_t  addr;
    uint64_t serial;
    uint32_t conditioning_started_ms;
    bool     conditioned;
    uint32_t crc_errors;
    uint32_t io_errors;
} sgp41_t;

typedef struct {
    uint16_t sraw_voc;
    uint16_t sraw_nox;
} sgp41_raw_t;

void        sgp41_init(sgp41_t *d, const aeris_i2c_t *bus, uint8_t addr);
aeris_err_t sgp41_probe(sgp41_t *d);
aeris_err_t sgp41_self_test(sgp41_t *d);

/* One conditioning step. Call every second until sgp41_is_conditioned();
 * voc_out receives the (already usable) raw VOC tick. */
aeris_err_t sgp41_condition_step(sgp41_t *d, uint16_t *voc_out);
bool        sgp41_is_conditioned(const sgp41_t *d);

/* Measure with live compensation. Pass the SHT31's readings; if either is
 * NaN the datasheet defaults are substituted. */
aeris_err_t sgp41_measure(sgp41_t *d, float rh_percent, float temp_c,
                          sgp41_raw_t *out);
aeris_err_t sgp41_heater_off(sgp41_t *d);

/* Compensation tick encoding, exposed for unit tests:
 *   rh_ticks = RH%     * 65535 / 100
 *   t_ticks  = (T + 45) * 65535 / 175                                     */
uint16_t sgp41_encode_rh(float rh_percent);
uint16_t sgp41_encode_temp(float temp_c);

#endif /* AERIS_SGP41_H */
