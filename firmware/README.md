# Firmware (ESP32-C3)

The portable core in [`core/`](../core) is compiled directly into this app; the
files here are only the parts that need the IDF: the I2C backend, the HTTP
server, WiFi/mDNS, and the LED.

## Build

```bash
. $IDF_PATH/export.sh          # ESP-IDF v5.2 or newer
cd firmware
idf.py set-target esp32c3
idf.py menuconfig              # Aeris AQ Monitor -> WiFi, GPIOs, calibration
idf.py build flash monitor
```

With no SSID configured the device comes up as an open access point named
`aeris-XXXX`; join it and open <http://192.168.4.1/>. With credentials set it
joins your network and the dashboard is at <http://aeris.local/>.

## Task layout

| Task     | Priority | Stack  | Does |
|----------|----------|--------|------|
| `sensor` | 5        | 4 kB   | Owns the I2C bus; the only caller of `aeris_app_step()` |
| `httpd`  | 4        | 6 kB   | Serves the dashboard and the JSON API from snapshots |
| `led`    | 3        | 3 kB   | Polls snapshots, drives three LEDC channels |

Only `sensor` touches the drivers, so there is no bus arbitration to get wrong.
Everything else reads a snapshot copied under one mutex. The sampling task's
blocking waits — 15 ms for the SHT31, 50 ms for the SGP41, 500 ms for an SCD40
stop — therefore delay only itself, and the dashboard keeps answering through
all of them.

## Endpoints

| Path | Returns |
|------|---------|
| `/` | The dashboard (embedded in flash, ~12 kB) |
| `/api/live` | Current readings, event label, AQI, class probabilities |
| `/api/history` | 90 minutes at 30 s resolution, column-oriented |
| `/api/status` | Sensor presence, per-sensor error counters, model info |
| `/api/debug` | The raw feature vector, named |
| `/api/history.csv` | The same history as CSV, for pulling into `ml/` |

## A note on floating point

The ESP32-C3 is RV32IMC — no FPU. The feature maths and the softmax are
therefore soft-float. At a 5 s sample period that is irrelevant (the whole
pipeline is a fraction of a percent of one core), and the inference path that
produces the argmax is pure integer. It would matter at a 10 Hz sample rate; it
does not here.
