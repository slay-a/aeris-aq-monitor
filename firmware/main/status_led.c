#include <math.h>
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "status_led.h"

static const char *TAG = "led";

#define LEDC_MODE      LEDC_LOW_SPEED_MODE
#define LEDC_TIMER     LEDC_TIMER_0
#define LEDC_RES       LEDC_TIMER_10_BIT
#define LEDC_MAX       1023
#define LED_TICK_MS    25            /* smooth enough for a visible fade */

static gpio_num_t s_pin[3];
static bool       s_have[3];
static aeris_app_t *s_app;

static void set_rgb(uint16_t r, uint16_t g, uint16_t b)
{
    const uint16_t v[3] = { r, g, b };
    for (int i = 0; i < 3; i++) {
        if (!s_have[i]) continue;
        ledc_set_duty(LEDC_MODE, (ledc_channel_t)i, v[i]);
        ledc_update_duty(LEDC_MODE, (ledc_channel_t)i);
    }
}

esp_err_t status_led_init(gpio_num_t r, gpio_num_t g, gpio_num_t b)
{
    s_pin[0] = r; s_pin[1] = g; s_pin[2] = b;

    ledc_timer_config_t t = {
        .speed_mode      = LEDC_MODE,
        .timer_num       = LEDC_TIMER,
        .duty_resolution = LEDC_RES,
        .freq_hz         = 4000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&t);
    if (err != ESP_OK) return err;

    int n = 0;
    for (int i = 0; i < 3; i++) {
        s_have[i] = (s_pin[i] != GPIO_NUM_NC);
        if (!s_have[i]) continue;
        ledc_channel_config_t c = {
            .gpio_num   = s_pin[i],
            .speed_mode = LEDC_MODE,
            .channel    = (ledc_channel_t)i,
            .timer_sel  = LEDC_TIMER,
            .duty       = 0,
            .hpoint     = 0,
        };
        err = ledc_channel_config(&c);
        if (err != ESP_OK) return err;
        n++;
    }
    ESP_LOGI(TAG, "%d channel(s) configured", n);
    return (n > 0) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

led_pattern_t status_led_pattern_for(const aeris_snapshot_t *s)
{
    switch (s->phase) {
    case AERIS_PHASE_BOOT:
    case AERIS_PHASE_PROBE:
        return LED_PATTERN_BOOT;
    case AERIS_PHASE_FAULT:
        return LED_PATTERN_FAULT;
    case AERIS_PHASE_CONDITION:
        return LED_PATTERN_CONDITION;
    case AERIS_PHASE_WARMUP:
    case AERIS_PHASE_RUN:
        break;
    }
    switch (s->aqi.band) {
    case AERIS_BAND_GOOD: return LED_PATTERN_GOOD;
    case AERIS_BAND_FAIR: return LED_PATTERN_FAIR;
    case AERIS_BAND_POOR: return LED_PATTERN_POOR;
    case AERIS_BAND_BAD:  return LED_PATTERN_BAD;
    }
    return LED_PATTERN_GOOD;
}

/* Brightness envelope for a pattern at phase p in [0,1). Kept as a pure
 * function of time so the task body stays a single non-blocking tick. */
static float envelope(led_pattern_t pat, float p)
{
    switch (pat) {
    case LED_PATTERN_BOOT:      return (p < 0.5f) ? 1.0f : 0.0f;          /* 2 Hz */
    case LED_PATTERN_CONDITION: return 0.15f + 0.85f * (0.5f - 0.5f * cosf(2.0f * 3.14159265f * p));
    case LED_PATTERN_GOOD:      return 0.18f;                              /* steady */
    case LED_PATTERN_FAIR:      return 0.35f;
    case LED_PATTERN_POOR:      return (p < 0.5f) ? 0.55f : 0.08f;
    case LED_PATTERN_BAD:       return (p < 0.25f || (p > 0.5f && p < 0.75f)) ? 1.0f : 0.05f;
    case LED_PATTERN_FAULT:     return (p < 0.12f || (p > 0.24f && p < 0.36f)) ? 1.0f : 0.0f;
    }
    return 0.2f;
}

static void colour_for(led_pattern_t pat, float *r, float *g, float *b)
{
    switch (pat) {
    case LED_PATTERN_BOOT:      *r = 0.0f; *g = 0.3f; *b = 1.0f; break;
    case LED_PATTERN_CONDITION: *r = 0.4f; *g = 0.0f; *b = 1.0f; break;
    case LED_PATTERN_GOOD:      *r = 0.0f; *g = 1.0f; *b = 0.1f; break;
    case LED_PATTERN_FAIR:      *r = 1.0f; *g = 0.6f; *b = 0.0f; break;
    case LED_PATTERN_POOR:      *r = 1.0f; *g = 0.3f; *b = 0.0f; break;
    case LED_PATTERN_BAD:
    case LED_PATTERN_FAULT:     *r = 1.0f; *g = 0.0f; *b = 0.0f; break;
    default:                    *r = 0.0f; *g = 0.0f; *b = 1.0f; break;
    }
}

/* Cycle period per pattern, in milliseconds. */
static uint32_t period_ms(led_pattern_t pat)
{
    switch (pat) {
    case LED_PATTERN_BOOT:      return 500;
    case LED_PATTERN_CONDITION: return 2000;
    case LED_PATTERN_POOR:      return 1600;
    case LED_PATTERN_BAD:       return 700;
    case LED_PATTERN_FAULT:     return 1500;
    default:                    return 1000;
    }
}

static void led_task(void *arg)
{
    (void)arg;
    uint32_t t = 0;
    /* The snapshot is only re-read once a second: it changes at 0.2 Hz, and
     * polling it every 25 ms would take the lock 40 times more often than
     * there is any reason to. */
    aeris_snapshot_t snap;
    aeris_app_snapshot(s_app, &snap);
    led_pattern_t pat = status_led_pattern_for(&snap);
    uint32_t last_poll = 0;

    for (;;) {
        if (t - last_poll >= 1000) {
            aeris_app_snapshot(s_app, &snap);
            pat = status_led_pattern_for(&snap);
            last_poll = t;
        }

        uint32_t per = period_ms(pat);
        float p = (float)(t % per) / (float)per;
        float e = envelope(pat, p);
        float r, g, b;
        colour_for(pat, &r, &g, &b);

        /* Gamma 2.2 so a 20 % duty actually looks like 20 % brightness. */
        float gamma = 2.2f;
        set_rgb((uint16_t)(powf(r * e, gamma) * LEDC_MAX),
                (uint16_t)(powf(g * e, gamma) * LEDC_MAX),
                (uint16_t)(powf(b * e, gamma) * LEDC_MAX));

        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
        t += LED_TICK_MS;
    }
}

esp_err_t status_led_start(aeris_app_t *app)
{
    s_app = app;
    BaseType_t ok = xTaskCreate(led_task, "led", 3072, NULL, 3, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}
