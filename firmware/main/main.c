/* main.c - Aeris AQ Monitor, ESP32-C3.
 *
 * Task structure
 * --------------
 *   sensor  prio 5, 4 kB   owns the I2C bus and every sensor; the only task
 *                          that calls aeris_app_step()
 *   httpd   prio 4, 6 kB   created by esp_http_server; reads snapshots
 *   led     prio 3, 3 kB   polls snapshots, drives LEDC
 *
 * Only the sensor task touches the drivers, so there is no bus arbitration to
 * get wrong. Everything else reads a published snapshot copied under one mutex.
 * The sampling task's blocking waits -- 15 ms for the SHT31, 50 ms for the
 * SGP41, 500 ms for an SCD40 stop -- therefore delay only itself; the dashboard
 * keeps answering through all of them.
 *
 * Why not one loop with delays: an earlier cut of this firmware did exactly
 * that, and the two five-second waits in it made the web server unreachable for
 * ten seconds out of every cycle.
 */
#include <stdio.h>
#include "aeris/app.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2c_esp.h"
#include "net.h"
#include "sdkconfig.h"
#include "status_led.h"
#include "web_server.h"

static const char *TAG = "aeris";

#define FW_VERSION "aeris 1.0.0"
#define HOSTNAME   "aeris"

static aeris_app_t      s_app;
static SemaphoreHandle_t s_app_lock;

/* The lock the core uses to publish snapshots. Deliberately a plain mutex and
 * not a critical section: the HTTP task can hold it for the length of a
 * memcpy of the snapshot, which is far too long to spend with interrupts off. */
static void app_lock(void *ctx, bool acquire)
{
    (void)ctx;
    if (acquire) xSemaphoreTake(s_app_lock, portMAX_DELAY);
    else         xSemaphoreGive(s_app_lock);
}

static uint32_t app_us(void *ctx)
{
    (void)ctx;
    return (uint32_t)esp_timer_get_time();
}

static void sensor_task(void *arg)
{
    (void)arg;
    aeris_phase_t last = (aeris_phase_t)-1;

    for (;;) {
        /* aeris_app_step() returns how long to wait before the next call, so
         * the cadence is the core's decision, not this task's: 10 ms during
         * bring-up, 1 s while the SGP41 conditions, 5 s once running. */
        uint32_t wait_ms = aeris_app_step(&s_app);

        if (s_app.phase != last) {
            last = s_app.phase;
            ESP_LOGI(TAG, "phase -> %s", aeris_phase_name(s_app.phase));
            if (s_app.phase == AERIS_PHASE_WARMUP) {
                ESP_LOGI(TAG, "sensors: scd40=%d sgp41=%d sht31=%d pmsa003i=%d",
                         s_app.snap.scd40_present, s_app.snap.sgp41_present,
                         s_app.snap.sht31_present, s_app.snap.pmsa003i_present);
                if (s_app.snap.scd40_present) {
                    ESP_LOGI(TAG, "scd40 serial %012llx",
                             (unsigned long long)s_app.scd.serial);
                }
            }
        }

        /* One line per sample at info level is 17 kB of log an hour over USB,
         * which is tolerable on a bench and the first thing you want when a
         * reading looks wrong. */
        if (s_app.phase == AERIS_PHASE_RUN) {
            aeris_snapshot_t s;
            aeris_app_snapshot(&s_app, &s);
            ESP_LOGI(TAG,
                     "co2=%4.0f voc=%3.0f nox=%3.0f pm2.5=%5.1f %4.1fC %4.1f%% "
                     "aqi=%3u(%s) %s %.0f%% %" PRIu32 "us",
                     s.co2_ppm, s.voc_index, s.nox_index, s.pm2_5_ugm3,
                     s.temperature_c, s.humidity_rh,
                     s.aqi.score, aeris_band_name(s.aqi.band),
                     aeris_class_name(s.event), s.event_confidence * 100.0,
                     s.inference_us);
        }

        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "%s on %s rev %d.%d, %d core(s), free heap %" PRIu32,
             FW_VERSION, CONFIG_IDF_TARGET, chip.full_revision / 100,
             chip.full_revision % 100, chip.cores, esp_get_free_heap_size());
    ESP_LOGI(TAG, "model %s: %" PRIu32 " parameter bytes, %d features, %d classes",
             AERIS_MODEL_ID, aeris_model_size_bytes(),
             AERIS_N_FEATURES, AERIS_N_CLASSES);
#if !AERIS_MODEL_TRAINED
    ESP_LOGW(TAG, "a placeholder model is flashed; run `make model` and rebuild");
#endif

    s_app_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_app_lock ? ESP_OK : ESP_ERR_NO_MEM);

    aeris_i2c_esp_t *i2c = NULL;
    ESP_ERROR_CHECK(aeris_i2c_esp_init((gpio_num_t)CONFIG_AERIS_I2C_SDA_GPIO,
                                       (gpio_num_t)CONFIG_AERIS_I2C_SCL_GPIO,
                                       CONFIG_AERIS_I2C_FREQ_HZ, &i2c));
    aeris_i2c_esp_scan(i2c);

    aeris_config_t cfg = aeris_config_default();
    cfg.scd40_temp_offset_c = (float)CONFIG_AERIS_SCD40_TEMP_OFFSET_CENTI / 100.0f;
    cfg.altitude_m          = (uint16_t)CONFIG_AERIS_ALTITUDE_M;

    aeris_app_init(&s_app, aeris_i2c_esp_hal(i2c), &cfg);
    aeris_app_set_lock(&s_app, app_lock, NULL);
    aeris_app_set_us_clock(&s_app, app_us, NULL);

    ESP_ERROR_CHECK(status_led_init((gpio_num_t)CONFIG_AERIS_LED_R_GPIO,
                                    (gpio_num_t)CONFIG_AERIS_LED_G_GPIO,
                                    (gpio_num_t)CONFIG_AERIS_LED_B_GPIO));
    ESP_ERROR_CHECK(status_led_start(&s_app));

    /* Sampling starts before the network: the sensors have a 10 s conditioning
     * step and a six-minute feature window to fill, and none of that should
     * wait on a DHCP lease. */
    BaseType_t ok = xTaskCreate(sensor_task, "sensor", 4096, NULL, 5, NULL);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_FAIL);

    ESP_ERROR_CHECK(net_start(HOSTNAME));
    ESP_ERROR_CHECK(web_server_start(&s_app, FW_VERSION));

    ESP_LOGI(TAG, "up. dashboard: http://%s.local/  http://%s/",
             HOSTNAME, net_ip_str());
    ESP_LOGI(TAG, "free heap after init: %" PRIu32, esp_get_free_heap_size());
}
