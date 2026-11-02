# Prometheus metrics endpoint: design

Date: 2026-11-02. Status: draft for review.

## 1. Purpose and scope

Let people monitor one or many boards from Prometheus (and Grafana) without polling JSON: device health (CPU, memory,
tasks, OpenThread lock, mode, versions) and the Thread network the board serves (role, mesh size, MAC and IP counters,
border routing, SRP).

In scope: `GET /metrics` in the Prometheus text format, a Thread collector that caches OpenThread figures, two small
additions to `diag`, docs.

Out of scope (follow-ups in section 10): per-neighbour series, mDNS service discovery, authentication, push gateways,
OpenMetrics format.

## 2. Decisions

| Topic | Decision |
| --- | --- |
| Path | `GET /metrics`, port 80: the Prometheus default, so a scrape config needs only the target |
| Format | Prometheus text exposition 0.0.4, `Content-Type: text/plain; version=0.0.4; charset=utf-8` |
| Prefix | every metric starts with `br_` |
| Coverage | device health plus Thread network figures; no per-neighbour series |
| Thread read | a cached collector every 10 s; a scrape never takes the OpenThread lock |
| Collector lock timeout | 10 ms |
| Freshness | Thread series only while the cache is valid and younger than 30 s |
| Cumulative values | counters (`_total`), so `rate()` works; no percentages are exported |
| States | state sets: one series per state, value 1 for the current state, 0 for the others |
| Partial data | a busy or missing source drops its series and sets `br_scrape_partial 1`; the scrape still answers 200 |
| Auth | none, like the rest of the API |
| Modes | answers in every mode (`STARTING`, `BR_RUNNING`, `SNIFFER`, `RADIO_FAULT`) |

## 3. Architecture

- New component `p4/components/metrics/`:
  - `render.c`: pure text-format writer and the metric families (no FreeRTOS, no OpenThread): host-tested;
  - `collector.c`: the Thread collector task and its cache;
  - `metrics_handler.c`: the HTTP glue that gathers the sources and streams the body;
  - `include/metrics.h`: `metrics_start()` (starts the collector) and `metrics_get_handler(httpd_req_t *)`.
- `device_api` registers `GET /metrics` in its route table (`device_api.c`), before the catch-all. The extra-handler
  budget (`EXTRA_HANDLER_SLOTS 16` in `esp_br_web.c`) has room: 13 device routes plus the catch-all today.
- `diag` gains `diag_metrics_view(diag_metrics_view_t *out)`: copies 64-bit CPU totals per core (busy and idle) and per
  task, minimum free stack per task, the untracked count, heap figures and the lock sums under the existing snapshot
  mutex (waits at most 100 ms, then `ESP_ERR_TIMEOUT`; `ESP_ERR_INVALID_STATE` before `diag_start`). It never takes the
  OpenThread lock.
- `diag` sampler keeps 64-bit running totals of CPU time per task and per core alongside its windows, accumulated from the
  existing 32-bit deltas, so the totals survive the runtime counter wrapping. A task that disappears drops out; a new task
  with a reused name starts again from 0 (a counter reset to Prometheus).
- `diag` lock wrapper adds `hold_us_total` and `wait_us_total` (64-bit) since boot to `diag_lock_stats_t` and the view.
- The status figures (mode, versions, storage, sniffer, PD state, Ethernet, trial) come from the same sources
  `/device/status` and `/device/network` use, without their OpenThread lock read: PD state comes from the collector.
- `app_main.c` calls `metrics_start()` after `diag_start()`. A failure is logged and the boot goes on; `/metrics` then
  serves device metrics only.

## 4. Metric catalogue

Every family has `# HELP` and `# TYPE` lines. Families appear in this order; a family with no samples in a scrape is left
out entirely (no `HELP`/`TYPE` either).

### 4.1 Device (always present unless the source was skipped)

