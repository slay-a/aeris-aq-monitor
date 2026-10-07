# Sensor protocols

Everything here is implemented from the datasheets in
[`core/src/`](../core/src), with no vendor libraries. This file records the
details that are easy to get wrong, because each of them produced a bug at some
point and each is now covered by a test.

## Sensirion parts (SCD40, SGP41, SHT31)

### Framing

A command is a **16-bit big-endian opcode**. Commands with arguments append one
or more triplets:

```
[cmd_msb] [cmd_lsb] [arg_msb] [arg_lsb] [crc8] ...
```

The CRC covers **only the two argument bytes**, not the opcode. Replies are
triplets too: `[msb] [lsb] [crc8]` per 16-bit word.

### CRC-8

Polynomial `0x31`, init `0xFF`, no reflection, no final XOR. The published test
vector is `0xBEEF → 0x92`, which `tests/test_crc8.c` pins; that one vector is
enough to catch a wrong polynomial, a wrong init value and a reflected
implementation all at once.

The suite also flips every single bit of a three-word reply — 48 flips — and
requires all 48 to be caught. Skipping the CRC is not an optimisation worth
making: a single flipped bit in the CO₂ word decodes as a perfectly plausible
reading a few hundred ppm away from the truth, and nothing downstream can tell.

### Timing: the part that bites

These parts **do not clock-stretch** in the modes used here. A read issued
before the command's execution time has elapsed gets a NACK — not a wait. So the
delay is the driver's responsibility, and it has to be the datasheet's number:

| Command | Delay |
|---------|-------|
| Most setup commands | 1 ms |
| `read_measurement`, `get_data_ready_status` | 1 ms |
| SHT31 high-repeatability measurement | 15 ms |
| SGP41 `measure_raw_signals`, `execute_conditioning` | 50 ms |
| SGP41 `execute_self_test` | 320 ms |
| SCD40 `stop_periodic_measurement` | **500 ms** |
| SCD40 `perform_forced_recalibration` | 400 ms |
| SCD40 `persist_settings` | 800 ms |
| SCD40 `perform_self_test` | 10 s |

The 500 ms stop is the one that causes trouble: the SCD40 ignores bus traffic
until it completes, so a setup command sent 50 ms later is silently dropped and
the sensor keeps its old configuration with no error anywhere. The virtual
sensor in `sim/src/virtual_i2c.c` enforces every one of these windows, so
removing a delay fails a test rather than producing a subtly mis-configured
device.

### SCD40 specifics

Conversions (datasheet §3.5.2):

```
CO₂[ppm] = raw                              (already in ppm)
T[°C]    = -45 + 175 · raw / 65535
RH[%]    =       100 · raw / 65535
```

The temperature **offset** uses a different divisor — `2¹⁶`, not `2¹⁶−1` —
because it is a span rather than an absolute:

```
offset_ticks = offset[°C] · 65536 / 175
```

Other traps:

- `get_serial_number` and the setup commands are **only valid while idle**.
  `scd40_probe()` issues a stop first, so a warm reset mid-measurement still
  lands in a known state.
- `get_data_ready_status`: only the **low 11 bits** carry the flag. The high
  bits are not zero, so testing the whole word reports "ready" forever.
- Ambient pressure is sent in **hectopascals**, so the whole range fits a 16-bit
  word. Sending pascals asks for a pressure 100× too high and the sensor clamps.
- A CO₂ word of `0` is CRC-valid and means *not calibrated yet*. The driver
  rejects it with `AERIS_ERR_RANGE` rather than letting a zero into the feature
  window.
- Forced recalibration returns `0xFFFF` when it refuses, and otherwise
  `correction + 0x8000`.

### SGP41 specifics

The SGP41 returns **raw resistance ticks, not an index**, and its sensitivity
depends strongly on humidity. Every `measure_raw_signals` therefore carries the
current RH and T as two argument words:

```
rh_ticks = RH[%]      · 65535 / 100
t_ticks  = (T[°C]+45) · 65535 / 175
```

The datasheet defaults are `0x8000` (50 %RH) and `0x6666` (25 °C), and those
double as the test vectors for the encoders. The driver substitutes them when
its inputs are NaN — which is what "the SHT31 has not answered yet" looks like
— rather than sending zeros, since zero ticks means bone-dry air and the sensor
would over-correct hard in the wrong direction.

Startup needs 10 s of `execute_conditioning` at 1 Hz before the first
measurement, during which only the VOC channel is meaningful.

`tests/test_sgp41.c` measures the benefit rather than asserting it: it sweeps the
room from 30 %RH to 75 %RH with the air chemistry held constant and compares the
spread of raw ticks when compensating correctly against the spread when sending
the defaults. Correct compensation has to hold the signal at least four times
steadier for the test to pass.

### SHT31 specifics

Single-shot, high repeatability, **clock stretching disabled** (`0x2400`). The
no-stretch variant is deliberate: the SGP41 shares this bus, and holding SCL for
15 ms would stall its own timing-sensitive reads. The driver waits the 15 ms
itself.

## PMSA003I

Not command/response. The part pushes a fixed **32-byte frame** that you read
whenever you like:

```
offset  0  1   0x42 0x4D              magic
offset  2  3   0x00 0x1C              frame length (28)
offset  4 .. 27                       13 big-endian uint16 data words
offset 30 31   checksum               16-bit sum of bytes 0..29
```

All three — magic, length and checksum — are validated. A short read on a shared
bus otherwise shifts the whole frame and decodes as a PM2.5 spike, which is
exactly the kind of artefact that would teach the classifier to see cooking
where there is none. `tests/test_pmsa003i.c` flips all 208 bits of the body and
requires every one to be caught.

The checksum is a plain additive sum, not a CRC, so it cannot catch a
transposition. That is a limitation of the part, not of the driver.

**A note on the particle-count fields:** during heavy frying the `>0.3 µm` count
genuinely exceeds 65535. The simulator originally cast those floats straight to
`uint16_t`, which is undefined behaviour for out-of-range values, and the result
was frames whose checksum did not match the bytes the compiler had actually
stored — about 16 % of reads failed in a clean run. Saturating conversion fixed
it, and `tests/test_pmsa003i.c` holds that line with a 900 µg/m³ regression case.
