#include <string.h>
#include "aeris/api.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "web_server.h"

static const char *TAG = "http";

/* dashboard.html, linked in by EMBED_FILES in main/CMakeLists.txt. */
extern const uint8_t dashboard_start[] asm("_binary_dashboard_html_start");
extern const uint8_t dashboard_end[]   asm("_binary_dashboard_html_end");

static httpd_handle_t   s_server;
static aeris_app_t     *s_app;
static const char      *s_version;

/* One shared response buffer rather than 12 kB on the httpd task's stack.
 * esp_http_server serialises requests on a single task by default, but the
 * mutex makes that an invariant the code states rather than one it assumes. */
static char            *s_buf;
static SemaphoreHandle_t s_buf_lock;

static esp_err_t send_json(httpd_req_t *req, size_t len)
{
    if (len == 0) {
        ESP_LOGE(TAG, "%s: response did not fit its buffer", req->uri);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "response buffer overflow");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* The dashboard is served from the device itself, so no CORS header is
     * needed. One is added anyway so the API can be polled from a laptop-side
     * script or a Grafana JSON datasource during bring-up. */
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, s_buf, (ssize_t)len);
}

static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)dashboard_start,
                           (ssize_t)(dashboard_end - dashboard_start - 1));
}

static esp_err_t h_live(httpd_req_t *req)
{
    aeris_snapshot_t s;
    aeris_app_snapshot(s_app, &s);
    xSemaphoreTake(s_buf_lock, portMAX_DELAY);
    size_t n = aeris_api_live(&s, s_buf, AERIS_API_HISTORY_BUF);
    esp_err_t e = send_json(req, n);
    xSemaphoreGive(s_buf_lock);
    return e;
}

static esp_err_t h_history(httpd_req_t *req)
{
    xSemaphoreTake(s_buf_lock, portMAX_DELAY);
    /* The history ring is written by the sampling task, so the copy has to
     * happen under the app lock, not just the buffer lock. */
    size_t n = aeris_api_history(&s_app->hist, s_buf, AERIS_API_HISTORY_BUF);
    esp_err_t e = send_json(req, n);
    xSemaphoreGive(s_buf_lock);
    return e;
}

static esp_err_t h_status(httpd_req_t *req)
{
    aeris_snapshot_t s;
    aeris_app_snapshot(s_app, &s);
    xSemaphoreTake(s_buf_lock, portMAX_DELAY);
    size_t n = aeris_api_status(&s, s_app, s_version, s_buf, AERIS_API_HISTORY_BUF);
    esp_err_t e = send_json(req, n);
    xSemaphoreGive(s_buf_lock);
    return e;
}

static esp_err_t h_debug(httpd_req_t *req)
{
    aeris_snapshot_t s;
    aeris_app_snapshot(s_app, &s);
    xSemaphoreTake(s_buf_lock, portMAX_DELAY);
    size_t n = aeris_api_debug(&s, s_buf, AERIS_API_HISTORY_BUF);
    esp_err_t e = send_json(req, n);
    xSemaphoreGive(s_buf_lock);
    return e;
}

/* CSV of the history ring, so a bring-up session can be pulled straight into a
 * spreadsheet or into ml/ as labelled data with `curl -o`. */
static esp_err_t h_csv(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=aeris-history.csv");
    char line[128];
    int n = snprintf(line, sizeof line,
                     "t_s,co2_ppm,voc_index,nox_index,pm2_5,temp_c,rh,event\n");
    esp_err_t e = httpd_resp_send_chunk(req, line, n);
    for (uint16_t i = 0; i < s_app->hist.count && e == ESP_OK; i++) {
        const aeris_hist_rec_t *r = aeris_history_get(&s_app->hist, i);
        if (!r) break;
        n = snprintf(line, sizeof line, "%lu,%u,%u,%u,%.1f,%.1f,%u,%s\n",
                     (unsigned long)r->t_s, r->co2, r->voc, r->nox,
                     r->pm2_5_x10 / 10.0, r->temp_cx10 / 10.0, r->rh,
                     aeris_class_name(r->event));
        e = httpd_resp_send_chunk(req, line, n);
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return e;
}

static const httpd_uri_t k_uris[] = {
    { .uri = "/",             .method = HTTP_GET, .handler = h_root    },
    { .uri = "/api/live",     .method = HTTP_GET, .handler = h_live    },
    { .uri = "/api/history",  .method = HTTP_GET, .handler = h_history },
    { .uri = "/api/status",   .method = HTTP_GET, .handler = h_status  },
    { .uri = "/api/debug",    .method = HTTP_GET, .handler = h_debug   },
    { .uri = "/api/history.csv", .method = HTTP_GET, .handler = h_csv  },
};

esp_err_t web_server_start(aeris_app_t *app, const char *fw_version)
{
    s_app     = app;
    s_version = fw_version;

    s_buf = malloc(AERIS_API_HISTORY_BUF);
    if (!s_buf) return ESP_ERR_NO_MEM;
    s_buf_lock = xSemaphoreCreateMutex();
    if (!s_buf_lock) { free(s_buf); s_buf = NULL; return ESP_ERR_NO_MEM; }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size      = 6144;
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }
    for (size_t i = 0; i < sizeof k_uris / sizeof k_uris[0]; i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &k_uris[i]));
    }
    ESP_LOGI(TAG, "serving %u endpoints, dashboard is %u bytes",
             (unsigned)(sizeof k_uris / sizeof k_uris[0]),
             (unsigned)(dashboard_end - dashboard_start - 1));
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server) { httpd_stop(s_server); s_server = NULL; }
}
