# Prometheus metrics endpoint: implementation plan

> **For agentic workers:** executed natively (superpowers:executing-plans) in one session, then one whole-branch
> review. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `GET /metrics` serves device health and cached Thread figures in Prometheus text format.

**Architecture:** a new `metrics` component holds a pure renderer (host-tested), a Thread collector task that reads
OpenThread under a 10 ms lock every 10 s into a cache, and the HTTP handler that copies each source once and streams
the body in 2 KB chunks. `diag` adds 64-bit CPU totals, lock sums and a copy-out view; `br_core` adds a status read
without the OpenThread lock; `netcfg` adds a trial flag; `device_api` registers the route.

**Tech stack:** C (ESP-IDF v6.1, FreeRTOS, OpenThread), Unity host tests on the `linux` target, pytest with
`prometheus_client`.

**Spec:** `docs/superpowers/specs/2026-11-02-prometheus-metrics-design.md`

**Styleguide:** `STYLEGUIDE.md` (astyle via `make lint`, `snake_case`, component prefixes, `esp_err_t` +
`ESP_RETURN_ON_ERROR`, host tests in `<component>/test_host/` named `"<scenario>: <effect>"`, target tests in
`p4/pytest/test_<feature>.py`, routine tasks through the Makefile). New public symbols use the prefix `metrics_`.

## Global constraints

- Every metric name starts with `br_`; names, types and labels exactly as the spec's section 4.
- Collector: period 10 s (`METRICS_COLLECT_PERIOD_MS 10000`), lock timeout 10 ms (`METRICS_COLLECT_LOCK_MS 10`; one
  tick at `CONFIG_FREERTOS_HZ` 100), freshness 30 s (`METRICS_FRESH_MS 30000`), core 1, lowest application priority,
  3 KB stack.
