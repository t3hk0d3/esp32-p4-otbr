# Prometheus `/metrics` endpoint: implementation plan

## Context

You approved the spec `docs/superpowers/specs/2026-11-02-prometheus-metrics-design.md` (branch `feat/metrics`). It adds `GET /metrics` in Prometheus text format 0.0.4, with every metric prefixed `br_`:

- device health from the existing diag sampler;
- Thread figures from a collector that takes the OpenThread lock for at most 10 ms every 10 s, so a scrape never takes the lock.

The aim is that Prometheus and Grafana can watch one board or many.

I started implementing before you approved the plan. This plan covers what already exists, so you can decide its fate:

- **Committed** `73f839a` (Task 1, diag totals). The diag host tests pass, 29 of 29.
- **Not committed:** `p4/components/metrics/` (part of Task 2: types, writer, renderer, build files, test helpers, writer and render tests). Never compiled, and written before its tests.

Execution: native, in this session. One whole-branch review at the end, done by a fresh reviewer on the most capable model.

**Styleguide:** `STYLEGUIDE.md`.

- Formatting through astyle (`make lint`); `snake_case`; component prefixes (new: `metrics_`).
- Errors as `esp_err_t` with `ESP_RETURN_ON_ERROR`.
- Host tests in `<component>/test_host/`, named `"<scenario>: <effect>"`.
- Target tests in `p4/pytest/test_<feature>.py`; routine tasks through the Makefile.

## Global constraints

- Metric names, types and labels follow spec section 4, minus the deviation below. `br_jumper_present` is already removed.
- Collector settings:
  - period 10 s (`METRICS_COLLECT_PERIOD_MS`);
  - lock timeout 10 ms (`METRICS_COLLECT_LOCK_MS`; one tick at `CONFIG_FREERTOS_HZ` 100);
  - data is fresh for 30 s (`METRICS_FRESH_MS`);
  - core 1, lowest application priority, 3 KB stack.
- A scrape never takes the OpenThread lock. It waits at most 100 ms for any source mutex.
- Content type `text/plain; version=0.0.4; charset=utf-8`. The body goes out in chunks of at most 2 KB.
- No floats: seconds are written as `<s>.<6 digits>`.
- No flashing. The on-board test only runs in the next batched hardware pass.
- Commits end with only `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

**Deviation from the spec (needs your OK):** drop `br_nat64_state`, `br_nat64_packets_total`, `br_nat64_bytes_total` and `br_nat64_errors_total`.

- `otNat64GetCounters` and `otNat64GetTranslatorState` are not in any linked library; I checked with `nm`. The NAT64 translator isn't compiled into this build.
- The spec gets amended to match, and the four families become a follow-up.

## Design notes

**Reused as is:**
- the diag snapshot mutex pattern (`p4/components/diag/sampler.c`, `diag_snapshot_json`);
- `reset_reason_name` in the same file;
- `diag_ot_lock_view` (`ot_lock_wrap.c`);
- the gated route table with `netcfg_request_allowed(req, NETCFG_SVC_DEVICE_API)` (`device_api.c:1082`);
- `sniffer_get_stats`;
- the management netif lookup in `fill_eth` (`device_api.c:206`);
- the host-test layout of `diag/test_host`.

**New small accessors:**
- `br_core_get_status_brief()`: the existing first half of `br_core_get_status`, which takes no lock. `br_core_get_status` calls it, so nothing is duplicated.
- `netcfg_trial_pending()`: `s.trial.on_trial` under `s.lock`.

**Shapes:**
- `diag_metrics_view_t` (in `diag/include/diag.h`, committed):
  - uptime and reset reason;
  - `wall_us`/`idle_us` per core;
  - 48 tasks with `name`, `cpu_us`, `stack_free_min`, plus the untracked count;
  - internal and PSRAM heap;
  - lock acquisitions, hold and wait totals, and boot/60 s maxima.
- `metrics_types.h`: `metrics_device_t`, `metrics_thread_t` (plain copies of the OpenThread counters), `metrics_collector_view_t` (started, ever_collected, valid, age_ms, failures, thread) and `metrics_input_t`. A `NULL` diag or collector pointer means that source was unavailable.
- Writer: `metrics_writer_init/raw/u64/usec/label_value/finish`, flushing through a callback. Renderer: `esp_err_t metrics_render(metrics_writer_t *, const metrics_input_t *)`.

**Duplicate task names.** A second FreeRTOS task with the same name is labelled `<name>#2`, a third `<name>#3`. Duplicate series would make Prometheus reject the whole scrape.

