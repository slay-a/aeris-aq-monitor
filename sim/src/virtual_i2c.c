#include <math.h>
#include <string.h>
#include <time.h>
#include "aeris/crc8.h"
#include "aeris/pmsa003i.h"
#include "aeris/scd40.h"
#include "aeris/sgp41.h"
#include "aeris/sht31.h"
#include "virtual_i2c.h"

/* Saturating float -> uint16_t. Converting an out-of-range float to an integer
 * type is undefined behaviour in C, not a wrap: during a frying event the
 * PMSA003I's particle-count fields genuinely exceed 65535, and letting that
 * reach a cast produced frames whose checksum did not match the bytes the
 * compiler had actually stored. The real sensor saturates its fields, so the
 * model does too. */
static uint16_t sat_u16(float v)
{
    if (!(v == v) || v <= 0.0f) return 0;        /* NaN or negative */
    if (v >= 65535.0f) return 65535u;
    return (uint16_t)v;
}

/* ---- sensor transfer functions ---------------------------------------- */

/* SGP41 raw VOC tick from the true TVOC concentration. The datasheet does not
 * publish a transfer function, so this is the qualitative behaviour that
 * matters for the pipeline: a log-concentration response, a falling signal as
 * gas rises, and a humidity cross-sensitivity that the compensation input is
 * supposed to cancel. The simulator applies that cross-sensitivity to the
 * *difference* between the compensation value the driver supplied and the
 * true RH, so a driver that sends the datasheet default instead of the live
 * reading gets a visibly worse signal -- which is how the compensation bullet
 * is actually tested rather than asserted. */
static uint16_t sgp41_voc_ticks(room_t *r, uint16_t rh_ticks_sent)
{
    float base = 30800.0f;
    float resp = 2600.0f * log10f(1.0f + r->voc_ppb / 25.0f);

    float rh_sent = (float)rh_ticks_sent * 100.0f / 65535.0f;
    float rh_err  = r->rh - rh_sent;          /* uncompensated residual */
    float rh_term = 38.0f * rh_err;

    return sat_u16(base - resp + rh_term + 9.0f * room_randn(r));
}

static uint16_t sgp41_nox_ticks(room_t *r)
{
    float base = 18200.0f;
    float resp = 1500.0f * log10f(1.0f + r->nox_ppb / 2.0f);
    return sat_u16(base - resp + 7.0f * room_randn(r));
}

static uint16_t enc_t(float c)  { return sat_u16((c + 45.0f) * 65535.0f / 175.0f + 0.5f); }
static uint16_t enc_rh(float h) { return sat_u16(h * 65535.0f / 100.0f + 0.5f); }

/* ---- reply construction ----------------------------------------------- */

static void reply_words(vdev_t *d, const uint16_t *w, size_t n)
{
    d->reply_len = 0;
    for (size_t i = 0; i < n; i++) {
        d->reply_len += aeris_put_word_crc(&d->reply[d->reply_len], w[i]);
    }
}

static void arm(vdev_t *d, vbus_t *b, uint16_t cmd, uint32_t exec_ms)
{
    d->pending_cmd     = cmd;
    d->cmd_ready_at_ms = b->now_ms + exec_ms;
}

/* ---- SCD40 ------------------------------------------------------------ */

