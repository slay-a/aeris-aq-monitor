#include <string.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_esp.h"

static const char *TAG = "i2c";

#define MAX_DEVS 8
#define XFER_TIMEOUT_MS 100

struct aeris_i2c_esp {
    i2c_master_bus_handle_t bus;
    struct {
        uint8_t addr;
        i2c_master_dev_handle_t dev;
    } devs[MAX_DEVS];
    int n_devs;
    uint32_t scl_hz;
    aeris_i2c_t hal;
};

/* Find or lazily create the device handle for an address. */
static i2c_master_dev_handle_t dev_for(struct aeris_i2c_esp *h, uint8_t addr)
{
    for (int i = 0; i < h->n_devs; i++) {
        if (h->devs[i].addr == addr) return h->devs[i].dev;
    }
    if (h->n_devs >= MAX_DEVS) return NULL;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = h->scl_hz,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(h->bus, &cfg, &dev) != ESP_OK) return NULL;

    h->devs[h->n_devs].addr = addr;
    h->devs[h->n_devs].dev  = dev;
    h->n_devs++;
    return dev;
}

static aeris_err_t hal_write(void *ctx, uint8_t addr, const uint8_t *buf, size_t len)
{
    struct aeris_i2c_esp *h = ctx;
    i2c_master_dev_handle_t dev = dev_for(h, addr);
    if (!dev) return AERIS_ERR_NOMEM;
    return (i2c_master_transmit(dev, buf, len, XFER_TIMEOUT_MS) == ESP_OK)
           ? AERIS_OK : AERIS_ERR_IO;
}

static aeris_err_t hal_read(void *ctx, uint8_t addr, uint8_t *buf, size_t len)
{
    struct aeris_i2c_esp *h = ctx;
    i2c_master_dev_handle_t dev = dev_for(h, addr);
    if (!dev) return AERIS_ERR_NOMEM;
    return (i2c_master_receive(dev, buf, len, XFER_TIMEOUT_MS) == ESP_OK)
           ? AERIS_OK : AERIS_ERR_IO;
}

static void hal_delay(void *ctx, uint32_t ms)
{
    (void)ctx;
    if (ms == 0) return;
    /* Short sensor gaps (1-15 ms) are shorter than a 10 ms tick, so vTaskDelay
     * would round them to 0 or overshoot badly. Busy-wait those and yield the
     * CPU for anything longer -- the 500 ms SCD40 stop, the 10 s self-test. */
    if (ms < 20) {
        esp_rom_delay_us(ms * 1000u);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static uint32_t hal_now(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

esp_err_t aeris_i2c_esp_init(gpio_num_t sda, gpio_num_t scl, uint32_t scl_hz,
                             aeris_i2c_esp_t **out)
{
    struct aeris_i2c_esp *h = calloc(1, sizeof *h);
    if (!h) return ESP_ERR_NO_MEM;

    i2c_master_bus_config_t bus_cfg = {
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .i2c_port                     = I2C_NUM_0,
        .scl_io_num                   = scl,
        .sda_io_num                   = sda,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &h->bus);
    if (err != ESP_OK) { free(h); return err; }

    h->scl_hz       = scl_hz;
    h->hal.write    = hal_write;
    h->hal.read     = hal_read;
    h->hal.delay_ms = hal_delay;
    h->hal.now_ms   = hal_now;
    h->hal.ctx      = h;

    ESP_LOGI(TAG, "bus up on SDA=%d SCL=%d at %" PRIu32 " Hz",
             (int)sda, (int)scl, scl_hz);
    *out = h;
    return ESP_OK;
}

const aeris_i2c_t *aeris_i2c_esp_hal(aeris_i2c_esp_t *h) { return &h->hal; }

void aeris_i2c_esp_scan(aeris_i2c_esp_t *h)
{
    ESP_LOGI(TAG, "scanning bus...");
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(h->bus, a, 50) == ESP_OK) {
            const char *name = (a == 0x62) ? " (SCD40)"
                             : (a == 0x59) ? " (SGP41)"
                             : (a == 0x44) ? " (SHT31)"
                             : (a == 0x12) ? " (PMSA003I)" : "";
            ESP_LOGI(TAG, "  0x%02X%s", a, name);
            found++;
        }
    }
    if (found == 0) {
        ESP_LOGE(TAG, "  nothing answered -- check 3V3, GND, and that SDA/SCL "
                      "are not swapped");
    }
}
