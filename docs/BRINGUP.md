# Bring-up checklist

The hardware is not here yet. This is the sequence to run when it arrives — in
order, because each step depends on the one before it, and because every step
has a specific failure it is there to catch.

Tick these off in a PR against this file so the measured numbers end up in the
repository rather than in a notebook.

---

### 1. Power before anything else

Before connecting a single sensor, bring up the ESP32-C3 alone and confirm
3.3 V under load. Then add sensors **one at a time**, measuring the rail each
time. The PMSA003I's fan draws ~100 mA on inrush and the SCD40 peaks at 205 mA
for 10 ms during each measurement; a thin USB cable sags enough to brown out.

Expected: 3.3 V ± 0.1 V with all four sensors attached and WiFi associated.

If it sags, the cable is the first suspect, not the board.

### 2. Bus scan

Flash and watch the log. `aeris_i2c_esp_scan()` runs before anything else:

```
I (412) i2c: scanning bus...
I (430) i2c:   0x12 (PMSA003I)
I (436) i2c:   0x44 (SHT31)
I (442) i2c:   0x59 (SGP41)
I (448) i2c:   0x62 (SCD40)
```

Four addresses or stop here. Nothing at all usually means SDA and SCL are
swapped, or the pull-ups are missing — the ESP32's internal pull-ups are ~45 kΩ
and will not drive a four-device bus on their own.

### 3. Serial numbers and self-tests

The log should show the SCD40's 48-bit serial. Add a one-off call to
`scd40_self_test()` (10 s) and `sgp41_self_test()` (320 ms) and confirm both
return `AERIS_OK`. These exercise the parts' internal diagnostics and will catch
a reflow problem that a bus scan will not.

### 4. CRC error counters at zero

Let it run for an hour and check `/api/status`:

```json
"scd40": { "present": true, "crc_errors": 0, "io_errors": 0 }
```

**Any** non-zero CRC count on a short bench wire is a wiring problem, not noise:
weak pull-ups, a long unshielded run, or SDA and SCL crosstalking. Fix it here.
It will only get worse in an enclosure next to a switching regulator.

### 5. CO₂ against fresh air

Take the device outside, away from buildings and traffic, and let it run 10
minutes. It should read **415–430 ppm**.

If it is off by more than ~50 ppm, run a forced recalibration:
`scd40_forced_recalibration(&d, 420, &correction)` after three minutes of
periodic measurement outdoors. Record the correction value here.

Leave automatic self-calibration enabled for indoor use — it assumes the room
reaches outdoor CO₂ at some point in each week, which is true of most homes and
false of a sealed lab.

### 6. Temperature offset — measure it, do not assume it

The default in `menuconfig` is 4.00 °C, which is what the reference board
measured. **Yours will differ.** Layout, enclosure and airflow all change it.

1. Set `CONFIG_AERIS_SCD40_TEMP_OFFSET_CENTI` to 0 and reflash.
2. Let it run **20 minutes** — thermal settling is slow and measuring early is
   the commonest mistake here.
3. Compare `/api/live` against a reference thermometer in the same air.
4. Set the offset to the difference and reflash.
5. Confirm the SHT31 now reads within 0.3 °C of the reference.

Record both the SCD40 and SHT31 errors in this file.

### 7. Humidity sanity

Compare against any reference hygrometer. Within 3 %RH is fine. A salt-saturated
calibration jar (75 %RH over sodium chloride) is the better check if you have
one; give it four hours to equilibrate.

### 8. SGP41 conditioning and baseline

Watch the first ten minutes. The log should show `phase -> conditioning` then
`phase -> warmup` after 10 s. The VOC index starts at 100 and will wander for
the first hour or two while the baseline tracker converges — that is expected
and `gas_index_ready()` gates it.

Then do the real test: **boil a kettle, or open a bottle of isopropyl alcohol
across the room.** The VOC index should rise within 30 s and decay over several
minutes. If it does not move, the SGP41's heater is not running — check the
self-test and the supply.

### 9. Inference time on target

The host figure is 0.13 µs, which tells you nothing about an RV32IMC core with
no FPU. Measure it properly:

```c
int64_t t0 = esp_timer_get_time();
aeris_model_infer(features, &out);
int64_t t1 = esp_timer_get_time();
```

Record the median over 1000 calls. Then record it again with the softmax removed
(leaving only the argmax path), because the softmax is the soft-float part and
the integer path is what the "on-device inference" claim rests on.

**Write both numbers into `docs/ML.md` and into the résumé bullet.** Do not
quote the host figure.

Also record free heap after init (`esp_get_free_heap_size()`). Flash is already
known from CI: **846 kB** (0xd3940), leaving 45 % of the 1.5 MB app partition
free. Confirm `idf.py size` agrees on your toolchain version.

### 10. Collect real labelled data

This is the step that makes the model mean something. The current model is
trained entirely on simulated data (see `docs/ML.md`), and the 98 % it scores is
against a physics model, not a room.

1. Run the device in a real kitchen for a week.
2. Pull history regularly: `curl http://aeris.local/api/history.csv >> session.csv`
   — the ring only holds 90 minutes, so pull at least hourly, or add an SD card.
3. Keep a log of what actually happened and when. A note on your phone is
   enough: *18:40 started frying, 19:05 opened window, 19:30 done.*
4. Label the CSV from that log, with the same 2.5-minute settling hold-out
   `sim/src/scenario.c` applies.
5. Retrain on the real data, or on real plus simulated with the real rows
   weighted up, and report **both** numbers in `docs/ML.md`.

Expect the real accuracy to be materially lower, especially for
`volatile` versus `occupancy`. That is the honest result and it is more
interesting than the simulated one.

### 11. Soak test

Leave it running for a week. Then check `/api/status`:

- `crc_errors` should still be 0, or a handful at most.
- `sample_errors` should be a tiny fraction of `samples`.
- Uptime should equal the wall-clock time — any reset means a brown-out or a
  watchdog, and `esp_reset_reason()` will say which.
- Free heap should be identical to the value at boot. The firmware allocates
  only once, at startup, so **any** drift is a leak and worth finding.

---

## Results

Fill in when the hardware arrives.

| Step | Measured | Date | Notes |
|------|----------|------|-------|
| 1 Rail under load | | | |
| 2 Bus scan | | | |
| 3 Self-tests | | | |
| 4 CRC errors / hour | | | |
| 5 Outdoor CO₂ | | | |
| 6 SCD40 offset | | | |
| 6 SHT31 error | | | |
| 7 Humidity error | | | |
| 9 Inference (full) | | | |
| 9 Inference (argmax only) | | | |
| 9 Free heap after init | | | |
| 9 Flash used | 846 kB | 2026-10-07 | from CI, ESP-IDF v5.3 |
| 10 Real-data accuracy | | | |
| 11 Soak: uptime / resets | | | |