static aeris_err_t scd40_write(vbus_t *b, const uint8_t *buf, size_t len)
{
    vdev_t *d = &b->scd40;
    if (len < 2) return AERIS_ERR_IO;
    uint16_t cmd = (uint16_t)(buf[0] << 8 | buf[1]);

    /* Commands carrying an argument must present a correct CRC-8, or the real
     * part silently ignores the write. Modelling that catches a mis-framed
     * argument rather than letting it pass. */
    if (len == 5) {
        if (aeris_crc8(&buf[2], 2) != buf[4]) return AERIS_ERR_IO;
    }
    uint16_t arg = (len >= 5) ? (uint16_t)(buf[2] << 8 | buf[3]) : 0;

    switch (cmd) {
    case SCD40_CMD_START_PERIODIC_MEASUREMENT:
        d->measuring = true;
        d->last_measure_ms = b->now_ms;
        arm(d, b, cmd, 1);
        return AERIS_OK;
    case SCD40_CMD_STOP_PERIODIC_MEASUREMENT:
        d->measuring = false;
        arm(d, b, cmd, SCD40_T_EXEC_STOP_MS);
        return AERIS_OK;
    case SCD40_CMD_GET_SERIAL_NUMBER: {
        if (d->measuring) return AERIS_ERR_IO;   /* needs idle */
        uint16_t w[3] = { (uint16_t)(d->serial >> 32),
                          (uint16_t)(d->serial >> 16),
                          (uint16_t)(d->serial) };
        reply_words(d, w, 3);
        arm(d, b, cmd, 1);
        return AERIS_OK;
    }
    case SCD40_CMD_GET_DATA_READY_STATUS: {
        /* The real part sets the flag every 5 s while measuring. */
        bool ready = d->measuring &&
                     (b->now_ms - d->last_measure_ms) >= SCD40_MEASURE_INTERVAL_MS;
        uint16_t w = ready ? (uint16_t)(0x8000u | 0x0123u) : 0x8000u;
        reply_words(d, &w, 1);
        arm(d, b, cmd, 1);
        return AERIS_OK;
    }
    case SCD40_CMD_READ_MEASUREMENT: {
        if (!d->measuring) return AERIS_ERR_IO;
        room_t *r = b->room;
        float co2 = r->co2_ppm + 12.0f * room_randn(r);
        if (co2 < 0.0f) co2 = 0.0f;
        if (co2 > 40000.0f) co2 = 40000.0f;   /* the part's stated range */
        /* The SCD40's own T/RH read high because the optical cavity is warm;
         * the configured offset is what the part subtracts internally. */
        float t_die = r->temp_c + 4.3f - d->temp_offset_c + 0.08f * room_randn(r);
        uint16_t w[3] = { sat_u16(co2), enc_t(t_die), enc_rh(r->rh) };
        reply_words(d, w, 3);
        d->last_measure_ms = b->now_ms;   /* consuming clears the flag */
        d->measure_count++;
        arm(d, b, cmd, 1);
        return AERIS_OK;
    }
    case SCD40_CMD_SET_TEMPERATURE_OFFSET:
        if (d->measuring) return AERIS_ERR_IO;
        d->temp_offset_c = 175.0f * ((float)arg / 65536.0f);
        arm(d, b, cmd, 1);
        return AERIS_OK;
    case SCD40_CMD_GET_TEMPERATURE_OFFSET: {
        uint16_t w = (uint16_t)(d->temp_offset_c * 65536.0f / 175.0f + 0.5f);
        reply_words(d, &w, 1);
        arm(d, b, cmd, 1);
        return AERIS_OK;
    }
    case SCD40_CMD_SET_SENSOR_ALTITUDE:
        if (d->measuring) return AERIS_ERR_IO;
        d->altitude_m = arg;
        arm(d, b, cmd, 1);
        return AERIS_OK;
    case SCD40_CMD_SET_AMBIENT_PRESSURE:
        arm(d, b, cmd, 1);
        return AERIS_OK;
    case SCD40_CMD_SET_ASC_ENABLED:
        if (d->measuring) return AERIS_ERR_IO;
        d->asc = (arg != 0);
        arm(d, b, cmd, 1);
        return AERIS_OK;
    case SCD40_CMD_GET_ASC_ENABLED: {
        uint16_t w = d->asc ? 1u : 0u;
        reply_words(d, &w, 1);
        arm(d, b, cmd, 1);
        return AERIS_OK;
    }
    case SCD40_CMD_PERFORM_SELF_TEST: {
        uint16_t w = 0;
        reply_words(d, &w, 1);
        arm(d, b, cmd, SCD40_T_EXEC_SELF_TEST_MS);
        return AERIS_OK;
    }
    case SCD40_CMD_PERFORM_FORCED_RECALIBRATION: {
        if (d->measuring) return AERIS_ERR_IO;
        /* Report the correction the part would have applied. */
        int32_t corr = (int32_t)(arg - (uint16_t)b->room->co2_ppm);
        uint16_t w = (uint16_t)(0x8000 + corr);
        reply_words(d, &w, 1);
        arm(d, b, cmd, SCD40_T_EXEC_FRC_MS);
        return AERIS_OK;
    }
    case SCD40_CMD_REINIT:
        arm(d, b, cmd, SCD40_T_EXEC_REINIT_MS);
        return AERIS_OK;
    case SCD40_CMD_PERSIST_SETTINGS:
        arm(d, b, cmd, 800);
        return AERIS_OK;
    default:
        return AERIS_ERR_IO;      /* unknown opcode: the part NACKs */
    }
}

