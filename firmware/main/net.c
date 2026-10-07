#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mdns.h"
#include "net.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "net";

#define STA_MAX_ATTEMPTS 6

static EventGroupHandle_t s_events;
#define BIT_GOT_IP  BIT0
#define BIT_FAILED  BIT1

static int  s_attempts;
static bool s_is_station;
static char s_ip[16] = "0.0.0.0";

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (++s_attempts < STA_MAX_ATTEMPTS) {
            ESP_LOGW(TAG, "disconnected, retry %d/%d", s_attempts, STA_MAX_ATTEMPTS);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_events, BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        s_attempts = 0;
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

static esp_err_t start_softap(const char *hostname)
{
    esp_netif_create_default_wifi_ap();

    /* Unique SSID from the MAC, so two of these on a bench do not collide. */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    wifi_config_t cfg = { 0 };
    int n = snprintf((char *)cfg.ap.ssid, sizeof cfg.ap.ssid,
                     "aeris-%02X%02X", mac[4], mac[5]);
    cfg.ap.ssid_len       = (uint8_t)n;
    cfg.ap.channel        = 1;
    cfg.ap.max_connection = 4;
    cfg.ap.authmode       = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    strncpy(s_ip, "192.168.4.1", sizeof s_ip - 1);
    s_is_station = false;
    ESP_LOGW(TAG, "no station link; SoftAP \"%s\" is open at %s",
             (char *)cfg.ap.ssid, s_ip);
    (void)hostname;
    return ESP_OK;
}

esp_err_t net_start(const char *hostname)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_events = xEventGroupCreate();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL, NULL));

    const char *ssid = CONFIG_AERIS_WIFI_SSID;
    if (ssid[0] == '\0') {
        ESP_LOGW(TAG, "no SSID configured (idf.py menuconfig -> Aeris)");
        ESP_ERROR_CHECK(start_softap(hostname));
    } else {
        esp_netif_t *sta = esp_netif_create_default_wifi_sta();
        esp_netif_set_hostname(sta, hostname);

        wifi_config_t cfg = { 0 };
        strncpy((char *)cfg.sta.ssid, ssid, sizeof cfg.sta.ssid - 1);
        strncpy((char *)cfg.sta.password, CONFIG_AERIS_WIFI_PASSWORD,
                sizeof cfg.sta.password - 1);
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
        /* The radio spends most of its life idle between 5 s samples; modem
         * sleep cuts the average draw substantially and costs only a little
         * first-packet latency on the dashboard. */
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
        ESP_ERROR_CHECK(esp_wifi_start());

        EventBits_t bits = xEventGroupWaitBits(
            s_events, BIT_GOT_IP | BIT_FAILED, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(25000));

        if (bits & BIT_GOT_IP) {
            s_is_station = true;
            ESP_LOGI(TAG, "joined \"%s\" as %s", ssid, s_ip);
        } else {
            ESP_LOGW(TAG, "could not join \"%s\"; falling back to SoftAP", ssid);
            esp_wifi_stop();
            esp_wifi_set_mode(WIFI_MODE_NULL);
            ESP_ERROR_CHECK(start_softap(hostname));
        }
    }

    /* mDNS so the dashboard is at http://aeris.local rather than at whatever
     * address the router handed out this week. */
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(hostname);
        mdns_instance_name_set("Aeris AQ Monitor");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "dashboard at http://%s.local/ (or http://%s/)",
                 hostname, s_ip);
    }
    return ESP_OK;
}

bool net_is_station(void) { return s_is_station; }
const char *net_ip_str(void) { return s_ip; }