- A scrape never takes the OpenThread lock; source mutexes are waited on for at most 100 ms.
- Content type `text/plain; version=0.0.4; charset=utf-8`; body sent in chunks of at most 2 KB (`METRICS_CHUNK_BYTES`).
- No floats in rendering: seconds from microseconds are written as `<s>.<6 digits>`.
- No flashing during tasks; the on-board test runs in the next batched hardware pass.
- Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` only.

## Spec deviation (ruling)

- The NAT64 translator is not compiled into this build: `otNat64GetCounters` and `otNat64GetTranslatorState` are
  absent from every linked library (checked with `nm`). The `br_nat64_state`, `br_nat64_packets_total`,
  `br_nat64_bytes_total` and `br_nat64_errors_total` families are dropped; the spec is amended in Task 5 and the
  removal is listed as a follow-up (export them if the translator is enabled later).

## Review focus

1. Two FreeRTOS tasks with the same name would produce duplicate series, which Prometheus rejects for the whole
   scrape: the renderer must make `task` label values unique (Task 2 test).
2. A chunk boundary falling inside a sample or an escape sequence must not change the bytes (Task 2 chunking test).
3. The 32-bit runtime counter wraps every ~71 min; the 64-bit totals must keep growing across a wrap (Task 1 test).
4. A collector start failure or a never-successful collection must leave device metrics intact and set
   `br_scrape_partial` only for a missing collector, not for a stale cache (Task 2 tests).
5. A client that disconnects mid-body must not leak the buffers or the diag view (Task 3, reviewed by reading: every
   exit path of the handler frees both).

## File structure

- `p4/components/diag/windows.{h,c}`: 64-bit totals per task (`total_us`) and per core (`wall_us`, `idle_us`).
- `p4/components/diag/lock_stats.{h,c}`: `hold_us_total`, `wait_us_total`.
- `p4/components/diag/include/diag.h`, `sampler.c`, `ot_lock_wrap.c`: `diag_metrics_view_t`, `diag_metrics_view()`.
- `p4/components/metrics/` (new):
  - `include/metrics.h`: `metrics_start()`, `metrics_get_handler()`;
  - `metrics_types.h`: the plain input structs the renderer consumes (no OpenThread types);
  - `writer.{h,c}`: chunked text writer;
  - `render.{h,c}`: the families;
  - `collector.c`, `collector.h`: task and cache;
  - `metrics_handler.c`: HTTP glue;
  - `CMakeLists.txt`, `test_host/`.
- `p4/components/br_core/`: `br_core_get_status_brief()`.
- `p4/components/netcfg/`: `netcfg_trial_pending()`.
- `p4/components/device_api/device_api.c`, `CMakeLists.txt`: the route.
- `p4/main/app_main.c`, `p4/main/CMakeLists.txt`: `metrics_start()`.
- `tools/tests/test_metrics_format.py`, `p4/pytest/test_metrics.py`, `p4/pytest/requirements.txt`, CI pip line.
- `docs/metrics.md`, `docs/api.md`, `docs/README.md`, `README.md`, `CHANGELOG.md`, the spec.

## Data shapes (contract between tasks)

`diag_metrics_view_t` (diag.h):

- `uint32_t uptime_s`; `const char *reset_reason` (static string);
- `struct { uint64_t wall_us, idle_us; } cores[DIAG_CORES]` (busy = wall − idle);
- `size_t task_count`; `struct { char name[16]; uint64_t cpu_us; uint32_t stack_free_min; } tasks[DIAG_MAX_TASKS]`;
  `uint32_t untracked`;
- `struct { uint32_t free, min_free, largest_free_block; } internal, psram`;
- `struct { uint32_t acquisitions; uint64_t hold_us_total, wait_us_total; uint32_t max_hold_us, max_wait_us,
  max_hold_60s_us, max_wait_60s_us; } lock`.

`metrics_types.h`:

- `metrics_device_t`: `int mode` (br_core_mode_t), `const char *version`, `int slot`, `const char *rcp_version`
  (`""` unknown), `bool rcp_compatible`, `bool storage_ok`, `bool sniffer_active`, `uint32_t sniffer_frames`,
  `uint32_t sniffer_drops`, `bool eth_up`, `bool trial_pending`.
- `metrics_thread_t`: `int role` (0 disabled … 4 leader, OpenThread's order); MLE `uint32_t role_entered[5]`,
  `partition_changes`, `attach_attempts`; `uint32_t routers, children, neighbors`; MAC `tx_total, tx_retry,
  tx_err_cca, tx_err_abort, tx_err_busy_channel, rx_total, rx_duplicated, rx_err_fcs, rx_err_sec, rx_err_no_frame,
  rx_err_unknown_neighbor, rx_err_invalid_src, rx_err_other`; IP `tx_success, tx_failure, rx_success, rx_failure`;
  `int br_state` (0..3), `int pd_state` (0..3, −1 unknown); BR counters `uint64_t packets[2][2], bytes[2][2]`
  (direction × cast), `uint32_t ra[3], rs[3]` (rx, tx_success, tx_failure); `int srp_state` (0..2);
  `uint32_t srp_hosts, srp_services`.
- `metrics_collector_view_t`: `bool started`, `bool ever_collected`, `bool valid`, `uint32_t age_ms`,
  `uint32_t failures`, `metrics_thread_t thread`.
- `metrics_input_t`: `const metrics_device_t *device`; `const diag_metrics_view_t *diag` (NULL = unavailable);
  `const metrics_collector_view_t *collector` (NULL = cache mutex timed out).

Writer (`writer.h`): `metrics_writer_init(metrics_writer_t *w, char *buf, size_t cap, metrics_flush_fn flush, void
*ctx)`, where `typedef esp_err_t (*metrics_flush_fn)(void *ctx, const char *data, size_t len)`;
`metrics_writer_raw`, `metrics_writer_u64`, `metrics_writer_usec` (µs as seconds), `metrics_writer_label_value`
(escaped); `esp_err_t metrics_writer_finish(metrics_writer_t *w)` flushes the rest and returns the first flush error.

Renderer (`render.h`): `esp_err_t metrics_render(metrics_writer_t *w, const metrics_input_t *in)`.

## BDD skeleton

```
diag_windows (windows.c)
    diag_windows_update
        On a task's samples
            - adds every runtime delta to that task's total_us
        On the 32-bit runtime counter wrapping
            - total_us keeps increasing by the modulo-2^32 delta
        On a task that disappears and a new task with the same name
            - the new task's total_us starts from its own runtime, not the old total
        On each sample
            - adds the wall delta to cores[c].wall_us and the core's IDLE task delta to cores[c].idle_us