/* ---- SGP41 ------------------------------------------------------------ */

static aeris_err_t sgp41_write(vbus_t *b, const uint8_t *buf, size_t len)
{
    vdev_t *d = &b->sgp41;
    if (len < 2) return AERIS_ERR_IO;
    uint16_t cmd = (uint16_t)(buf[0] << 8 | buf[1]);

    switch (cmd) {
    case SGP41_CMD_MEASURE_RAW_SIGNALS:
    case SGP41_CMD_EXECUTE_CONDITIONING: {
        if (len != 8) return AERIS_ERR_IO;          /* must carry both words */
        if (aeris_crc8(&buf[2], 2) != buf[4]) return AERIS_ERR_IO;
        if (aeris_crc8(&buf[5], 2) != buf[7]) return AERIS_ERR_IO;
        d->last_rh_ticks = (uint16_t)(buf[2] << 8 | buf[3]);
        d->last_t_ticks  = (uint16_t)(buf[5] << 8 | buf[6]);
        d->measure_count++;

        uint16_t voc = sgp41_voc_ticks(b->room, d->last_rh_ticks);
        if (cmd == SGP41_CMD_EXECUTE_CONDITIONING) {
            reply_words(d, &voc, 1);
            arm(d, b, cmd, SGP41_T_CONDITION_MS);
        } else {
            uint16_t w[2] = { voc, sgp41_nox_ticks(b->room) };
            reply_words(d, w, 2);
            arm(d, b, cmd, SGP41_T_MEASURE_MS);
        }
        return AERIS_OK;
    }
    case SGP41_CMD_GET_SERIAL_NUMBER: {
        uint16_t w[3] = { (uint16_t)(d->serial >> 32),
                          (uint16_t)(d->serial >> 16),
                          (uint16_t)(d->serial) };
        reply_words(d, w, 3);
        arm(d, b, cmd, 1);
        return AERIS_OK;
    }
    case SGP41_CMD_EXECUTE_SELF_TEST: {
        uint16_t w = 0xD400u;      /* high byte fixed, low bits clear = pass */
        reply_words(d, &w, 1);
        arm(d, b, cmd, SGP41_T_SELF_TEST_MS);
        return AERIS_OK;
    }
    case SGP41_CMD_TURN_HEATER_OFF:
        arm(d, b, cmd, 1);
        return AERIS_OK;
    default:
        return AERIS_ERR_IO;
    }
}

/* ---- SHT31 ------------------------------------------------------------ */

