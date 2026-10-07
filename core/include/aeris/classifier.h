/* classifier.h - turns per-sample model output into a stable event label.
 *
 * A raw argmax at 0.2 Hz flickers: a single borderline sample flips the
 * dashboard from "cooking" to "occupancy" and back, which reads as a broken
 * device even when the model is right on average. Two mechanisms fix that:
 *
 *   1. The probability vector is smoothed with an EMA, so evidence has to
 *      persist to move the decision.
 *   2. Leaving a state requires the challenger to beat the incumbent by a
 *      margin and to hold the lead for a minimum dwell time (hysteresis).
 *
 * Confidence reported to the API is the smoothed probability of the state
 * actually being displayed, not of the instantaneous argmax -- otherwise the
 * number contradicts the label during a transition.
 */
#ifndef AERIS_CLASSIFIER_H
#define AERIS_CLASSIFIER_H

#include <stdbool.h>
#include <stdint.h>
#include "aeris/aqi.h"
#include "aeris/model.h"

typedef struct {
    float    smooth[AERIS_N_CLASSES];
    float    alpha;            /* EMA rate on the probability vector      */
    float    enter_margin;     /* challenger must lead by this to win     */
    uint32_t min_dwell_ms;     /* ...and hold it this long                */
    int      state;            /* currently displayed class               */
    int      candidate;        /* challenger, -1 if none                  */
    uint32_t candidate_since_ms;
    uint32_t state_since_ms;
    uint32_t transitions;
    bool     initialised;
} aeris_classifier_t;

void aeris_classifier_init(aeris_classifier_t *c, float alpha,
                           float enter_margin, uint32_t min_dwell_ms);

/* Feed one inference. Returns true when the displayed state changed. */
bool aeris_classifier_update(aeris_classifier_t *c, const aeris_infer_t *inf,
                             uint32_t now_ms);

int   aeris_classifier_state(const aeris_classifier_t *c);
float aeris_classifier_confidence(const aeris_classifier_t *c);
uint32_t aeris_classifier_state_age_ms(const aeris_classifier_t *c, uint32_t now_ms);

/* The one-line explanation for a class on its own. */
const char *aeris_classifier_advice(int cls);

/* What the dashboard actually shows. The class answers "what is happening now";
 * the air-quality band answers "how is the air right now". They disagree often
 * and legitimately -- the commonest case is a room that is quiet again but has
 * not finished clearing after cooking -- and advice drawn from the class alone
 * then tells you to do nothing while PM2.5 is still at 40. */
const char *aeris_advice_for(int cls, aeris_band_t band);

#endif /* AERIS_CLASSIFIER_H */