diag_lock_stats (lock_stats.c)
    diag_lock_on_acquire
        On a successful or timed-out acquire
            - adds wait_us to wait_us_total
    diag_lock_on_release
        On the outermost release
            - adds the hold time to hold_us_total
        On a nested release
            - leaves hold_us_total unchanged
    diag_lock_view
        Always
            - reports hold_us_total and wait_us_total since boot

diag (sampler.c)
    diag_metrics_view
        Before diag_start
            - returns ESP_ERR_INVALID_STATE
        On a NULL argument
            - returns ESP_ERR_INVALID_ARG
        When the snapshot mutex is not free within 100 ms
            - returns ESP_ERR_TIMEOUT
        On success
            - fills totals, tasks, heap, lock figures, uptime and reset reason; never takes the OpenThread lock

metrics_writer (writer.c)
    metrics_writer_raw / _u64 / _usec / _label_value
        On output larger than the buffer
            - calls flush with each full buffer, in order; concatenated output equals a single large-buffer render
        On a flush error
            - stops writing; metrics_writer_finish returns that error
        _usec
            - writes 1500000 µs as "1.500000" and 0 as "0.000000"
        _label_value
            - escapes backslash, double quote and newline as \\, \" and \n
    metrics_writer_finish
        On success
            - flushes the remaining bytes once and returns ESP_OK

metrics_render (render.c)
    metrics_render
        On any input
            - writes "# HELP" and "# TYPE" once per family that has samples, before its samples
            - writes families in the order of the spec's catalogue
            - leaves out a family with no samples, including its HELP and TYPE
        Device family
            - br_info carries version, slot, rcp_version, rcp_compatible labels with value 1
            - br_mode has exactly one series at 1 among STARTING, BR_RUNNING, SNIFFER, RADIO_FAULT
            - br_cpu_seconds_total has core 0/1 × mode busy/idle, busy = wall − idle
            - br_ot_lock_* and br_heap_* carry the diag values with the spec's labels
        On two tasks with the same name
            - the second task's label value is "<name>#2" (the third "#3"), so every series is unique
        On a NULL diag view
            - leaves out every diag-sourced family and writes br_scrape_partial 1
        On a NULL collector view, or a collector that is not started
            - leaves out Thread and collector families and writes br_scrape_partial 1
        On a valid cache younger than 30 s
            - writes every Thread family; br_thread_role and each state set have exactly one series at 1
        On a cache aged 30 s or more, or invalid
            - leaves out the Thread families; still writes br_thread_collect_age_seconds and _failures_total
            - writes br_scrape_partial 0 when every source was available
        Before the first successful collection
            - leaves out br_thread_collect_age_seconds, writes br_thread_collect_failures_total
        On pd_state −1
            - leaves out br_dhcp6_pd_state
        On a writer error
            - returns that error

metrics collector (collector.c)
    metrics_start
        On first call
            - creates the cache mutex and the collector task, returns ESP_OK
        On a second call
            - returns ESP_ERR_INVALID_STATE
        When the task or mutex cannot be created
            - releases what was created, returns ESP_ERR_NO_MEM; a later call may retry
    collector period
        In a mode other than BR_RUNNING
            - marks the cache invalid without taking the OpenThread lock
        When the lock is not free within 10 ms
            - increments failures and keeps the previous cache
        On success
            - stores every metrics_thread_t field read under the lock and the collection time; valid = true
    metrics_collector_copy (private)
        When the cache mutex is not free within 100 ms
            - returns ESP_ERR_TIMEOUT
        Otherwise
            - copies the cache and computes age_ms from the collection time

