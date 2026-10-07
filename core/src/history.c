#include <string.h>
#include "aeris/history.h"

static uint16_t clamp_u16(float v)
{
    if (!(v == v) || v < 0.0f) return 0;
    if (v > 65535.0f) return 65535;
    return (uint16_t)(v + 0.5f);
}

void aeris_history_init(aeris_history_t *h)
{
    memset(h, 0, sizeof *h);
    /* last_push_ms left at 0 so the first sample is stored immediately. */
}

bool aeris_history_maybe_push(aeris_history_t *h, uint32_t now_ms,
                             float co2, float voc, float nox, float pm2_5,
                             float temp_c, float rh, uint8_t event)
{
    if (h->total_pushed != 0 &&
        (now_ms - h->last_push_ms) < AERIS_HISTORY_PERIOD_MS) {
        return false;
    }

    aeris_hist_rec_t *r = &h->rec[h->head];
    r->t_s       = now_ms / 1000u;
    r->co2       = clamp_u16(co2);
    r->voc       = clamp_u16(voc);
    r->nox       = clamp_u16(nox);
    r->pm2_5_x10 = clamp_u16(pm2_5 * 10.0f);
    float tc = temp_c * 10.0f;
    if (!(tc == tc)) tc = 0.0f;
    if (tc < -32768.0f) tc = -32768.0f;
    if (tc >  32767.0f) tc =  32767.0f;
    r->temp_cx10 = (int16_t)(tc >= 0 ? tc + 0.5f : tc - 0.5f);
    r->rh        = (uint8_t)((rh < 0.0f) ? 0 : (rh > 100.0f ? 100 : rh + 0.5f));
    r->event     = event;

    h->head = (uint16_t)((h->head + 1u) % AERIS_HISTORY_SLOTS);
    if (h->count < AERIS_HISTORY_SLOTS) h->count++;
    h->last_push_ms = now_ms;
    h->total_pushed++;
    return true;
}

const aeris_hist_rec_t *aeris_history_get(const aeris_history_t *h, uint16_t i)
{
    if (i >= h->count) return NULL;
    /* head points one past the newest; oldest is head - count. */
    uint16_t idx = (uint16_t)((h->head + AERIS_HISTORY_SLOTS - h->count + i)
                              % AERIS_HISTORY_SLOTS);
    return &h->rec[idx];
}