| Metric | Type | Labels | Source |
| --- | --- | --- | --- |
| `br_info` (value 1) | gauge | `version`, `slot`, `rcp_version` (`""` unknown), `rcp_compatible` (`true`/`false`) | status |
| `br_uptime_seconds` | gauge | | diag |
| `br_reset_reason` (value 1) | gauge | `reason` | diag |
| `br_mode` | gauge, state set | `mode` = STARTING, BR_RUNNING, SNIFFER, RADIO_FAULT | br_core |
| `br_cpu_seconds_total` | counter | `core` = 0/1, `mode` = busy/idle | diag |
| `br_task_cpu_seconds_total` | counter | `task` | diag |
| `br_task_stack_free_min_bytes` | gauge | `task` | diag |
| `br_tasks_untracked` | gauge | | diag |
| `br_heap_free_bytes` | gauge | `region` = internal/psram | diag |
| `br_heap_min_free_bytes` | gauge | `region` | diag |
| `br_heap_largest_free_block_bytes` | gauge | `region` | diag |
| `br_ot_lock_acquisitions_total` | counter | | diag |
| `br_ot_lock_hold_seconds_total` | counter | | diag |
| `br_ot_lock_wait_seconds_total` | counter | | diag |
| `br_ot_lock_hold_max_seconds` | gauge | `window` = boot/60s | diag |
| `br_ot_lock_wait_max_seconds` | gauge | `window` = boot/60s | diag |
| `br_storage_ok` | gauge 0/1 | | status |
| `br_sniffer_active` | gauge 0/1 | | sniffer |
| `br_sniffer_frames_total` | counter (per capture) | | sniffer |
| `br_sniffer_drops_total` | counter (per capture) | | sniffer |
| `br_eth_up` | gauge 0/1 | | netcfg |
| `br_network_trial_pending` | gauge 0/1 | | netcfg |
| `br_scrape_partial` | gauge 0/1 | | handler |

Task names are the FreeRTOS names the sampler already tracks (at most 48, `br_tasks_untracked` counts the rest).

### 4.2 Thread (from the collector cache, only while fresh)

| Metric | Type | Labels | OpenThread source |
| --- | --- | --- | --- |
| `br_thread_role` | state set | `role` = disabled, detached, child, router, leader | `otThreadGetDeviceRole` |
| `br_thread_role_entered_total` | counter | `role` | `otThreadGetMleCounters` |
| `br_thread_partition_changes_total` | counter | | MLE counters |
| `br_thread_attach_attempts_total` | counter | | MLE counters |
| `br_thread_routers` | gauge | | router table (`otThreadGetRouterInfo` over ids) |
| `br_thread_children` | gauge | | `otThreadGetChildInfoByIndex` |
| `br_thread_neighbors` | gauge | | `otThreadGetNextNeighborInfo` |
| `br_mac_tx_frames_total` | counter | | `otLinkGetCounters` |
| `br_mac_tx_retries_total` | counter | | MAC counters |
| `br_mac_tx_errors_total` | counter | `reason` = cca, abort, busy_channel | MAC counters |
| `br_mac_rx_frames_total` | counter | | MAC counters |
| `br_mac_rx_duplicated_total` | counter | | MAC counters |
| `br_mac_rx_errors_total` | counter | `reason` = fcs, security, no_frame, unknown_neighbor, invalid_src, other | MAC counters |
| `br_thread_ip6_packets_total` | counter | `direction` = tx/rx, `result` = success/failure | `otThreadGetIp6Counters` |
| `br_border_routing_state` | state set | `state` = uninitialized, disabled, stopped, running | `otBorderRoutingGetState` |
| `br_dhcp6_pd_state` | state set | `state` = disabled, stopped, running, idle | `otBorderRoutingDhcp6PdGetState` |
| `br_border_routing_packets_total` | counter | `direction` = inbound/outbound, `cast` = unicast/multicast | `otIp6GetBorderRoutingCounters` |
| `br_border_routing_bytes_total` | counter | `direction`, `cast` | BR counters |
| `br_border_routing_ra_total` | counter | `event` = rx, tx_success, tx_failure | BR counters |
| `br_border_routing_rs_total` | counter | `event` = rx, tx_success, tx_failure | BR counters |
| `br_srp_server_state` | state set | `state` = disabled, running, stopped | `otSrpServerGetState` |
| `br_srp_hosts` | gauge | | `otSrpServerGetNextHost` (not deleted) |
| `br_srp_services` | gauge | | `otSrpServerHostGetNextService` (not deleted) |

NAT64 translation counters are not exported: the NAT64 translator is not compiled into this firmware
(`otNat64GetCounters` and `otNat64GetTranslatorState` are absent from the linked libraries). See section 10.

### 4.3 Collector (always present)

| Metric | Type | Notes |
| --- | --- | --- |
| `br_thread_collect_age_seconds` | gauge | seconds since the last successful collection; left out before the first one |
| `br_thread_collect_failures_total` | counter | collections skipped because the lock was not free within 10 ms |

OpenThread counters restart when the stack restarts (sniffer exit, radio fault recovery, a radio update). Prometheus
treats a decrease as a counter reset; the endpoint does nothing special.

## 5. Collector

