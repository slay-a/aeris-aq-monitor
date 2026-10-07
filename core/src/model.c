#include <math.h>
#include <string.h>
#include "aeris/model.h"

/* Clamp an int32 accumulator to the int8 range after requantization. */
static int8_t sat8(int32_t v)
{
    if (v < -128) return -128;
    if (v >  127) return  127;
    return (int8_t)v;
}

/* Rounding right shift, symmetric about zero. Plain >> would bias every
 * negative activation downward, which across three layers is enough to move
 * the argmax on borderline samples. */
static int32_t rshift_round(int32_t x, int shift)
{
    if (shift <= 0) return x;
    int32_t half = 1 << (shift - 1);
    return (x >= 0) ? ((x + half) >> shift)
                    : -(((-x) + half) >> shift);
}

void aeris_model_quantize_input(const float *features, int8_t *q)
{
    for (int i = 0; i < AERIS_N_FEATURES; i++) {
        float z = (features[i] - AERIS_FEAT_MEAN[i]) * AERIS_FEAT_INV_SCALE[i];
        float scaled = z / AERIS_INPUT_SCALE;

        /* A NaN feature means a sensor dropped out mid-window. Zero after
         * standardisation is the training-set mean, which is the least
         * misleading substitute; letting NaN through poisons every
         * accumulator downstream. */
        if (!(scaled == scaled)) scaled = 0.0f;

        /* Clamp in the float domain, before any integer conversion. A feature
         * far outside the training range scales past INT32_MAX, and both the
         * cast and lrintf() are then undefined -- in practice it wraps to a
         * large negative value and saturates to -128, i.e. the opposite end of
         * the range from the truth. */
        if (scaled >  127.0f) { q[i] =  127; continue; }
        if (scaled < -128.0f) { q[i] = -128; continue; }
        q[i] = sat8((int32_t)lrintf(scaled));
    }
}

/* One dense layer: int8 x int8 -> int32, add int32 bias, requantize by a
 * per-layer (multiplier, shift) pair, optional ReLU. */
static void dense(const int8_t *in, int n_in,
                  const int8_t *w, const int32_t *b, int n_out,
                  int32_t mult, int shift, bool relu,
                  int8_t *out, int32_t *out_i32)
{
    for (int o = 0; o < n_out; o++) {
        int32_t acc = b[o];
        const int8_t *row = &w[(size_t)o * (size_t)n_in];
        for (int i = 0; i < n_in; i++) {
            acc += (int32_t)row[i] * (int32_t)in[i];
        }
        if (out_i32) out_i32[o] = acc;
        if (out) {
            int64_t scaled = (int64_t)acc * (int64_t)mult;
            int32_t r = rshift_round((int32_t)(scaled >> 16), shift);
            if (relu && r < 0) r = 0;
            out[o] = sat8(r);
        }
    }
}

void aeris_model_infer(const float *features, aeris_infer_t *out)
{
    int8_t  qin[AERIS_N_FEATURES];
    int8_t  h1[AERIS_MODEL_H1];
    int8_t  h2[AERIS_MODEL_H2];
    int32_t logits_i32[AERIS_N_CLASSES];

    aeris_model_quantize_input(features, qin);

    dense(qin, AERIS_N_FEATURES, AERIS_W1, AERIS_B1, AERIS_MODEL_H1,
          AERIS_M1_MULT, AERIS_M1_SHIFT, true, h1, NULL);
    dense(h1, AERIS_MODEL_H1, AERIS_W2, AERIS_B2, AERIS_MODEL_H2,
          AERIS_M2_MULT, AERIS_M2_SHIFT, true, h2, NULL);
    dense(h2, AERIS_MODEL_H2, AERIS_W3, AERIS_B3, AERIS_N_CLASSES,
          0, 0, false, NULL, logits_i32);

    /* Dequantize the output layer's accumulators back to real logits so the
     * softmax -- and therefore the confidence the dashboard shows -- is on a
     * meaningful scale. */
    float maxlog = -1e30f;
    for (int c = 0; c < AERIS_N_CLASSES; c++) {
        out->logit[c] = (float)logits_i32[c] * AERIS_OUTPUT_SCALE;
        if (out->logit[c] > maxlog) { maxlog = out->logit[c]; out->argmax = c; }
    }

    float sum = 0.0f;
    for (int c = 0; c < AERIS_N_CLASSES; c++) {
        out->prob[c] = expf(out->logit[c] - maxlog);
        sum += out->prob[c];
    }
    for (int c = 0; c < AERIS_N_CLASSES; c++) out->prob[c] /= sum;
    out->inference_us = 0;
}

uint32_t aeris_model_size_bytes(void)
{
    return (uint32_t)(sizeof(AERIS_W1) + sizeof(AERIS_B1) +
                      sizeof(AERIS_W2) + sizeof(AERIS_B2) +
                      sizeof(AERIS_W3) + sizeof(AERIS_B3));
}

const char *aeris_class_name(int cls)
{
    if (cls < 0 || cls >= AERIS_N_CLASSES) return "unknown";
    return AERIS_CLASS_NAMES[cls];
}
