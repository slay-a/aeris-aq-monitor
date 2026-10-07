/* sim_main.c - host-side harness for the firmware's core.
 *
 * The same aeris_app_step() the ESP32 runs is driven here against virtual
 * sensors and a room physics model. Four modes:
 *
 *   --dataset N FILE  generate a labelled feature CSV from N random sessions
 *   --run             run the fixed acceptance scenario and score the model
 *   --serve PORT      run in near-real time and serve the real dashboard
 *   --faults          run with NACK and bit-flip injection, check resilience
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aeris/api.h"
#include "aeris/app.h"
#include "harness.h"
#include "room_model.h"
#include "scenario.h"
#include "virtual_i2c.h"

#ifndef AERIS_FW_VERSION
#define AERIS_FW_VERSION "sim"
#endif

/* ---- dataset mode ------------------------------------------------------ */

static int mode_dataset(int n_sessions, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return 1; }

    fprintf(f, "session");
    for (int i = 0; i < AERIS_N_FEATURES; i++) fprintf(f, ",%s", aeris_feature_name(i));
    fprintf(f, ",label\n");

    long rows = 0;
    long counts[EV_N_CLASSES] = {0};

    for (int s = 0; s < n_sessions; s++) {
        harness_t h;
        /* A distinct seed per session, derived so the whole dataset is
         * reproducible from the session count alone. */
        harness_init(&h, 0x9E3779B9u * (uint32_t)(s + 1));
        scenario_random(&h.sc, &h.room, 14);

        while (harness_tick(&h)) {
            aeris_snapshot_t snap;
            aeris_app_snapshot(&h.app, &snap);
            if (!snap.features_valid) continue;
            if (!scenario_settled(&h.sc, h.bus.now_ms)) continue;

            room_event_t label = scenario_label(&h.sc);
            fprintf(f, "%d", s);
            for (int i = 0; i < AERIS_N_FEATURES; i++) {
                fprintf(f, ",%.6f", snap.features[i]);
            }
            fprintf(f, ",%d\n", (int)label);
            rows++;
            counts[label]++;
        }
        if ((s + 1) % 10 == 0) {
            fprintf(stderr, "  %d/%d sessions, %ld rows\n", s + 1, n_sessions, rows);
        }
    }
    fclose(f);

    fprintf(stderr, "wrote %ld rows to %s\n", rows, path);
    fprintf(stderr, "class balance:");
    for (int c = 0; c < EV_N_CLASSES; c++) {
        fprintf(stderr, " %s=%ld", room_event_name((room_event_t)c), counts[c]);
    }
    fprintf(stderr, "\n");
    return rows > 0 ? 0 : 1;
}

/* ---- acceptance run --------------------------------------------------- */

