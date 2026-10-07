#include <string.h>
#include "aeris/classifier.h"
#include "test.h"

static aeris_infer_t one_hot(int cls, float p)
{
    aeris_infer_t inf;
    memset(&inf, 0, sizeof inf);
    float rest = (1.0f - p) / (float)(AERIS_N_CLASSES - 1);
    for (int i = 0; i < AERIS_N_CLASSES; i++) inf.prob[i] = rest;
    inf.prob[cls] = p;
    inf.argmax = cls;
    return inf;
}

void suite_classifier(void)
{
    SUITE("classifier");

    aeris_classifier_t c;
    aeris_classifier_init(&c, 0.25f, 0.12f, 20000);
    CHECK_EQ_I(aeris_classifier_state(&c), 0);

    /* Sustained confident evidence must eventually switch the state. */
    uint32_t t = 0;
    bool changed = false;
    aeris_infer_t cook = one_hot(2, 0.9f);
    for (int i = 0; i < 40; i++) {
        t += 5000;
        if (aeris_classifier_update(&c, &cook, t)) changed = true;
    }
    CHECK(changed);
    CHECK_EQ_I(aeris_classifier_state(&c), 2);
    CHECK(aeris_classifier_confidence(&c) > 0.7f);

    /* The dwell requirement must be honoured: a state cannot change twice in
     * less than min_dwell_ms. */
    aeris_classifier_init(&c, 0.25f, 0.12f, 20000);
    t = 0;
    for (int i = 0; i < 40; i++) { t += 5000; aeris_classifier_update(&c, &cook, t); }
    uint32_t switched_at = c.state_since_ms;
    aeris_infer_t vent = one_hot(3, 0.95f);
    int changes = 0;
    for (int i = 0; i < 3; i++) {
        t += 5000;
        if (aeris_classifier_update(&c, &vent, t)) changes++;
    }
    CHECK_MSG(changes == 0, "state changed %d times inside the dwell window", changes);
    CHECK(c.state_since_ms == switched_at);

    /* ---- the behaviour this exists for ------------------------------- */
    /* A single borderline sample in the middle of a steady run must not flip
     * the displayed state. Raw argmax would flip on every one of these. */
    aeris_classifier_init(&c, 0.25f, 0.12f, 20000);
    t = 0;
    for (int i = 0; i < 60; i++) { t += 5000; aeris_classifier_update(&c, &cook, t); }
    CHECK_EQ_I(aeris_classifier_state(&c), 2);
    uint32_t before = c.transitions;
    aeris_infer_t blip = one_hot(1, 0.55f);
    for (int i = 0; i < 12; i++) {
        t += 5000;
        /* Alternate: one odd sample, then the true class again. */
        aeris_classifier_update(&c, (i % 2) ? &cook : &blip, t);
    }
    CHECK_EQ_I(c.transitions, before);
    CHECK_EQ_I(aeris_classifier_state(&c), 2);

    /* But a genuine sustained change must get through, and reasonably fast:
     * within about two minutes of the room actually changing. */
    uint32_t change_start = t;
    for (int i = 0; i < 60; i++) {
        t += 5000;
        if (aeris_classifier_update(&c, &vent, t)) break;
    }
    CHECK_EQ_I(aeris_classifier_state(&c), 3);
    CHECK_MSG(t - change_start <= 120000,
              "took %u ms to accept a sustained change", t - change_start);

    /* Confidence must describe the state on screen, not the instantaneous
     * argmax -- otherwise the number contradicts the label mid-transition. */
    aeris_classifier_init(&c, 0.25f, 0.12f, 20000);
    t = 0;
    for (int i = 0; i < 60; i++) { t += 5000; aeris_classifier_update(&c, &cook, t); }
    t += 5000;
    aeris_classifier_update(&c, &vent, t);       /* one contrary sample */
    CHECK_EQ_I(aeris_classifier_state(&c), 2);
    CHECK_NEAR(aeris_classifier_confidence(&c), c.smooth[2], 0.0001f);

    /* Age tracking. */
    CHECK_EQ_I(aeris_classifier_state_age_ms(&c, t), t - c.state_since_ms);

    /* Advice strings must exist for every class and be distinct. */
    for (int i = 0; i < AERIS_N_CLASSES; i++) {
        CHECK(strlen(aeris_classifier_advice(i)) > 10);
    }
    CHECK_STR(aeris_classifier_advice(-1), "");
    CHECK_STR(aeris_classifier_advice(AERIS_N_CLASSES), "");

    /* Advice has to account for the air-quality band, not just the class. A
     * quiet room that has not finished clearing after cooking must not be told
     * there is nothing to do. */
    CHECK_STR(aeris_advice_for(0, AERIS_BAND_GOOD),
              aeris_classifier_advice(0));
    CHECK(strcmp(aeris_advice_for(0, AERIS_BAND_POOR),
                 aeris_classifier_advice(0)) != 0);
    CHECK(strcmp(aeris_advice_for(0, AERIS_BAND_BAD),
                 aeris_classifier_advice(0)) != 0);
    CHECK(strstr(aeris_advice_for(0, AERIS_BAND_POOR), "not cleared") != NULL);
    /* Ventilating but still bad should acknowledge that, not claim success. */
    CHECK(strcmp(aeris_advice_for(3, AERIS_BAND_BAD),
                 aeris_classifier_advice(3)) != 0);
    /* Every combination must produce something usable. */
    for (int cls = 0; cls < AERIS_N_CLASSES; cls++) {
        for (int b = AERIS_BAND_GOOD; b <= AERIS_BAND_BAD; b++) {
            CHECK(strlen(aeris_advice_for(cls, (aeris_band_t)b)) > 10);
        }
    }
}
