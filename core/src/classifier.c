#include <string.h>
#include "aeris/classifier.h"

void aeris_classifier_init(aeris_classifier_t *c, float alpha,
                           float enter_margin, uint32_t min_dwell_ms)
{
    memset(c, 0, sizeof *c);
    c->alpha        = alpha;
    c->enter_margin = enter_margin;
    c->min_dwell_ms = min_dwell_ms;
    c->candidate    = -1;
    c->state        = 0;          /* baseline until proven otherwise */
}

bool aeris_classifier_update(aeris_classifier_t *c, const aeris_infer_t *inf,
                             uint32_t now_ms)
{
    if (!c->initialised) {
        memcpy(c->smooth, inf->prob, sizeof c->smooth);
        c->initialised    = true;
        c->state_since_ms = now_ms;
        return false;
    }

    for (int i = 0; i < AERIS_N_CLASSES; i++) {
        c->smooth[i] += c->alpha * (inf->prob[i] - c->smooth[i]);
    }

    int   best = 0;
    float best_p = -1.0f;
    for (int i = 0; i < AERIS_N_CLASSES; i++) {
        if (c->smooth[i] > best_p) { best_p = c->smooth[i]; best = i; }
    }

    if (best == c->state) {
        /* Incumbent still leading: drop any pending challenger. */
        c->candidate = -1;
        return false;
    }

    /* The margin is measured against the state on screen, not against the
     * runner-up, so a state only loses when something clearly beats it. */
    if (best_p - c->smooth[c->state] < c->enter_margin) {
        c->candidate = -1;
        return false;
    }

    if (c->candidate != best) {
        c->candidate          = best;
        c->candidate_since_ms = now_ms;
        return false;
    }

    if (now_ms - c->candidate_since_ms < c->min_dwell_ms) return false;

    c->state          = best;
    c->state_since_ms = now_ms;
    c->candidate      = -1;
    c->transitions++;
    return true;
}

int aeris_classifier_state(const aeris_classifier_t *c) { return c->state; }

float aeris_classifier_confidence(const aeris_classifier_t *c)
{
    return c->initialised ? c->smooth[c->state] : 0.0f;
}

uint32_t aeris_classifier_state_age_ms(const aeris_classifier_t *c, uint32_t now_ms)
{
    return now_ms - c->state_since_ms;
}

const char *aeris_advice_for(int cls, aeris_band_t band)
{
    /* Nothing is driving the air now, but it has not recovered yet. */
    if (cls == 0 && band >= AERIS_BAND_POOR) {
        return "Nothing is adding to it now, but the air has not cleared. "
               "Open a window to speed it up.";
    }
    if (cls == 0 && band == AERIS_BAND_FAIR) {
        return "Settling back down. Nothing urgent.";
    }
    /* Already ventilating and still bad: say so rather than claiming success. */
    if (cls == 3 && band >= AERIS_BAND_POOR) {
        return "Fresh air is coming in, but it is still clearing. Give it longer.";
    }
    return aeris_classifier_advice(cls);
}

const char *aeris_classifier_advice(int cls)
{
    switch (cls) {
    case 0: return "Air is stable. Nothing to do.";
    case 1: return "Room is occupied and CO2 is building. Crack a window or door.";
    case 2: return "Looks like cooking. Run the extractor fan.";
    case 3: return "Fresh air is coming in. CO2 and VOC are clearing.";
    case 4: return "Solvent or aerosol detected. Ventilate and avoid the room briefly.";
    default: return "";
    }
}
