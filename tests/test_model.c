/* Model tests.
 *
 * The parity block is the important one: ml/train.py emits the feature vectors
 * from a held-out split together with the int8 input, the int32 logits and the
 * argmax that *its own* simulation of this kernel produced. If the C here and
 * the Python there ever disagree by a single accumulator bit, this fails. That
 * is the only way to be sure the accuracy measured in training is the accuracy
 * the device will deliver.
 */
#include <math.h>
#include <string.h>
#include "aeris/model.h"
#include "test.h"
#include "vectors_model.h"

void suite_model(void)
{
    SUITE("model");

    /* ---- invariants that hold for any trained network ----------------- */
    float f[AERIS_N_FEATURES];
    for (int i = 0; i < AERIS_N_FEATURES; i++) f[i] = 0.0f;

    aeris_infer_t out;
    aeris_model_infer(f, &out);
    CHECK(out.argmax >= 0 && out.argmax < AERIS_N_CLASSES);

    float sum = 0.0f;
    for (int c = 0; c < AERIS_N_CLASSES; c++) {
        CHECK(out.prob[c] >= 0.0f && out.prob[c] <= 1.0f);
        CHECK_MSG(out.prob[c] == out.prob[c], "class %d probability is NaN", c);
        sum += out.prob[c];
    }
    CHECK_NEAR(sum, 1.0f, 1e-4f);

    /* argmax must agree with the largest probability. */
    int best = 0;
    for (int c = 1; c < AERIS_N_CLASSES; c++) if (out.prob[c] > out.prob[best]) best = c;
    CHECK_EQ_I(out.argmax, best);

    /* Absurd inputs must saturate the int8 input, not wrap and produce a
     * confident wrong answer. */
    for (int i = 0; i < AERIS_N_FEATURES; i++) f[i] = 1e9f;
    int8_t q[AERIS_N_FEATURES];
    aeris_model_quantize_input(f, q);
    for (int i = 0; i < AERIS_N_FEATURES; i++) CHECK_EQ_I(q[i], 127);
    aeris_model_infer(f, &out);
    CHECK(out.argmax >= 0 && out.argmax < AERIS_N_CLASSES);
    for (int c = 0; c < AERIS_N_CLASSES; c++) CHECK(out.prob[c] == out.prob[c]);

    for (int i = 0; i < AERIS_N_FEATURES; i++) f[i] = -1e9f;
    aeris_model_quantize_input(f, q);
    for (int i = 0; i < AERIS_N_FEATURES; i++) CHECK_EQ_I(q[i], -128);
    aeris_model_infer(f, &out);
    CHECK(out.argmax >= 0 && out.argmax < AERIS_N_CLASSES);

    /* Inference must be deterministic -- no uninitialised accumulator. */
    for (int i = 0; i < AERIS_N_FEATURES; i++) f[i] = 0.37f * (float)i;
    aeris_infer_t a, b;
    aeris_model_infer(f, &a);
    aeris_model_infer(f, &b);
    CHECK_EQ_I(a.argmax, b.argmax);
    CHECK_EQ_I(memcmp(a.logit, b.logit, sizeof a.logit), 0);

    /* The model must fit comfortably in flash: this is a microcontroller. */
    CHECK_MSG(aeris_model_size_bytes() < 16384,
              "model is %u bytes", aeris_model_size_bytes());

    CHECK_STR(aeris_class_name(-1), "unknown");
    CHECK_STR(aeris_class_name(AERIS_N_CLASSES), "unknown");
    for (int c = 0; c < AERIS_N_CLASSES; c++) {
        CHECK(strcmp(aeris_class_name(c), "unknown") != 0);
    }

#if AERIS_HAVE_VECTORS
    /* ---- bit-exact parity with the trainer --------------------------- */
    int q_mismatch = 0, logit_mismatch = 0, argmax_mismatch = 0;
    for (int v = 0; v < AERIS_N_VECTORS; v++) {
        const float *feat = AERIS_VEC_FEATURES[v];

        int8_t qc[AERIS_N_FEATURES];
        aeris_model_quantize_input(feat, qc);
        for (int i = 0; i < AERIS_N_FEATURES; i++) {
            if (qc[i] != AERIS_VEC_QINPUT[v][i]) {
                if (q_mismatch < 3) {
                    printf("  vector %d feature %d (%s): C quantized %d, "
                           "Python %d\n", v, i, aeris_feature_name(i),
                           qc[i], AERIS_VEC_QINPUT[v][i]);
                }
                q_mismatch++;
            }
        }

        aeris_infer_t o;
        aeris_model_infer(feat, &o);
        for (int c = 0; c < AERIS_N_CLASSES; c++) {
            float expect = (float)AERIS_VEC_LOGITS[v][c] * AERIS_OUTPUT_SCALE;
            if (fabsf(o.logit[c] - expect) > 1e-4f) {
                if (logit_mismatch < 3) {
                    printf("  vector %d class %d: C logit %.6f, Python %.6f\n",
                           v, c, (double)o.logit[c], (double)expect);
                }
                logit_mismatch++;
            }
        }
        if (o.argmax != AERIS_VEC_ARGMAX[v]) argmax_mismatch++;
    }
    CHECK_MSG(q_mismatch == 0, "%d input-quantization mismatches vs the trainer",
              q_mismatch);
    CHECK_MSG(logit_mismatch == 0, "%d logit mismatches vs the trainer",
              logit_mismatch);
    CHECK_MSG(argmax_mismatch == 0, "%d argmax mismatches vs the trainer "
              "across %d vectors", argmax_mismatch, AERIS_N_VECTORS);
    if (q_mismatch == 0 && logit_mismatch == 0 && argmax_mismatch == 0) {
        printf("  parity: %d vectors replayed through the C kernel, bit-exact\n",
               AERIS_N_VECTORS);
    }
#else
    printf("  (no parity vectors yet -- run `make model`)\n");
#endif
}
