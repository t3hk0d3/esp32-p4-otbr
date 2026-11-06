# Chip temperature: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

## Context

The ESP32-P4 has an on-die temperature sensor (`SOC_TEMP_SENSOR_SUPPORTED`, driver `esp_driver_tsens`,
`driver/temperature_sensor.h`). Nothing reads it today. The board sits in a closed case with PoE power, so the die
temperature is a useful health signal.

**Goal:** the chip temperature shows up in `GET /device/status`, on the Status page (the dashboard) and in
`GET /metrics`.

**What it is:** the die temperature, not the room temperature. It reads above ambient and rises with load. The docs say
so.

**Architecture:** `diag` owns the sensor. Its sampler task already reads the heap once a second; it reads the
temperature in the same tick and keeps the last value under its mutex. Two readers copy it without touching the
driver:
- `device_api` asks `diag_temperature()` while it builds `/device/status`;
- `metrics` gets it in `diag_metrics_view_t`, which `scrape.c` already copies.

The dashboard reads it from the `/device/status` poll it already makes.

**Tech stack:** ESP-IDF v6.1, Unity host tests on the `linux` target, node page tests, pytest-embedded.

**Styleguide:** `STYLEGUIDE.md`:
- astyle via `make lint`, `snake_case`, the `diag_` / `device_api_` / `metrics_` prefixes, `esp_err_t` +
  `ESP_RETURN_ON_ERROR`;
- host tests in `<component>/test_host/main/`, named `"<scenario>: <effect>"`, tagged `[device_api]` / `[metrics]`;
- `status.js` stays ES5, page tests in `tools/tests/test_status_page.py`;
- the Status page is in the vendored `esp_ot_br_server` frontend: list the change in `p4/components/PATCHES.md` if the
  page files are listed there.

## Decisions

1. **Range 20..100 °C** (P4 table: ±2 °C). The most accurate range, −10..80 °C (±1 °C), clamps exactly where
   overheating starts. The die sits well above 20 °C indoors.
2. **Sensor failure is not fatal.** If install or enable fails, `diag_start` logs one warning and carries on; the
   temperature stays unavailable. A failed read makes the value unavailable until the next good read.
3. **One decimal.** JSON and the exposition format both write one decimal, rounded half away from zero. Neither writer
   has floats today, so each gains a small signed "tenths" formatter.
4. **Names:** JSON `chip_temperature_c`, metric `br_chip_temperature_celsius` (gauge, no labels). "chip" says what is
   measured.
5. **Unavailable means absent, not zero:** `null` in JSON, no series in `/metrics`, `-` on the page. It does not set
   `br_scrape_partial`: the sensor is not a source that can be busy.
6. **Not in `/device/diag`.** Not asked for; easy to add later from the same value.

## Contract

- `diag.h`: `esp_err_t diag_temperature(float *out_c)`. ESP_OK with the last reading; ESP_ERR_NOT_FOUND when there is
  none (sensor unavailable, last read failed, or no sample yet); ESP_ERR_INVALID_STATE before `diag_start`;
  ESP_ERR_TIMEOUT when the mutex is not free in 100 ms; ESP_ERR_INVALID_ARG for NULL. Never calls the driver.
- `diag_metrics_view_t` gains `bool chip_temperature_valid; float chip_temperature_c;`.
- `device_api_status_t` gains `bool chip_temperature_known; float chip_temperature_c;`.
- `/device/status` gains top-level `"chip_temperature_c": 47.3 | null`, after `storage_ok`.

## BDD skeleton

