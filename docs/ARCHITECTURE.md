# Architecture

## The shape of it

```
                    ┌──────────────────────────────────────────┐
                    │              core/  (portable C11)       │
   ESP32-C3         │                                          │        host
   ────────         │  drivers    scd40  sgp41  sht31  pmsa003i│      ────────
  i2c_esp.c  ──────▶│     │                                    │◀──── virtual_i2c.c
  (IDF driver)      │     ▼                                    │      (sensor models)
                    │  app.c  ── sampling state machine        │            ▲
  status_led.c ◀────│     │                                    │            │
  (LEDC, own task)  │     ├─▶ gas_index.c   raw ticks → index  │       room_model.c
                    │     ├─▶ features.c    rolling window     │       (CO₂ mass
  web_server.c ◀────│     ├─▶ model.c       int8 MLP           │        balance,
  (esp_http_server) │     ├─▶ classifier.c  hysteresis         │        VOC/PM/RH)
                    │     ├─▶ aqi.c         0-100 score        │            ▲
  net.c             │     └─▶ history.c     90 min ring        │            │
  (WiFi, mDNS)      │                                          │       scenario.c
                    │  api.c  ── JSON bodies ──┐               │      (event timeline)
                    └──────────────────────────┼───────────────┘
                                               │
                                     identical bytes both sides
```

`core/` has no dependency on the ESP-IDF, FreeRTOS, lwIP or libc beyond
`string.h`, `math.h` and `stdio.h`'s `snprintf`. It allocates nothing. That is
what lets the host test suite and the simulator compile and run the files that
actually ship, rather than a model of them.

## The seam

Exactly one interface separates portable code from the platform:

```c
struct aeris_i2c {
    aeris_err_t (*write)(void *ctx, uint8_t addr, const uint8_t *buf, size_t len);
    aeris_err_t (*read )(void *ctx, uint8_t addr, uint8_t *buf, size_t len);
    void        (*delay_ms)(void *ctx, uint32_t ms);
    uint32_t    (*now_ms)(void *ctx);
    void *ctx;
};
```

On the device it is backed by `i2c_master_transmit`/`i2c_master_receive` and
`esp_timer`. On the host it is backed by device models that decode the real
opcodes, answer with real CRC-8s, and NACK a read issued before the datasheet's
execution time has elapsed. A driver that forgets a delay or mis-frames an
argument fails on the host in the same place it would fail on the bench.

## Tasks and the snapshot

Three tasks, one shared structure:

| Task | Prio | Stack | Role |
|------|------|-------|------|
| `sensor` | 5 | 4 kB | Owns the bus. The only caller of `aeris_app_step()`. |
| `httpd` | 4 | 6 kB | Serves the dashboard and the API. |
| `led` | 3 | 3 kB | Drives three LEDC channels. |

Only `sensor` touches a driver, so there is no bus arbitration. Everything else
calls `aeris_app_snapshot()`, which copies a plain-values struct under one
mutex. No pointers escape, so a reader can take as long as it likes to
serialise without blocking the next sample.

**This is the fix for the original problem.** The first cut of this firmware was
a single loop with two `delay(5000)` calls in it, and those made the web server
unreachable for ten seconds out of every cycle. The sampling task still blocks —
15 ms for the SHT31, 50 ms for the SGP41, 500 ms for an SCD40 stop, 10 s for a
self-test — but now it blocks only itself.

## The sampling cycle

`aeris_app_step()` is a state machine that returns how long the caller should
wait before calling it again, so the cadence is the core's decision and not the
task's:

| Phase | Cadence | What happens |
|-------|---------|--------------|
| `boot` | 10 ms | — |
| `probe` | 1 s | Read serials, apply calibration, start periodic measurement |
| `conditioning` | 1 s | The SGP41's mandatory 10 s burn-in |
| `warmup` | 5 s | Sampling, but the 6-minute feature window is not full yet |
| `run` | 5 s | Full pipeline |
| `fault` | 5 s | Nothing answered; retry probe so a reseated connector recovers |

Within a `run` cycle the order is deliberate:

1. **SHT31** — temperature and humidity first,
2. **SGP41** — immediately after, so it gets the *freshest* RH/T as its
   compensation arguments,
3. **SCD40** — poll `get_data_ready_status`, read only if there is a new sample,
4. **PMSA003I** — read the pushed frame.

Step 2 following step 1 is the whole point of the compensation work: the SGP41's
sensitivity is a strong function of humidity, and handing it a reading from five
seconds ago on a day when a window just opened is measurably worse than handing
it the one taken 15 ms ago.

## Degraded operation

A sensor that stops answering must not take the device down.

- **No SHT31** → the SGP41 gets the datasheet defaults instead of live values,
  and T/RH fall back to the SCD40's own (worse, because its die is warm, but
  present). The device keeps classifying.
- **No SCD40** → there is no CO₂, so there is no honest feature vector.
  `features_valid` goes false, the API says so, and the dashboard stops claiming
  to know what is happening rather than inventing an answer.
- **No PMSA003I** → PM2.5 reads as 0 and cooking is detected on VOC and CO₂
  alone, with lower confidence.
- **Nothing at all** → the `fault` phase, a double-blink red LED, and a probe
  retry every five seconds so reseating a connector recovers without a reboot.

Every one of these is a test in `tests/test_integration.c`.

## Memory

| | |
|---|---|
| Feature window | 72 slots × 44 B = 3.2 kB |
| History ring | 180 records × 12 B = 2.1 kB |
| Model parameters | 708 B (flash) |
| HTTP response buffer | 12 kB, one, heap-allocated at startup |
| Dashboard | ~12 kB in flash |

Nothing in `core/` calls `malloc`. The only heap allocation in the firmware is
the HTTP response buffer, done once at startup, so there is no fragmentation
path at all.