static aeris_err_t sht31_write(vbus_t *b, const uint8_t *buf, size_t len)
{
    vdev_t *d = &b->sht31;
    if (len < 2) return AERIS_ERR_IO;
    uint16_t cmd = (uint16_t)(buf[0] << 8 | buf[1]);
    room_t *r = b->room;

    switch (cmd) {
    case SHT31_CMD_MEAS_HIGHREP_NOSTRETCH: {
        float t = r->temp_c + 0.9f - 0.9f + 0.05f * room_randn(r);
        float h = r->rh + 0.4f * room_randn(r);
        if (h < 0.0f) h = 0.0f;
        if (h > 100.0f) h = 100.0f;
        uint16_t w[2] = { enc_t(t), enc_rh(h) };
        reply_words(d, w, 2);
        arm(d, b, cmd, SHT31_T_MEAS_HIGHREP_MS);
        return AERIS_OK;
    }
    case SHT31_CMD_SOFT_RESET:
        arm(d, b, cmd, SHT31_T_RESET_MS);
        d->reply_len = 0;
        return AERIS_OK;
    case SHT31_CMD_READ_STATUS: {
        uint16_t w = 0x8010u;
        reply_words(d, &w, 1);
        arm(d, b, cmd, 1);
        return AERIS_OK;
    }
    case SHT31_CMD_HEATER_ON:
    case SHT31_CMD_HEATER_OFF:
    case SHT31_CMD_CLEAR_STATUS:
        arm(d, b, cmd, 1);
        return AERIS_OK;
    default:
        return AERIS_ERR_IO;
    }
}

/* ---- PMSA003I --------------------------------------------------------- */

static void pms_build_frame(vbus_t *b)
{
    vdev_t *d = &b->pms;
    room_t *r = b->room;
    uint8_t f[PMSA003I_FRAME_LEN];
    memset(f, 0, sizeof f);
    f[0] = PMSA003I_MAGIC_0;
    f[1] = PMSA003I_MAGIC_1;
    f[2] = 0x00; f[3] = PMSA003I_FRAME_BODY;

    float pm25 = r->pm2_5_ugm3 + 0.6f * room_randn(r);
    if (pm25 < 0.0f) pm25 = 0.0f;
    /* The PMSA003I's specified range tops out at 1000 ug/m3 and its registers
     * saturate there rather than wrapping. */
    if (pm25 > 1000.0f) pm25 = 1000.0f;
    float pm10 = pm25 * 1.45f;
    float pm1  = pm25 * 0.72f;

    uint16_t vals[12] = {
        sat_u16(pm1), sat_u16(pm25), sat_u16(pm10),
        sat_u16(pm1), sat_u16(pm25), sat_u16(pm10),
        sat_u16(pm25 * 180.0f), sat_u16(pm25 * 52.0f),
        sat_u16(pm25 * 9.0f),   sat_u16(pm25 * 1.1f),
        sat_u16(pm25 * 0.3f),   sat_u16(pm25 * 0.1f),
    };
    for (int i = 0; i < 12; i++) {
        f[4 + i * 2]     = (uint8_t)(vals[i] >> 8);
        f[4 + i * 2 + 1] = (uint8_t)(vals[i] & 0xFF);
    }
    uint16_t sum = 0;
    for (int i = 0; i < PMSA003I_FRAME_LEN - 2; i++) sum = (uint16_t)(sum + f[i]);
    f[30] = (uint8_t)(sum >> 8);
    f[31] = (uint8_t)(sum & 0xFF);

    memcpy(d->reply, f, sizeof f);
    d->reply_len = sizeof f;
    d->cmd_ready_at_ms = b->now_ms;   /* always readable, no command needed */
}

/* ---- bus plumbing ----------------------------------------------------- */

static vdev_t *dev_for(vbus_t *b, uint8_t addr)
{
    switch (addr) {
    case SCD40_I2C_ADDR:    return &b->scd40;
    case SGP41_I2C_ADDR:    return &b->sgp41;
    case SHT31_I2C_ADDR_A:  return &b->sht31;
    case PMSA003I_I2C_ADDR: return &b->pms;
    default: return NULL;
    }
}

static bool should_nack(vbus_t *b)
{
    b->txn_count++;
    if (b->nack_every && (b->txn_count % b->nack_every) == 0) {
        b->injected_nacks++;
        return true;
    }
    return false;
}

