# Hardware

## Bill of materials

| Part | Role | Interface | Addr | Notes |
|------|------|-----------|------|-------|
| ESP32-C3-DevKitM-1 | MCU, WiFi | — | — | RV32IMC @160 MHz, 400 kB SRAM, 4 MB flash |
| Sensirion SCD40 | CO₂ | I²C | `0x62` | Photoacoustic NDIR, true CO₂, ±(50 ppm + 5 %) |
| Sensirion SGP41 | VOC / NOx | I²C | `0x59` | Metal-oxide, raw ticks only |
| Sensirion SHT31 | Temperature, RH | I²C | `0x44` | ±0.2 °C, ±2 %RH |
| Plantower PMSA003I | PM1.0 / 2.5 / 10 | I²C | `0x12` | Laser scattering, has a fan |

Everything is on one I²C bus, which is the reason this sensor set was chosen:
four parts, four wires, no SPI chip selects and no analogue front end.

## Wiring

```
ESP32-C3                      I²C bus (3.3 V)
--------                      ---------------
3V3  ─────────────┬──────┬──────┬──────┐
GND  ─────────┬───┼──┬───┼──┬───┼──┬───┼───┐
GPIO5 (SDA) ──┼───┼──┼───┼──┼───┼──┼───┤   │
GPIO6 (SCL) ──┼───┼──┼───┼──┼───┼──┤   │   │
              │   │  │   │  │   │  │   │   │
            SCD40  SGP41  SHT31   PMSA003I
             0x62   0x59   0x44     0x12

GPIO3  ── 220R ── LED red   ── GND
GPIO4  ── 220R ── LED green ── GND
GPIO10 ── 220R ── LED blue  ── GND
```

Pins are set in `idf.py menuconfig` → **Aeris AQ Monitor**, so none of this is
baked into the source.

### Pins to avoid on the ESP32-C3

- **GPIO2, GPIO8, GPIO9** are strapping pins. A pull-up from a breakout board on
  GPIO9 holds the chip in download mode and it will look bricked.
- **GPIO18, GPIO19** are USB D−/D+ on boards using the native USB-Serial-JTAG.
- **GPIO11** is `VDD_SPI` on some packages.

GPIO5/GPIO6 for I²C and GPIO3/4/10 for the LED avoid all of these.

## Pull-ups

Each Sensirion breakout usually carries its own 10 kΩ pull-ups. Four breakouts
in parallel is 2.5 kΩ, which is still fine at 100 kHz but is more current than
necessary. If you build a custom board, fit one pair of 4.7 kΩ pull-ups and
remove the rest. The firmware also enables the ESP32's internal pull-ups, which
are weak (~45 kΩ) and will *not* drive the bus on their own — they are a safety
net, not the design.

## Power

| Rail | Draw | Note |
|------|------|------|
| ESP32-C3 | 80–240 mA | peaks on WiFi transmit |
| SCD40 | 15 mA avg, 205 mA peak | peaks for ~10 ms during each measurement |
| SGP41 | 3 mA avg | the heater runs during each 50 ms measurement |
| SHT31 | 0.6 mA | negligible |
| PMSA003I | 100 mA | the fan runs continuously |

Budget **500 mA at 5 V**. The SCD40's 205 mA peak coinciding with a WiFi
transmit is the worst case, and on a thin USB cable it sags the rail enough to
brown out. The firmware enables the brown-out detector at its most sensitive
level so that failure is a clean reset with a logged reason rather than a hang.

A 100 µF bulk capacitor across the 3V3 rail, plus 100 nF next to each sensor,
is worth fitting. The PMSA003I in particular wants its own bulk capacitance
because the fan's inrush is on the same rail.

## Self-heating

The SCD40 reads temperature from its own die, next to a heated optical cavity,
and both it and the SHT31 sit in whatever air the ESP32's regulator has already
warmed. On the reference build the SCD40 reads **4.0 °C high** and the SHT31
about **0.9 °C high** once thermally settled, which takes roughly 20 minutes.

The firmware writes the SCD40's offset into the sensor so the part compensates
internally. The SHT31 is the authority for the temperature and humidity that
reach the dashboard and the SGP41's compensation inputs, because its sensing
element is further from the hot parts — but it is not immune, which is why
`docs/BRINGUP.md` step 6 measures both against a reference thermometer rather
than assuming the defaults.

If you are laying out a board: put the SHT31 and the SCD40 on a thermal island,
slot the ground plane between them and the regulator, and keep them upwind of
the PMSA003I's exhaust.
