# The classifier

## What it is for

CO₂, VOC and particulate numbers on a screen tell you *that* the air changed.
They do not tell you *why*, and the why is what determines what you should do:
1100 ppm of CO₂ with flat VOC and no particulates means open a window; 1100 ppm
with VOC at 300 and PM2.5 at 90 means turn the extractor on. The classifier
turns four sensor streams into one of five answers.

| Class | What it looks like |
|-------|--------------------|
| `baseline` | Everything flat or decaying toward outdoor |
| `occupancy` | CO₂ rising steadily, mild VOC, no particulates, absolute humidity drifting up |
| `cooking` | PM2.5 and VOC up together, real heat and moisture, NOx too on a gas hob |
| `ventilation` | CO₂ and VOC falling together while absolute humidity and temperature move toward outdoor |
| `volatile` | Large VOC with no particulates, no CO₂ and no heat — a solvent or aerosol |

The separations are physical, not statistical. An electric hob adds VOC and
particulates but no CO₂ at all. A solvent adds VOC and nothing else. Only
ventilation lowers absolute humidity while raising relative humidity.

## Features

Fourteen, computed by `core/src/features.c` over a 6-minute rolling window at
the 5 s sampling cadence:

| # | Feature | Why |
|---|---------|-----|
| 0 | `co2_ppm` | Level |
| 1 | `co2_slope_1min` | Something just changed |
| 2 | `co2_slope_5min` | A trend, not a gust |
| 3 | `co2_sd_1min` | A steady source wobbles; a draught does not |
| 4 | `voc_index` | Level |
| 5 | `voc_slope_1min` | Onset |
| 6 | `voc_slope_5min` | Trend |
| 7 | `nox_index` | Combustion marker — gas hob versus electric |
| 8 | `temp_c` | Level |
| 9 | `temp_slope_5min` | Cooking warms, ventilation cools |
| 10 | `rh_pct` | Level |
| 11 | `abs_humidity` | See below |
| 12 | `ah_slope_5min` | The ventilation signature |
| 13 | `pm2_5` | 1-minute mean; cooking's clearest marker |

**Absolute humidity earns its place.** Relative humidity rises when a window
opens on a cold day, because the incoming air cools the room faster than it
dries it — so RH alone makes ventilation look like occupancy. Absolute humidity,
computed from temperature and RH with the Magnus formula, falls instead, because
cold outdoor air genuinely holds less water. Fusing the two sensors into one
physical quantity is what separates those classes cleanly.

## Where the training data comes from

**It is simulated, and that is the honest limitation of the current model.**

`make dataset` runs the C simulator, which drives a room physics model —
a stirred-tank CO₂ mass balance, first-order VOC/NOx/PM sources with ventilation
and deposition, lumped thermal and moisture capacitance for the structure —
through randomised event timelines. The virtual sensors apply their own transfer
functions, noise and quantization, and the **firmware's own** gas-index,
windowing and feature code produces the rows. So there is no train/serve skew:
the features in the CSV are literally what the device computes.

What that buys: 186,000 labelled rows in under a second, exact labels, and
reproducibility from a seed.

What it does not buy: **realism**. A physics model does not contain the smells
of a particular kitchen, a radiator cycling, a cat, or an SGP41's own ageing.
The 98 % below is accuracy against a simulator, not against a room, and the
real figure will be lower. `docs/BRINGUP.md` step 10 is the plan for collecting
labelled data from the actual device via `/api/history.csv`, and this file will
carry both numbers once that exists.

Two things make the simulated number less hollow than it could be:

- **The split is grouped by session.** Rows inside one session are heavily
  autocorrelated — a 5-minute slope feature overlaps 60 consecutive samples — so
  a random row split leaks the test set into training. Whole sessions go to one
  side or the other; the figures below are from 15 sessions the model never saw.
- **The first 2.5 minutes after every event change are discarded.** When cooking
  starts, the room takes minutes to respond and the slope features longer still,
  so those rows are genuinely unlabelable. Training on them produces a model that
  looks good offline and is useless in the room.

## The model

```
14 features → 16 (ReLU) → 16 (ReLU) → 5 logits
```

708 bytes of parameters. Not TensorFlow Lite Micro: the interpreter, its op
resolver and its tensor arena cost more flash and RAM than the entire rest of
the firmware, to run an operator set that is three matrix multiplies. Writing
the kernel out by hand also means the quantization scheme is mine end to end,
which is what makes the parity test below possible.

### Quantization

