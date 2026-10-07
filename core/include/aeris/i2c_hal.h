/* i2c_hal.h - the single seam between portable driver code and the platform.
 *
 * Every driver in core/ talks to sensors only through this struct. On the
 * ESP32-C3 it is backed by the IDF i2c_master driver; in the host simulator
 * it is backed by a virtual bus that models each chip's command set. The
 * driver sources are compiled unmodified for both, which is what makes the
 * simulator worth trusting.
 */
#ifndef AERIS_I2C_HAL_H
#define AERIS_I2C_HAL_H

#include <stddef.h>
#include <stdint.h>
#include "aeris/aeris_err.h"

typedef struct aeris_i2c aeris_i2c_t;

struct aeris_i2c {
    /* 7-bit addressed write of len bytes. */
    aeris_err_t (*write)(void *ctx, uint8_t addr, const uint8_t *buf, size_t len);
    /* 7-bit addressed read of len bytes. */
    aeris_err_t (*read)(void *ctx, uint8_t addr, uint8_t *buf, size_t len);
    /* Blocking delay. Sensirion parts need a command-specific gap between
     * the command write and the read; they do not support clock stretching
     * in the modes we use, so this gap is mandatory, not an optimisation. */
    void (*delay_ms)(void *ctx, uint32_t ms);
    /* Monotonic milliseconds. */
    uint32_t (*now_ms)(void *ctx);
    void *ctx;
};

static inline aeris_err_t aeris_i2c_write(const aeris_i2c_t *b, uint8_t a,
                                          const uint8_t *buf, size_t len) {
    return b->write(b->ctx, a, buf, len);
}
static inline aeris_err_t aeris_i2c_read(const aeris_i2c_t *b, uint8_t a,
                                         uint8_t *buf, size_t len) {
    return b->read(b->ctx, a, buf, len);
}
static inline void aeris_delay_ms(const aeris_i2c_t *b, uint32_t ms) {
    b->delay_ms(b->ctx, ms);
}
static inline uint32_t aeris_now_ms(const aeris_i2c_t *b) {
    return b->now_ms(b->ctx);
}

#endif /* AERIS_I2C_HAL_H */
