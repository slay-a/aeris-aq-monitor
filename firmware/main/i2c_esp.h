/* i2c_esp.h - ESP-IDF backing for the portable aeris_i2c_t seam.
 *
 * One master bus, one device handle per address, created up front. The handles
 * are cached because i2c_master_bus_add_device() allocates, and doing that on
 * every transaction at 0.2 Hz for four sensors would fragment the heap for no
 * reason.
 */
#ifndef AERIS_I2C_ESP_H
#define AERIS_I2C_ESP_H

#include "aeris/i2c_hal.h"
#include "driver/gpio.h"
#include "esp_err.h"

typedef struct aeris_i2c_esp aeris_i2c_esp_t;

/* scl_hz: the Sensirion parts are specified to 100 kHz standard mode; the
 * PMSA003I is also 100 kHz. Going faster buys nothing at this sample rate and
 * costs margin on a hand-wired bus. */
esp_err_t aeris_i2c_esp_init(gpio_num_t sda, gpio_num_t scl, uint32_t scl_hz,
                             aeris_i2c_esp_t **out);
const aeris_i2c_t *aeris_i2c_esp_hal(aeris_i2c_esp_t *h);

/* Log every address that answers. Run this first on a new board: it turns
 * "nothing works" into "the SGP41 is not on the bus". */
void aeris_i2c_esp_scan(aeris_i2c_esp_t *h);

#endif /* AERIS_I2C_ESP_H */