```
diag sampler (sampler.c, checked on the board: the sampler has no host seam)
    diag_start
        On a working sensor
            - installs the sensor with range 20..100 and enables it
        On a sensor that cannot be installed or enabled
            - logs one warning
            - still starts the sampler (returns ESP_OK)
    sample tick
        On a good read
            - stores the reading as the current value
        On a failed read
            - marks the value unavailable
    diag_temperature
        On a stored reading
            - returns ESP_OK and the reading
        On no reading
            - returns ESP_ERR_NOT_FOUND
        Before diag_start
            - returns ESP_ERR_INVALID_STATE
        On NULL
            - returns ESP_ERR_INVALID_ARG
    diag_metrics_view
        On a stored reading
            - sets chip_temperature_valid and chip_temperature_c
        On no reading
            - leaves chip_temperature_valid false

device_api_status_json (status_json.c, host-tested)
    On a known temperature
        - writes chip_temperature_c with one decimal (47.25 -> 47.3)
    On a temperature below zero
        - writes the sign once (-0.5 -> -0.5, -3.04 -> -3.0)
    On an unknown temperature
        - writes chip_temperature_c: null

device_api status handler (device_api.c, checked on the board)
    On diag_temperature ESP_OK
        - sets chip_temperature_known and the value
    On any error
        - leaves chip_temperature_known false (the status still answers 200)

metrics render (render.c, host-tested)
    On a valid temperature
        - writes br_chip_temperature_celsius with HELP and TYPE gauge, one decimal
        - places the family after the heap families, in the catalogue's order
    On an invalid temperature
        - writes no br_chip_temperature_celsius family
        - does not set br_scrape_partial
    On no diag view
        - writes no br_chip_temperature_celsius family

Status page (status.js, page-tested)
    render
        On a number
            - shows "47.3 °C" in t-chip-temp
        On null or a missing field
            - shows "-"
        On the status poll failing
            - shows "-" (with the other status fields)
```

## Tasks

### Task 1: diag reads the sensor

- [ ] `p4/components/diag/CMakeLists.txt`: add `esp_driver_tsens` to the target build's `PRIV_REQUIRES`.
- [ ] `sampler.c`: install and enable in `diag_start` (Decision 2); read in `sample_once` next to the heap reads and
  store under `s_mutex`; implement `diag_temperature`; fill the two new `diag_metrics_view_t` fields.
- [ ] `diag.h`: declare `diag_temperature` and the new view fields, with doc comments in the file's style.
- [ ] `make test-host` (the diag host tests must still build), `make lint`.

### Task 2: `/device/status`

- [ ] Host tests first (`test_status.c`), from the skeleton.
- [ ] `device_api_priv.h`: the two status fields. `status_json.c`: a static signed one-decimal writer and the field.
  `device_api.c`: fill the fields from `diag_temperature` where `storage_ok` is filled.
- [ ] `make test-host`, `make lint`.

### Task 3: `/metrics`

- [ ] Host tests first (`test_render.c`; `sample_input.c` sets a reading), from the skeleton. Update the catalogue
  the order test checks, and `tools/tests/test_metrics_format.py` if it lists families.
- [ ] `writer.c/.h`: `metrics_writer_tenths(metrics_writer_t *w, int32_t tenths)`. `render.c`: the family in
  `render_diag`, after the heap families.
- [ ] `make test-host`, `make test-tools`, `make lint`.

### Task 4: the Status page

- [ ] Page tests first (`tools/tests/test_status_page.py`), from the skeleton.
- [ ] `index.html`: a "Chip temperature" row (`t-chip-temp`) in the "Load and memory" card, after PSRAM. `status.js`:
  fill it in `render(s)`, clear it with the other status fields.
- [ ] `PATCHES.md` entry if needed. `make test-tools`, `make lint`.

### Task 5: Docs

- [ ] `docs/api.md`: `chip_temperature_c` in the `/device/status` row (die temperature, one decimal, `null` when
  unavailable).
- [ ] `docs/metrics.md`: the family in the table; an example alert `br_chip_temperature_celsius > 85` for 10 m.
- [ ] `CHANGELOG.md` under Unreleased.

### Task 6: review, then the on-board check

- [ ] Code review of the whole branch; fix findings.
- [ ] Target tests: `p4/pytest/test_br_boot.py` asserts `chip_temperature_c` is a number in 20..100;
  `p4/pytest/test_metrics.py` asserts the series is present. These run in the batched hardware pass, not inside a
  task.

## Verification

```bash
make test-host
```

```bash
make test-tools
```

```bash
make lint
```

On the board, in the batched hardware pass: the Status page shows a plausible value (30–70 °C at idle), `/metrics` has
`br_chip_temperature_celsius`, and the value rises under load (for example during a topology scan).
