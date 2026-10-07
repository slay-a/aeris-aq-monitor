/* history.h - compact ring of past readings for the dashboard chart.
 *
 * Stored quantized rather than as floats: 90 minutes of history at 30 s
 * resolution is 180 records, and at 12 bytes each that is 2.1 kB of the
 * ESP32-C3's 400 kB SRAM. The same data as floats, kept at the 5 s sampling
 * cadence, would be 26 kB for no visible benefit on a 900 px chart.
 */
#ifndef AERIS_HISTORY_H
#define AERIS_HISTORY_H

#include <stdbool.h>
#include <stdint.h>

#define AERIS_HISTORY_SLOTS    180
#define AERIS_HISTORY_PERIOD_MS 30000

typedef struct {
    uint32_t t_s;        /* seconds since boot            */
    uint16_t co2;        /* ppm                           */
    uint16_t voc;        /* index                         */
    uint16_t nox;        /* index                         */
    uint16_t pm2_5_x10;  /* ug/m3 * 10                    */
    int16_t  temp_cx10;  /* deg C * 10                    */
    uint8_t  rh;         /* %                             */
    uint8_t  event;      /* class id at the time          */
} aeris_hist_rec_t;

typedef struct {
    aeris_hist_rec_t rec[AERIS_HISTORY_SLOTS];
    uint16_t head;
    uint16_t count;
    uint32_t last_push_ms;
    uint32_t total_pushed;
} aeris_history_t;

void aeris_history_init(aeris_history_t *h);
/* Rate-limited to AERIS_HISTORY_PERIOD_MS; returns true if it stored. */
bool aeris_history_maybe_push(aeris_history_t *h, uint32_t now_ms,
                              float co2, float voc, float nox, float pm2_5,
                              float temp_c, float rh, uint8_t event);
/* Oldest-first iteration: i in [0, count). */
const aeris_hist_rec_t *aeris_history_get(const aeris_history_t *h, uint16_t i);

#endif /* AERIS_HISTORY_H */