static aeris_err_t v_write(void *ctx, uint8_t addr, const uint8_t *buf, size_t len)
{
    vbus_t *b = (vbus_t *)ctx;
    vdev_t *d = dev_for(b, addr);
    if (!d || !d->present) return AERIS_ERR_IO;
    if (should_nack(b)) return AERIS_ERR_IO;

    if (addr == SCD40_I2C_ADDR)    return scd40_write(b, buf, len);
    if (addr == SGP41_I2C_ADDR)    return sgp41_write(b, buf, len);
    if (addr == SHT31_I2C_ADDR_A)  return sht31_write(b, buf, len);
    return AERIS_ERR_IO;           /* the PMSA003I takes no commands */
}

static aeris_err_t v_read(void *ctx, uint8_t addr, uint8_t *buf, size_t len)
{
    vbus_t *b = (vbus_t *)ctx;
    vdev_t *d = dev_for(b, addr);
    if (!d || !d->present) return AERIS_ERR_IO;
    if (should_nack(b)) return AERIS_ERR_IO;

    if (addr == PMSA003I_I2C_ADDR) pms_build_frame(b);

    /* The real parts NACK a read issued before the command has executed.
     * Honouring that is what makes a missing delay_ms() a test failure. */
    if (b->now_ms < d->cmd_ready_at_ms) return AERIS_ERR_IO;
    if (d->reply_len == 0) return AERIS_ERR_IO;
    if (len > d->reply_len) return AERIS_ERR_IO;

    memcpy(buf, d->reply, len);

    if (b->corrupt_every && (b->txn_count % b->corrupt_every) == 0) {
        /* Flip one bit in the payload. The driver must catch this via CRC-8,
         * not pass it upstream as a plausible reading. */
        buf[0] ^= 0x10u;
        b->injected_corruptions++;
    }
    return AERIS_OK;
}

/* Sleep for the real time that dt_ms of virtual time should take, accumulating
 * the sub-millisecond remainder so a long run does not drift. */
void vbus_pace(vbus_t *b, uint32_t dt_ms)
{
    if (!b->time_speedup) return;
    b->sleep_debt_ns += (uint64_t)dt_ms * 1000000ull / b->time_speedup;
    if (b->sleep_debt_ns < 1000000ull) return;      /* under 1 ms: keep waiting */
    struct timespec ts = {
        .tv_sec  = (time_t)(b->sleep_debt_ns / 1000000000ull),
        .tv_nsec = (long)(b->sleep_debt_ns % 1000000000ull),
    };
    b->sleep_debt_ns = 0;
    nanosleep(&ts, NULL);
}

static void v_delay(void *ctx, uint32_t ms)
{
    vbus_t *b = (vbus_t *)ctx;
    b->now_ms += ms;
    room_step(b->room, ms);
    vbus_pace(b, ms);
}

static uint32_t v_now(void *ctx) { return ((vbus_t *)ctx)->now_ms; }

void vbus_init(vbus_t *b, room_t *room)
{
    memset(b, 0, sizeof *b);
    b->room   = room;
    b->now_ms = 0;
    b->rng    = 0xC0FFEEu;

    b->scd40.present = true;  b->scd40.serial = 0x1A2B3C4D5E6Full;
    b->scd40.asc     = true;
    b->sgp41.present = true;  b->sgp41.serial = 0x0011223344ull;
    b->sht31.present = true;
    b->pms.present   = true;

    b->hal.write    = v_write;
    b->hal.read     = v_read;
    b->hal.delay_ms = v_delay;
    b->hal.now_ms   = v_now;
    b->hal.ctx      = b;
}

void vbus_set_present(vbus_t *b, uint8_t addr, bool present)
{
    vdev_t *d = dev_for(b, addr);
    if (d) d->present = present;
}

const aeris_i2c_t *vbus_hal(vbus_t *b) { return &b->hal; }