**Access.** `/metrics` goes through the same gate as `/device/*`, so in split mode it answers only on the management side.

## BDD skeleton

```
diag_windows (windows.c)                                  [done in 73f839a]
    diag_windows_update
        On a task's samples
            - adds every runtime delta to that task's cpu_us
        On the 32-bit runtime counter wrapping
            - cpu_us keeps increasing by the modulo-2^32 delta
        On a task replaced by a new one with the same name
            - the new cpu_us starts from its own runtime
        On each sample
            - adds the wall delta to wall_us[c] and the IDLE task delta to idle_us[c]

diag_lock_stats (lock_stats.c)                            [done in 73f839a]
    diag_lock_on_acquire
        On any acquire, timeouts included
            - adds wait_us to wait_us_total
    diag_lock_on_release
        On the outermost release
            - adds the hold time to hold_us_total
        On a nested release
            - leaves hold_us_total unchanged

diag (sampler.c)
    diag_metrics_view
        Before diag_start            - returns ESP_ERR_INVALID_STATE
        On NULL                      - returns ESP_ERR_INVALID_ARG
        Mutex not free in 100 ms     - returns ESP_ERR_TIMEOUT
        On success                   - fills totals, tasks, heap, lock, uptime, reset reason; no OpenThread lock

metrics_writer (writer.c)
    raw / u64 / usec / label_value
        Output larger than the buffer  - flushes full buffers in order; same bytes as one large buffer
        Flush error                    - stops writing; finish returns that error
        usec                           - 1500000 -> "1.500000", 0 -> "0.000000"
        label_value                    - escapes \ " newline as \\ \" \n
    finish
        On success                     - flushes the rest once, returns ESP_OK

metrics_render (render.c)
    metrics_render
        Any input
            - HELP and TYPE once per family with samples, before its samples
            - families in catalogue order; a family without samples is left out entirely
        Device
            - br_info has version, slot, rcp_version, rcp_compatible, value 1
            - br_mode: exactly one of four series at 1
            - br_cpu_seconds_total: core 0/1 x busy/idle, busy = wall - idle
            - lock and heap families carry the diag values with the spec's labels
        Two tasks with the same name
            - the second is "<name>#2"
        NULL diag
            - diag families left out, br_scrape_partial 1
        NULL collector, or collector not started
            - Thread and collector families left out, br_scrape_partial 1
        Valid cache younger than 30 s
            - every Thread family; each state set has exactly one series at 1; br_scrape_partial 0
        Cache aged >= 30 s, or invalid
            - Thread families left out; age and failures still written; br_scrape_partial 0
        Before the first successful collection
            - no age; failures written
        pd_state -1
            - br_dhcp6_pd_state left out
        Writer error
            - returns that error

metrics collector (collector.c)
    metrics_start
        First call                 - creates the mutex and task, ESP_OK
        Second call                - ESP_ERR_INVALID_STATE
        Task or mutex fails        - releases what was created, ESP_ERR_NO_MEM, can be retried
    each period
        Mode not BR_RUNNING        - marks the cache invalid, no OpenThread lock
        Lock not free in 10 ms     - failures + 1, old cache kept
        Success                    - stores every metrics_thread_t field and the time; valid = true
    metrics_collector_copy
        Mutex not free in 100 ms   - ESP_ERR_TIMEOUT
        Otherwise                  - copies the cache; age_ms from the collection time

metrics_get_handler (metrics_handler.c)
    On success                     - 200, 0.0.4 content type, chunks <= 2 KB, empty final chunk
    Buffers not allocated          - 500 {"error":"no_memory","detail":"out of memory"}, no chunk
    Chunk send fails               - stops, frees everything, ESP_FAIL

br_core_get_status_brief           - mode, storage, RCP, fault reason; never the OpenThread lock
netcfg_trial_pending               - false before netcfg_start; true during a trial
GET /metrics route
    Split mode, Thread side        - 403 like /device/*
    Management side, any mode      - served by metrics_get_handler

On-board (p4/pytest/test_metrics.py, next hardware pass)
    GET /metrics
        Any mode                   - 200, parses with prometheus_client, br_mode has one series at 1
        BR_RUNNING with a network  - after 12 s br_thread_role and br_mac_tx_frames_total present, age < 30
```