metrics_handler (metrics_handler.c)
    metrics_get_handler
        On success
            - answers 200 with text/plain; version=0.0.4; charset=utf-8 in chunks of at most 2 KB, ends with an empty chunk
        When the buffers cannot be allocated
            - answers 500 {"error":"no_memory","detail":"out of memory"} and sends no chunk
        When a chunk send fails
            - stops, frees the buffers, returns ESP_FAIL

br_core
    br_core_get_status_brief
        Always
            - fills mode, storage, RCP fields and fault reason; never takes the OpenThread lock; ot_state_valid = false

netcfg
    netcfg_trial_pending
        Before netcfg_start
            - returns false
        During a trial
            - returns true

device_api route table
    GET /metrics
        In split mode from the Thread side
            - answers 403 like /device/*
        From the management side, in any mode
            - is served by metrics_get_handler
```

On-board (`p4/pytest/test_metrics.py`, next batched hardware pass):

```
GET /metrics
    In any mode
        - answers 200 with the 0.0.4 content type and parses with prometheus_client
        - br_mode has exactly one series at 1
    In BR_RUNNING with a formed network, after 12 s
        - br_thread_role and br_mac_tx_frames_total are present
        - br_thread_collect_age_seconds is below 30
```

---

### Task 1: diag totals, lock sums and the metrics view

**Files:** modify `p4/components/diag/windows.h`, `windows.c`, `lock_stats.h`, `lock_stats.c`, `include/diag.h`,
`sampler.c`, `ot_lock_wrap.c`, `diag_priv.h`; tests in `p4/components/diag/test_host/main/test_windows.c`,
`test_lock_stats.c`.

**Interfaces:** produces `diag_metrics_view_t` and `esp_err_t diag_metrics_view(diag_metrics_view_t *out)`.

- [ ] Write the failing host tests for the `diag_windows` and `diag_lock_stats` scenarios of the skeleton.
- [ ] Run `logrun make test-host`; expect the new tests to fail (missing fields).
- [ ] Add `total_us` to `diag_task_track_t` (moved with the task on drop), `wall_us`/`idle_us` per core to
  `diag_windows_t`; accumulate in `diag_windows_update`. Add `hold_us_total`/`wait_us_total` to the stats and the view.
- [ ] Add `diag_metrics_view` in `sampler.c`: copy under the snapshot mutex (100 ms), lock figures through
  `diag_ot_lock_view`, uptime and reset reason as in `diag_snapshot_json`.
- [ ] Run `logrun make test-host`; expect pass. Commit `feat(diag): CPU-time totals, lock sums and a metrics view`.

### Task 2: writer and renderer (pure)

**Files:** create `p4/components/metrics/{CMakeLists.txt,metrics_types.h,writer.h,writer.c,render.h,render.c}`,
`p4/components/metrics/test_host/{CMakeLists.txt,main/CMakeLists.txt,main/test_writer.c,main/test_render.c}`.

**Interfaces:** consumes `diag_metrics_view_t`; produces the writer, `metrics_render`, `metrics_types.h`.

- [ ] Create the component; on `linux` it builds only `writer.c` and `render.c` (like `diag`). The test project lists
  `metrics`, `diag`, `device_api`, `rcp_sync`, `rcp_sync_proto` in `EXTRA_COMPONENT_DIRS` (as diag's test project).
- [ ] Write the failing host tests for every `metrics_writer` and `metrics_render` scenario, with a flush callback that
  appends to a growing string; the chunking test renders a full input once with a 64-byte buffer and once with 64 KB
  and compares. One extra test case writes the full-input render to `METRICS_SAMPLE_PATH` (a compile definition set to
  `${CMAKE_BINARY_DIR}/sample.prom`) for Task 4.
- [ ] Run `logrun make test-host`; expect failure.
- [ ] Implement writer and renderer.
- [ ] Run `logrun make test-host`; expect pass. Commit `feat(metrics): Prometheus text renderer`.

### Task 3: collector, handler and wiring

**Files:** create `p4/components/metrics/{include/metrics.h,collector.h,collector.c,metrics_handler.c}`; modify
`p4/components/br_core/include/br_core.h`, `br_core.c`, `p4/components/netcfg/include/netcfg.h`, `netcfg.c`,
`p4/components/device_api/device_api.c`, `device_api/CMakeLists.txt`, `p4/main/app_main.c`, `p4/main/CMakeLists.txt`.

**Interfaces:** consumes Tasks 1–2; produces `metrics_start`, `metrics_get_handler`, `br_core_get_status_brief`,
`netcfg_trial_pending`.

- [ ] `br_core_get_status_brief`: the part of `br_core_get_status` before the lock; `br_core_get_status` calls it
  first (no duplicated block).
- [ ] `netcfg_trial_pending`: `s.ready && s.trial.on_trial` under `s.lock`.
- [ ] Collector per the skeleton. Under the lock: role, MLE, router walk (`0..otThreadGetMaxRouterId`,
  `otThreadGetRouterInfo` OK), children (`0..otThreadGetMaxAllowedChildren`, `otThreadGetChildInfoByIndex` OK),
  neighbours (`otThreadGetNextNeighborInfo`), MAC, IP, BR state, PD state, BR counters, SRP state and the non-deleted
  host and service counts. No allocation or logging under the lock.
- [ ] Handler per the skeleton: allocate the chunk buffer and the diag view; gather device fields (br_core brief,
  `esp_app_get_description`, running slot, sniffer stats, `esp_netif_is_netif_up(netcfg_mgmt_netif())` falling back to
  the Ethernet netif as `fill_eth` does, `netcfg_trial_pending`), `diag_metrics_view` (any error → NULL),
  `metrics_collector_copy` (timeout → NULL); render with `httpd_resp_send_chunk` as flush.
- [ ] Route `{"/metrics", HTTP_GET, metrics_get_handler}` in the gated table; `device_api` requires `metrics`.
- [ ] `app_main`: `metrics_start()` after `diag_start()`, error logged, boot goes on.
- [ ] Run `logrun make build` (release image) and `logrun make test-host`; expect both to pass. Commit
  `feat(metrics): GET /metrics with a cached Thread collector`.

### Task 4: format check and on-board test

**Files:** create `tools/tests/test_metrics_format.py`, `p4/pytest/test_metrics.py`; modify
`p4/pytest/requirements.txt` (`prometheus_client==0.21.1`), `.github/workflows/build.yml` (pip line adds
`prometheus_client`).

- [ ] `test_metrics_format.py`: reads `p4/components/metrics/test_host/build/sample.prom` (fails with "run make
  test-host first" if missing), parses with `text_string_to_metric_families`, checks that the family names and types
  equal the catalogue (minus NAT64) and that no two samples share a name and label set.
- [ ] `test_metrics.py`: the on-board skeleton; not run now.
- [ ] Run `logrun make test-tools` with prometheus_client installed in the IDF env; expect pass. Commit
  `test(metrics): exposition format parsed by prometheus_client`.

### Task 5: docs and spec amendment

BDD skipped: docs only.

- [ ] `docs/metrics.md`: scrape config (one job, several targets), the catalogue, freshness and partial rules, three
  example alerts. Row in `docs/api.md`, entry in `docs/README.md`, the README documentation line, CHANGELOG Unreleased.
- [ ] Spec: remove the NAT64 rows, add the deviation note and the follow-up.
- [ ] `logrun make lint`; commit `docs: metrics endpoint`.

### Final

- [ ] `logrun make test` and `logrun make build`; one whole-branch review on the most capable model; fix findings.