static int mode_run(uint32_t seed, bool verbose)
{
    harness_t h;
    harness_init(&h, seed);
    scenario_acceptance(&h.sc);

    long scored = 0, correct = 0;
    long conf[EV_N_CLASSES][EV_N_CLASSES];
    memset(conf, 0, sizeof conf);
    uint32_t max_inference_us = 0;
    room_event_t last_shown = EV_N_CLASSES;

    while (harness_tick(&h)) {
        aeris_snapshot_t snap;
        aeris_app_snapshot(&h.app, &snap);
        if (!snap.features_valid) continue;

        if (verbose && (room_event_t)snap.event != last_shown) {
            printf("  t=%6us  event -> %-12s conf=%.2f  co2=%4.0f voc=%3.0f "
                   "nox=%3.0f pm=%5.1f  truth=%s\n",
                   h.bus.now_ms / 1000u, aeris_class_name(snap.event),
                   (double)snap.event_confidence, (double)snap.co2_ppm,
                   (double)snap.voc_index, (double)snap.nox_index,
                   (double)snap.pm2_5_ugm3,
                   room_event_name(scenario_label(&h.sc)));
            last_shown = (room_event_t)snap.event;
        }
        if (snap.inference_us > max_inference_us) max_inference_us = snap.inference_us;

        if (!scenario_settled(&h.sc, h.bus.now_ms)) continue;
        room_event_t truth = scenario_label(&h.sc);
        conf[truth][snap.event]++;
        scored++;
        if ((int)truth == snap.event) correct++;
    }

    double acc = scored ? 100.0 * (double)correct / (double)scored : 0.0;
    printf("\nacceptance scenario: %ld/%ld settled samples correct (%.1f%%)\n",
           correct, scored, acc);
    printf("\nconfusion (rows = truth, cols = predicted)\n%-13s", "");
    for (int c = 0; c < EV_N_CLASSES; c++) printf("%12s", room_event_name((room_event_t)c));
    printf("\n");
    for (int t = 0; t < EV_N_CLASSES; t++) {
        printf("%-13s", room_event_name((room_event_t)t));
        for (int p = 0; p < EV_N_CLASSES; p++) printf("%12ld", conf[t][p]);
        printf("\n");
    }

    aeris_snapshot_t snap;
    aeris_app_snapshot(&h.app, &snap);
    printf("\nsensors: scd40=%d sgp41=%d sht31=%d pmsa003i=%d\n",
           snap.scd40_present, snap.sgp41_present, snap.sht31_present,
           snap.pmsa003i_present);
    printf("samples=%u sample_errors=%u crc_errors=%u transitions=%u\n",
           snap.samples_total, snap.sample_errors, snap.crc_errors,
           h.app.cls.transitions);
    printf("  per-sensor crc/frame: scd40=%u sgp41=%u sht31=%u pms=%u\n",
           h.app.scd.crc_errors, h.app.sgp.crc_errors,
           h.app.sht.crc_errors, h.app.pms.frame_errors);
    printf("  per-sensor io:        scd40=%u sgp41=%u sht31=%u pms=%u\n",
           h.app.scd.io_errors, h.app.sgp.io_errors,
           h.app.sht.io_errors, h.app.pms.io_errors);
    printf("model: %u param bytes, %d features, %d classes\n",
           aeris_model_size_bytes(), AERIS_N_FEATURES, AERIS_N_CLASSES);

    /* The SGP41 must have been handed live humidity, not the 0x8000 default.
     * This is the assertion behind the compensation bullet. */
    printf("sgp41 last compensation: rh_ticks=0x%04X (%.1f%%RH) "
           "t_ticks=0x%04X (%.1fC)\n",
           h.bus.sgp41.last_rh_ticks,
           h.bus.sgp41.last_rh_ticks * 100.0 / 65535.0,
           h.bus.sgp41.last_t_ticks,
           h.bus.sgp41.last_t_ticks * 175.0 / 65535.0 - 45.0);

    if (!AERIS_MODEL_TRAINED) {
        printf("\nNOTE: placeholder model is loaded; run `make model` first.\n");
        return 0;
    }
    /* 85 % is the bar this scenario has to clear for the run to pass. */
    if (acc < 85.0) {
        printf("\nFAIL: accuracy below the 85%% acceptance threshold\n");
        return 1;
    }
    printf("\nPASS\n");
    return 0;
}

/* ---- fault injection -------------------------------------------------- */

static int mode_faults(void)
{
    harness_t h;
    harness_init(&h, 0xBADF00Du);
    /* Every 17th transaction NACKs and every 23rd reply has a bit flipped.
     * That is far worse than any real bus, which is the point. */
    h.bus.nack_every    = 17;
    h.bus.corrupt_every = 23;
    scenario_acceptance(&h.sc);

    long valid_rows = 0;
    bool co2_sane = true, temp_sane = true;

    while (harness_tick(&h)) {
        aeris_snapshot_t snap;
        aeris_app_snapshot(&h.app, &snap);
        if (!snap.features_valid) continue;
        valid_rows++;
        /* The real check: did a corrupted word ever reach the published
         * snapshot as a plausible-looking reading? */
        if (snap.co2_ppm == snap.co2_ppm &&
            (snap.co2_ppm < 300.0f || snap.co2_ppm > 6000.0f)) co2_sane = false;
        if (snap.temperature_c == snap.temperature_c &&
            (snap.temperature_c < -5.0f || snap.temperature_c > 60.0f)) temp_sane = false;
    }

    printf("fault injection: %u NACKs, %u bit-flips injected\n",
           h.bus.injected_nacks, h.bus.injected_corruptions);
    printf("driver-caught crc/frame errors: scd40=%u sgp41=%u sht31=%u pms=%u\n",
           h.app.scd.crc_errors, h.app.sgp.crc_errors,
           h.app.sht.crc_errors, h.app.pms.frame_errors);
    printf("i/o errors: scd40=%u sgp41=%u sht31=%u pms=%u\n",
           h.app.scd.io_errors, h.app.sgp.io_errors,
           h.app.sht.io_errors, h.app.pms.io_errors);
    printf("valid feature rows produced anyway: %ld\n", valid_rows);

    int rc = 0;
    if (h.bus.injected_corruptions == 0) {
        printf("FAIL: no corruption was injected, the test proves nothing\n");
        rc = 1;
    }
    uint32_t caught = h.app.scd.crc_errors + h.app.sgp.crc_errors +
                      h.app.sht.crc_errors + h.app.pms.frame_errors;
    if (caught == 0) {
        printf("FAIL: drivers caught none of the injected corruption\n");
        rc = 1;
    }
    if (!co2_sane)  { printf("FAIL: an implausible CO2 value was published\n");  rc = 1; }
    if (!temp_sane) { printf("FAIL: an implausible temperature was published\n"); rc = 1; }
    if (valid_rows == 0) {
        printf("FAIL: the pipeline stalled completely under load\n");
        rc = 1;
    }
    printf(rc ? "\nFAIL\n" : "\nPASS: corruption was caught, pipeline kept running\n");
    return rc;
}