The collector, handler and accessors run only on the target, so they have no host tests. They are covered by the build, the on-board test and the final review. `br_core_get_status_brief` and `netcfg_trial_pending` are checked by reading them.

## Steps

0. **Keep the started work, done properly.** The repo plan file `docs/superpowers/plans/2026-11-02-prometheus-metrics.md` is replaced with this plan.
   - Task 1 (`73f839a`) stays.
   - For the uncommitted Task 2: move `writer.c`/`render.c` aside, build the metrics host tests and watch them fail; restore the files, watch them pass, then fix what fails.
1. **diag view.** `diag_metrics_view` already exists in `sampler.c`, uncompiled. It gets compiled by the step 3 target build.
2. **Writer and renderer.** Finish them against the skeleton, run `make test-host`, commit `feat(metrics): Prometheus text renderer`. The host test also writes `sample.prom` for step 4.
3. **Collector, handler and wiring.**
   - Create `collector.c`, `collector.h`, `metrics_handler.c` and `include/metrics.h`.
   - Add the br_core and netcfg accessors.
   - Add the route `{"/metrics", HTTP_GET, metrics_get_handler}` to `device_api.c`; `device_api` requires `metrics`.
   - Call `metrics_start()` in `p4/main/app_main.c` after `diag_start()`, and add `metrics` to `p4/main/CMakeLists.txt`.
   - Run `logrun make build`, then `make test-host`, then commit.
4. **Format check.**
   - `tools/tests/test_metrics_format.py` parses `p4/components/metrics/test_host/build/sample.prom` with `prometheus_client.parser.text_string_to_metric_families`. It checks the names and types against the catalogue and that no series appears twice.
   - Add `p4/pytest/test_metrics.py` for the board.
   - Add `prometheus_client` to `p4/pytest/requirements.txt` and to the CI pip line in `.github/workflows/build.yml`.
   - Run `make test-tools`, then commit.
5. **Docs.** No BDD here, since it is docs only.
   - `docs/metrics.md`: scrape config, catalogue, the freshness and partial rules, example alerts.
   - A row in `docs/api.md`, an entry in `docs/README.md`, the README documentation line, and a `CHANGELOG.md` Unreleased entry.
   - The NAT64 amendment in the spec and in the published doc.
   - Run `make lint`, then commit.
6. **Review.** Run `make test` and `make build`, then a whole-branch review on the most capable model. Fix the Critical and Important findings, each test-first.

## Verification

- `logrun make test-host`: the diag and metrics suites are green.
- `logrun make test-tools`: the sample render parses with `prometheus_client`, matches the catalogue, and has no duplicate series.
- `logrun make build`: the release image builds; it covers `sampler.c`, the collector, the handler and the wiring.
- `logrun make lint`.
- Hardware (deferred, batched): `make test-target TEST=p4/pytest/test_metrics.py`, and one `curl http://<board>/metrics` while sniffing and in `BR_RUNNING`.
