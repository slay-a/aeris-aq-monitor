/* status_led.h - the LED runs in its own task.
 *
 * This is the part that was wrong in the first cut of the firmware: the LED was
 * timed with blocking delays inside the sampling loop, which froze the web
 * server for ten seconds out of every cycle. Separating it into a task is not a
 * style preference -- it is the difference between a dashboard that responds and
 * one that times out.
 */
#ifndef AERIS_STATUS_LED_H
#define AERIS_STATUS_LED_H

#include "aeris/app.h"
#include "driver/gpio.h"
#include "esp_err.h"

typedef enum {
    LED_PATTERN_BOOT = 0,     /* fast blink: coming up                    */
    LED_PATTERN_CONDITION,    /* slow pulse: SGP41 burning in             */
    LED_PATTERN_GOOD,         /* steady dim green                         */
    LED_PATTERN_FAIR,         /* steady amber                             */
    LED_PATTERN_POOR,         /* slow blink amber                         */
    LED_PATTERN_BAD,          /* fast blink red                           */
    LED_PATTERN_FAULT,        /* double-blink red: no sensors              */
} led_pattern_t;

/* Pass GPIO_NUM_NC for any channel the board does not have. A board with only
 * one LED still gets the blink patterns, just without the colour. */
esp_err_t status_led_init(gpio_num_t r, gpio_num_t g, gpio_num_t b);

/* Starts the LED task. It polls the snapshot rather than being pushed to, so
 * the sampling task never blocks on the LED. */
esp_err_t status_led_start(aeris_app_t *app);

led_pattern_t status_led_pattern_for(const aeris_snapshot_t *s);

#endif /* AERIS_STATUS_LED_H */
