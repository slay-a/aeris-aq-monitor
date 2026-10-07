/* scd40.h - register-level driver for the Sensirion SCD40 photoacoustic
 * CO2 sensor. No vendor library: commands, inter-frame delays, CRC-8 and
 * fixed-point conversion are all implemented here from the datasheet.
 *
 * Wire format
 * -----------
 * A command is a 16-bit big-endian opcode. Commands that carry arguments
 * append one or more [msb, lsb, crc8] triplets. Commands that return data
 * require a datasheet-specified delay between the write and the read -- the
 * SCD40 does not clock-stretch, so reading early returns a NACK, not a wait.
 * Every returned 16-bit word carries its own CRC-8.
 *
 * Timing
 * ------
 * Periodic measurement produces a sample every 5 s. get_data_ready_status is
 * polled instead of blind-sleeping so the sampler never reads a stale frame.
 */
#ifndef AERIS_SCD40_H
#define AERIS_SCD40_H

#include <stdbool.h>
#include <stdint.h>
#include "aeris/aeris_err.h"
#include "aeris/i2c_hal.h"

#define SCD40_I2C_ADDR 0x62

/* Command opcodes, SCD4x datasheet rev 1.6 section 3. */
typedef enum {
    SCD40_CMD_START_PERIODIC_MEASUREMENT  = 0x21B1,
    SCD40_CMD_READ_MEASUREMENT            = 0xEC05,
    SCD40_CMD_STOP_PERIODIC_MEASUREMENT   = 0x3F86,
    SCD40_CMD_SET_TEMPERATURE_OFFSET      = 0x241D,
    SCD40_CMD_GET_TEMPERATURE_OFFSET      = 0x2318,
    SCD40_CMD_SET_SENSOR_ALTITUDE         = 0x2427,
    SCD40_CMD_GET_SENSOR_ALTITUDE         = 0x2322,
    SCD40_CMD_SET_AMBIENT_PRESSURE        = 0xE000,
    SCD40_CMD_PERFORM_FORCED_RECALIBRATION= 0x362F,
    SCD40_CMD_SET_ASC_ENABLED             = 0x2416,
    SCD40_CMD_GET_ASC_ENABLED             = 0x2313,
    SCD40_CMD_START_LOW_POWER_PERIODIC    = 0x21AC,
    SCD40_CMD_GET_DATA_READY_STATUS       = 0xE4B8,
    SCD40_CMD_PERSIST_SETTINGS            = 0x3615,
    SCD40_CMD_GET_SERIAL_NUMBER           = 0x3682,
    SCD40_CMD_PERFORM_SELF_TEST           = 0x3639,
    SCD40_CMD_PERFORM_FACTORY_RESET       = 0x3632,
    SCD40_CMD_REINIT                      = 0x3646,
    SCD40_CMD_MEASURE_SINGLE_SHOT         = 0x219D,
    SCD40_CMD_MEASURE_SINGLE_SHOT_RHT     = 0x2196,
    SCD40_CMD_POWER_DOWN                  = 0x36E0,
    SCD40_CMD_WAKE_UP                     = 0x36F6,
} scd40_cmd_t;

/* Datasheet execution times (ms) for the commands we issue. */
#define SCD40_T_EXEC_SHORT_MS        1    /* most setup commands          */
#define SCD40_T_EXEC_STOP_MS         500  /* stop_periodic_measurement    */
#define SCD40_T_EXEC_SELF_TEST_MS    10000
#define SCD40_T_EXEC_FRC_MS          400
#define SCD40_T_EXEC_SINGLE_SHOT_MS  5000
#define SCD40_T_EXEC_REINIT_MS       30
#define SCD40_MEASURE_INTERVAL_MS    5000

typedef struct {
    const aeris_i2c_t *bus;
    uint8_t  addr;
    uint64_t serial;        /* 48-bit, filled by scd40_probe()           */
    bool     measuring;
    uint32_t crc_errors;    /* cumulative, surfaced on the dashboard     */
    uint32_t io_errors;
} scd40_t;

typedef struct {
    uint16_t co2_ppm;       /* 0..40000, raw sensor word is already ppm   */
    float    temperature_c;
    float    humidity_rh;
} scd40_sample_t;

void        scd40_init(scd40_t *d, const aeris_i2c_t *bus, uint8_t addr);

/* Read the 48-bit serial. Doubles as a presence check: a part that answers
 * with a valid CRC on three words is the part we think it is. */
aeris_err_t scd40_probe(scd40_t *d);

aeris_err_t scd40_start_periodic(scd40_t *d);
aeris_err_t scd40_stop_periodic(scd40_t *d);
aeris_err_t scd40_data_ready(scd40_t *d, bool *ready);
aeris_err_t scd40_read_measurement(scd40_t *d, scd40_sample_t *out);

/* Compensation / calibration. */
aeris_err_t scd40_set_temperature_offset(scd40_t *d, float offset_c);
aeris_err_t scd40_get_temperature_offset(scd40_t *d, float *offset_c);
aeris_err_t scd40_set_sensor_altitude(scd40_t *d, uint16_t metres);
aeris_err_t scd40_set_ambient_pressure(scd40_t *d, uint32_t pressure_pa);
aeris_err_t scd40_set_asc_enabled(scd40_t *d, bool enabled);
aeris_err_t scd40_get_asc_enabled(scd40_t *d, bool *enabled);
aeris_err_t scd40_forced_recalibration(scd40_t *d, uint16_t target_ppm,
                                       int32_t *correction_ppm);
aeris_err_t scd40_persist_settings(scd40_t *d);
aeris_err_t scd40_self_test(scd40_t *d);
aeris_err_t scd40_reinit(scd40_t *d);

/* Unit-testable conversions, exposed so the test suite can pin them to the
 * worked examples in the datasheet. */
uint16_t scd40_encode_temp_offset(float offset_c);
float    scd40_decode_temperature(uint16_t raw);
float    scd40_decode_humidity(uint16_t raw);

#endif /* AERIS_SCD40_H */
