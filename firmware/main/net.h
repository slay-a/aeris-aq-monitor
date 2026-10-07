/* net.h - bring the network up far enough to serve the dashboard.
 *
 * Station mode when credentials are configured, SoftAP otherwise. The AP
 * fallback matters more than it looks: a device that only works once it has
 * joined a WiFi network is useless on a bench with no router, and the first
 * thing you want on a new board is the dashboard.
 */
#ifndef AERIS_NET_H
#define AERIS_NET_H

#include <stdbool.h>
#include "esp_err.h"

/* Blocks until associated or until the station attempts are exhausted and the
 * SoftAP is up. Either way the HTTP server has an interface to bind to. */
esp_err_t net_start(const char *hostname);
bool      net_is_station(void);
const char *net_ip_str(void);

#endif /* AERIS_NET_H */