Symmetric per-tensor int8 weights, int32 accumulate, per-layer
`(multiplier, shift)` requantization, round-half-away-from-zero.

- Inputs are standardised with the training split's mean and σ, then quantized
  at a fixed scale of 0.0625 standardised units per step — ±7.9σ across the
  int8 range.
- Hidden activation scales are calibrated at the **99.9th percentile** of the
  training set's activations, not the maximum, so a handful of outliers cannot
  set the scale and crush the resolution of everything ordinary.
- The output layer's int32 accumulators are dequantized directly, so the softmax
  — and therefore the confidence shown on the dashboard — is on a real scale.

### Parity

`ml/quant.py` reimplements `core/src/model.c`'s integer arithmetic exactly: the
same accumulators, the same 16-bit multiplier shift, the same rounding shift,
the same saturation. The trainer evaluates the **integer** network with it, so
the accuracy reported below is the device's accuracy and not the float model's.

It then emits 255 held-out samples with their int8 inputs, int32 logits and
argmax into `tests/vectors_model.h`, and `tests/test_model.c` replays them
through the C and requires an exact match. If the two ever disagree by one
accumulator bit, the build fails.

## Results

Trained on 136,513 rows from 45 sessions, tested on 49,923 rows from 15
**held-out** sessions.

| | Accuracy | Macro F1 |
|---|---|---|
| float32 | 98.28 % | 0.978 |
| **int8 (what ships)** | **98.25 %** | **0.978** |

Quantization costs 0.03 points. float/int8 prediction agreement is 99.4 %.

| Class | Precision | Recall | F1 | Support |
|-------|-----------|--------|-----|---------|
| baseline | 0.962 | 0.981 | 0.972 | 10,416 |
| occupancy | 0.985 | 0.975 | 0.980 | 20,970 |
| cooking | 1.000 | 1.000 | 1.000 | 9,202 |
| ventilation | 1.000 | 1.000 | 1.000 | 6,889 |
| volatile | 0.936 | 0.944 | 0.940 | 2,446 |

Cooking and ventilation are perfectly separated because their signatures are
unambiguous in this model — particulates for one, falling absolute humidity for
the other. The remaining confusion is `volatile` against `occupancy`, which is
the right thing to find hard: a mild solvent event and a person in the room both
produce a modest VOC rise, and the only thing distinguishing them is whether CO₂
follows. In a real room that boundary will be considerably worse.

Regenerate all of this with `make model`; the full report is written to
`ml/data/report.json`.

A note on reproducibility: these numbers come from the committed model, trained
on macOS. The same pipeline on Linux lands at 98.23 % instead of 98.25 %,
because the dataset is produced by C float code and `expf`/`logf` differ between
libm implementations, so the weights differ by a few least-significant bits. CI
therefore requires a from-scratch retrain to clear an accuracy floor and to stay
bit-exact against the C kernel, rather than requiring byte-identical weights.

## From a prediction to a label on screen

Raw argmax at 0.2 Hz flickers. One borderline sample flips the dashboard from
"cooking" to "occupancy" and back, which reads as a broken device even when the
model is right on average. `core/src/classifier.c` adds two things:

1. An EMA over the probability vector (α = 0.25, a ~17 s time constant), so
   evidence has to persist to move the decision.
2. Hysteresis: a challenger must beat **the state currently on screen** by 0.12
   and hold that lead for 20 s before it takes over.

Confidence reported to the API is the smoothed probability of the state being
*displayed*, not of the instantaneous argmax — otherwise the number contradicts
the label during a transition.

`tests/test_classifier.c` pins the behaviour this exists for: alternating a
contrary sample with the true class for a minute must produce zero transitions,
while a genuinely sustained change must be accepted within two minutes.

## The air-quality score

Deliberately not the regulatory US AQI, which is an outdoor PM and ozone index
and says nothing about CO₂ — the number that indoors actually tracks how a room
feels. The score is the worst of four channels, each mapped 0–100 by
piecewise-linear interpolation:

- **CO₂**: 400 outdoor, 1000 the ASHRAE 62.1 comfort ceiling, 1400+ where
  measured cognitive-performance effects appear.
- **PM2.5**: 15 µg/m³ is the WHO 2021 24-hour guideline, 75 is interim target 1.
- **VOC / NOx index**: 100 is the sensor's own notion of clean air.

Worst-of rather than average: a room with perfect CO₂ and 80 µg/m³ of smoke is
not "mostly fine on average". The API reports which channel set the score.
