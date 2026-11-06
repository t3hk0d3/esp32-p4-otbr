# Chip temperature: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

## Context

The ESP32-P4 has an on-die temperature sensor (`SOC_TEMP_SENSOR_SUPPORTED`, driver `esp_driver_tsens`,
`driver/temperature_sensor.h`). Nothing reads it today. The board sits in a closed case with PoE power, so the die
temperature is a useful health signal.

**Goal:** the chip temperature shows up in `GET /device/diag`, on the Status page (the dashboard) and in
`GET /metrics`.

**What it is:** the die temperature, not the room temperature. It reads above ambient and rises with load. The docs say
so.

**Architecture:** `diag` owns the sensor and every reader of it. Its sampler task already reads the heap once a second;
it reads the temperature in the same tick and keeps the last value under its mutex, next to the heap figures. The two
existing copy paths carry it out, and neither touches the driver:
- `diag_snapshot_json` puts it in the `/device/diag` body;
- `diag_metrics_view` puts it in `diag_metrics_view_t`, which `metrics/scrape.c` already copies.

The dashboard's "Load and memory" card is already filled from `/device/diag` (`refreshDiag` in `status.js`), so the
new row updates and clears with CPU and memory. `device_api` and `/device/status` do not change.

**Tech stack:** ESP-IDF v6.1, Unity host tests on the `linux` target, node page tests, pytest-embedded.

**Styleguide:** `STYLEGUIDE.md`:
- astyle via `make lint`, `snake_case`, the `diag_` / `metrics_` prefixes, `esp_err_t` + `ESP_RETURN_ON_ERROR`;
- host tests in `<component>/test_host/main/`, named `"<scenario>: <effect>"`, tagged `[diag]` / `[metrics]`;
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
6. **`/device/diag`, not `/device/status`.** `/device/status` is configuration and state, read by Home Assistant;
   live health figures (load, heap, uptime) are in `/device/diag`. Keeping it there needs no new public function and
   no `device_api` change.

## Contract

- `diag_metrics_view_t` (`diag.h`) gains `bool chip_temperature_valid; float chip_temperature_c;`.
- `diag_snapshot_input_t` (`snapshot_json.h`) gains the same two fields.
- `/device/diag` gains top-level `"chip_temperature_c": 47.3 | null`, after `reset_reason`.

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
    diag_snapshot_json
        On a stored reading
            - passes chip_temperature_valid and the reading to the renderer
        On no reading
            - passes chip_temperature_valid false
    diag_metrics_view
        On a stored reading
            - sets chip_temperature_valid and chip_temperature_c
        On no reading
            - leaves chip_temperature_valid false

diag_snapshot_render (snapshot_json.c, host-tested)
    On a valid temperature
        - writes chip_temperature_c after reset_reason, with one decimal (47.25 -> 47.3)
    On a temperature below zero
        - writes the sign once (-0.5 -> -0.5, -3.04 -> -3.0)
    On an invalid temperature
        - writes chip_temperature_c: null

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
    refreshDiag
        On a number
            - shows "47.3 °C" in t-chip-temp
        On null or a missing field
            - shows "-"
        On the diag request failing
            - shows "-" (diagDashes, with the other diag fields)
```

## Tasks

### Task 1: diag reads the sensor and writes it to `/device/diag`

- [ ] Host tests first (`diag/test_host/main/test_snapshot_json.c`), from the skeleton.
- [ ] `p4/components/diag/CMakeLists.txt`: add `esp_driver_tsens` to the target build's `PRIV_REQUIRES`.
- [ ] `sampler.c`: install and enable in `diag_start` (Decision 2); read in `sample_once` next to the heap reads and
  store under `s_mutex`; copy into `diag_snapshot_input_t` and `diag_metrics_view_t`.
- [ ] `diag.h`, `snapshot_json.h`: the new fields, with doc comments in the files' style. `snapshot_json.c`: a static
  signed one-decimal writer and the field.
- [ ] `make test-host`, `make lint`.

### Task 2: `/metrics`

- [ ] Host tests first (`test_render.c`; `sample_input.c` sets a reading), from the skeleton. Update the catalogue
  the order test checks, and `tools/tests/test_metrics_format.py` if it lists families.
- [ ] `writer.c/.h`: `metrics_writer_tenths(metrics_writer_t *w, int32_t tenths)`. `render.c`: the family in
  `render_diag`, after the heap families.
- [ ] `make test-host`, `make test-tools`, `make lint`.

### Task 3: the Status page

- [ ] Page tests first (`tools/tests/test_status_page.py`), from the skeleton; the existing `refreshDiag` tests list
  the diag field ids and gain `t-chip-temp`.
- [ ] `index.html`: a "Chip temperature" row (`t-chip-temp`) in the "Load and memory" card, after PSRAM. `status.js`:
  fill it in `refreshDiag`, add it to `diagDashes`.
- [ ] `PATCHES.md` entry if needed. `make test-tools`, `make lint`.

### Task 4: Docs

- [ ] `docs/api.md`: chip temperature in the `/device/diag` row (die temperature, one decimal, `null` when
  unavailable).
- [ ] `docs/metrics.md`: the family in the table; an example alert `br_chip_temperature_celsius > 85` for 10 m.
- [ ] `docs/diagnostics.md` if it lists the `/device/diag` fields.
- [ ] `CHANGELOG.md` under Unreleased.

### Task 5: review, then the on-board check

- [ ] Code review of the whole branch; fix findings.
- [ ] Target tests: the existing `/device/diag` target test asserts `chip_temperature_c` is a number in 20..100;
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

On the board, in the batched hardware pass: the Status page shows a plausible value (30–70 °C at idle), `/device/diag`
and `/metrics` carry it, and the value rises under load (for example during a topology scan).