- One FreeRTOS task, core 1, lowest application priority, 3 KB stack, period 10 s (`METRICS_COLLECT_PERIOD_MS`).
- Each period:
  - mode not `BR_RUNNING`: mark the cache invalid and wait for the next period;
  - otherwise take the OpenThread lock with a 10 ms timeout (`METRICS_COLLECT_LOCK_MS`). On timeout increment the failure
    counter, keep the old cache, try again at the next period (no retry inside a period);
  - with the lock held, read every figure in 4.2 into a stack-local struct, release the lock, then copy the struct into
    the cache under the cache mutex with the collection time.
- Work under the lock is reads only: no allocation, no logging, no waiting. The SRP walk is bounded by the server's host
  table; the router walk by the router id range.
- A scrape copies the cache under the mutex (waits at most 100 ms; on timeout the Thread series are skipped and
  `br_scrape_partial` is 1).
- Freshness at scrape: Thread series (4.2) appear only when the cache is valid and its age is below 30 s
  (`METRICS_FRESH_MS`). A stale or invalid cache leaves them out, so dashboards show gaps instead of frozen values.
- `metrics_start()` returns `ESP_ERR_INVALID_STATE` if already started and `ESP_ERR_NO_MEM` if the task or mutex cannot
  be created (everything created is released). Before a successful start the Thread series and 4.3 are absent.

## 6. Rendering and HTTP

- `render.c` writes into a caller buffer through a small writer that reports overflow, like `json_out`. Families are
  rendered one at a time; the handler sends each filled 2 KB chunk with `httpd_resp_send_chunk` and finishes with an
  empty chunk. No buffer for the whole body is allocated.
- A sample is `name{label="value",...} value`. Label values escape `\` as `\\`, `"` as `\"`, newline as `\n`. Integer
  values are written as integers; seconds from microsecond sources are written with six decimals, without floats.
- Labels within a sample are in the fixed order of the catalogue. No timestamps are written.
- Errors: a failure before the first chunk (out of memory) answers 500 with the JSON error body used by `/device/*`
  (`no_memory`). A send failure mid-body aborts the response. Busy or missing sources never fail the scrape.
- The handler gathers sources in this order: status figures, diag view, collector cache. Each source is copied once,
  then rendering runs without holding any lock.

## 7. Testing

- Host tests (`p4/components/metrics/test_host/`, the same harness as the other components):
  - exposition format: `HELP`/`TYPE` per family, sample syntax, integer and six-decimal values;
  - label escaping;
  - every state set has exactly one series at 1;
  - families with no samples are left out entirely;
  - freshness: a cache aged 29 s renders Thread series, 30 s and invalid do not;
  - partial: a skipped source sets `br_scrape_partial 1` and drops only its families;
  - chunking: output split across buffer boundaries is identical to one large buffer.
- Host tests in `diag`: 64-bit CPU totals across a 32-bit runtime wrap; task disappearing and reappearing; lock hold and
  wait sums, including nested acquires and timeouts.
- Tools test (`tools/tests/`): a host program renders a fixed sample input; pytest parses it with
  `prometheus_client.parser.text_string_to_metric_families` and checks family names, types and label sets against the
  catalogue.
- On-board (`p4/pytest_*`, run only in the next batched hardware pass): scrape `/metrics`; it parses; `br_mode` has
  exactly one series at 1; in `BR_RUNNING` with a formed network `br_thread_role` and `br_mac_tx_frames_total` are present
  and `br_thread_collect_age_seconds` is below 30.

## 8. Docs

- New `docs/metrics.md`: what is exported, a Prometheus scrape config example (one job, several boards as targets),
  the catalogue tables, freshness and partial rules, example alerts (`br_mode{mode="RADIO_FAULT"} == 1`,
  `absent(br_thread_role)`, low `br_heap_min_free_bytes`).
- `docs/api.md`: one row for `GET /metrics`. `docs/README.md` and the README documentation line link the new page.
- `CHANGELOG.md`: an Unreleased entry.

## 9. Constraints

- Follow `STYLEGUIDE.md`; `make lint` and `make test` stay green.
- No change to host networking or to Home Assistant.
- Hardware checks are batched, never part of a task.

## 10. Follow-ups

- NAT64 translator figures (`br_nat64_state`, `br_nat64_packets_total`, `br_nat64_bytes_total`,
  `br_nat64_errors_total`) once the translator is enabled in the OpenThread build.
- Per-neighbour series (RSSI, link quality, frame error rate by extended address), behind an opt-in, because label
  cardinality grows with the mesh.
- mDNS advertisement (`_prometheus-http._tcp`) for service discovery.
- A Grafana dashboard JSON in `docs/`.
