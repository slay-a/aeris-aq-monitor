/* api.h - response bodies for the device's JSON API.
 *
 * Kept out of the HTTP layer so the simulator serves byte-identical payloads
 * and the test suite can assert on them without a network stack.
 *
 * Endpoints:
 *   GET /api/live     current readings, event label, AQI        (~700 B)
 *   GET /api/history  the chart series, oldest first            (~9 kB)
 *   GET /api/status   sensor presence, error counters, model    (~600 B)
 *   GET /api/debug    the raw feature vector, named             (~700 B)
 */
#ifndef AERIS_API_H
#define AERIS_API_H

#include <stddef.h>
#include "aeris/app.h"
#include "aeris/history.h"

#define AERIS_API_LIVE_BUF    1024
#define AERIS_API_HISTORY_BUF 12288
#define AERIS_API_STATUS_BUF  1024
#define AERIS_API_DEBUG_BUF   1024

size_t aeris_api_live(const aeris_snapshot_t *s, char *buf, size_t cap);
size_t aeris_api_history(const aeris_history_t *h, char *buf, size_t cap);
size_t aeris_api_status(const aeris_snapshot_t *s, const aeris_app_t *a,
                        const char *fw_version, char *buf, size_t cap);
size_t aeris_api_debug(const aeris_snapshot_t *s, char *buf, size_t cap);

#endif /* AERIS_API_H */