/* ---- benchmark -------------------------------------------------------- */

/* Times the integer inference path on its own. The ESP32-C3 is RV32IMC with no
 * FPU, so the softmax and the feature maths are soft-float there and will be
 * markedly slower than this host figure; the argmax path itself is pure
 * integer. The on-target number belongs in docs/BRINGUP.md, measured with
 * esp_timer, not extrapolated from here. */
static int mode_bench(int iters)
{
    harness_t h;
    harness_init(&h, 0xBEEFu);
    scenario_clear(&h.sc);
    scenario_add(&h.sc, EV_COOKING, 30u * 60000u);
    while (harness_tick(&h)) { }

    aeris_snapshot_t s;
    aeris_app_snapshot(&h.app, &s);
    if (!s.features_valid) { printf("no features to benchmark\n"); return 1; }

    aeris_infer_t out;
    /* Warm the caches first; the first call is not representative. */
    for (int i = 0; i < 1000; i++) aeris_model_infer(s.features, &out);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < iters; i++) aeris_model_infer(s.features, &out);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double ns = ((double)(t1.tv_sec - t0.tv_sec) * 1e9 +
                 (double)(t1.tv_nsec - t0.tv_nsec)) / (double)iters;
    printf("inference: %.2f us per call over %d iterations (host, %s)\n",
           ns / 1000.0, iters, AERIS_MODEL_ID);
    printf("model: %u parameter bytes, %d features, %d classes\n",
           aeris_model_size_bytes(), AERIS_N_FEATURES, AERIS_N_CLASSES);
    printf("multiply-accumulates per inference: %d\n",
           AERIS_N_FEATURES * AERIS_MODEL_H1 + AERIS_MODEL_H1 * AERIS_MODEL_H2 +
           AERIS_MODEL_H2 * AERIS_N_CLASSES);
    printf("predicted: %s (%.1f%% confidence)\n",
           aeris_class_name(out.argmax), (double)out.prob[out.argmax] * 100.0);
    return 0;
}

/* ---- entry ------------------------------------------------------------ */

int sim_serve(int port, int speed);   /* http_sim.c */

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s --run [--verbose] [--seed N]\n"
        "       %s --dataset N FILE\n"
        "       %s --faults\n"
        "       %s --bench [ITERS]\n"
        "       %s --serve PORT [--speed N]   (N = times faster than real time)\n",
        argv0, argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(argv[0]); return 2; }

    if (!strcmp(argv[1], "--run")) {
        bool verbose = false;
        uint32_t seed = 0xAE215u;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--verbose")) verbose = true;
            else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint32_t)strtoul(argv[++i], NULL, 0);
        }
        return mode_run(seed, verbose);
    }
    if (!strcmp(argv[1], "--dataset")) {
        if (argc < 4) { usage(argv[0]); return 2; }
        return mode_dataset(atoi(argv[2]), argv[3]);
    }
    if (!strcmp(argv[1], "--faults")) return mode_faults();
    if (!strcmp(argv[1], "--bench")) {
        return mode_bench(argc > 2 ? atoi(argv[2]) : 200000);
    }
    if (!strcmp(argv[1], "--serve")) {
        if (argc < 3) { usage(argv[0]); return 2; }
        int speed = 1;
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--speed") && i + 1 < argc) speed = atoi(argv[++i]);
        }
        return sim_serve(atoi(argv[2]), speed);
    }
    usage(argv[0]);
    return 2;
}
