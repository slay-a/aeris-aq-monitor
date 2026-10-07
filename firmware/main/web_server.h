/* web_server.h - esp_http_server wrapper.
 *
 * Runs in its own task (the httpd's own), reads the published snapshot under the
 * app's lock, and serialises with the same core/src/api.c functions the host
 * simulator uses -- so the payload is identical to the one the test suite
 * validates.
 */
#ifndef AERIS_WEB_SERVER_H
#define AERIS_WEB_SERVER_H

#include "aeris/app.h"
#include "esp_err.h"

esp_err_t web_server_start(aeris_app_t *app, const char *fw_version);
void      web_server_stop(void);

#endif /* AERIS_WEB_SERVER_H */
