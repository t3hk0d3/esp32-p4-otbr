# Prometheus metrics

`GET http://<board>/metrics` serves device health and Thread network figures in the Prometheus text format 0.0.4. It
answers in every mode, without authentication, like the rest of the API. In split mode it answers only on the
management side, as `/device/*` does.

## Scrape config

One job covers every board:

```yaml
scrape_configs:
  - job_name: thread-border-routers
    scrape_interval: 30s
    static_configs:
      - targets: ["br-kitchen.lan", "br-garage.lan", "192.168.1.117"]
```

The path is the Prometheus default, so no `metrics_path` is needed.

## How the figures are read

- Device figures come from the diagnostics sampler, which runs once a second, and from the board's status. A scrape
  never takes the OpenThread lock.
- Thread figures come from a collector that reads OpenThread every 10 s in `BR_RUNNING`. It waits at most 10 ms for the
  OpenThread lock: when the lock is busy, the collection is skipped (`br_thread_collect_failures_total` goes up) and the
  previous figures are kept.
- Thread series are only served while the collected figures are younger than 30 s. In `STARTING`, `SNIFFER` and
  `RADIO_FAULT`, or after three missed collections, they are left out, so a dashboard shows a gap instead of frozen
  values. `br_thread_collect_age_seconds` tells how old they are.
- If the diagnostics, the collector or the routing task are not running or are busy, their series are left out and
  `br_scrape_partial` is 1. The scrape still answers 200.
- Routing figures come from the routing task's last status (at most about 100 ms old), with no lock.
- Counters from OpenThread start again from 0 when the Thread stack restarts (sniffer exit, radio fault recovery,
  radio firmware update). Prometheus treats that as a counter reset, so `rate()` and `increase()` stay correct.
- Enumerations are state sets: one series per state, 1 for the current one and 0 for the others.

## Device metrics

| Metric | Type | Labels |
| --- | --- | --- |
| `br_info` (value 1) | gauge | `version`, `slot`, `rcp_version` (`""` when unknown), `rcp_compatible` |
| `br_uptime_seconds` | gauge | |
| `br_reset_reason` (value 1) | gauge | `reason` |
| `br_mode` | state set | `mode` = STARTING, BR_RUNNING, SNIFFER, RADIO_FAULT |
| `br_cpu_seconds_total` | counter | `core` = 0/1, `mode` = busy/idle |
| `br_task_cpu_seconds_total` | counter | `task` |
| `br_task_stack_free_min_bytes` | gauge | `task` |
| `br_tasks_untracked` | gauge | |
| `br_heap_free_bytes`, `br_heap_min_free_bytes`, `br_heap_largest_free_block_bytes` | gauge | `region` = internal/psram |
| `br_ot_lock_acquisitions_total`, `br_ot_lock_hold_seconds_total`, `br_ot_lock_wait_seconds_total` | counter | |
| `br_ot_lock_hold_max_seconds`, `br_ot_lock_wait_max_seconds` | gauge | `window` = boot/60s |
| `br_storage_ok`, `br_sniffer_active`, `br_eth_up`, `br_network_trial_pending` | gauge 0/1 | |
| `br_sniffer_frames_total`, `br_sniffer_drops_total` | counter (current or last capture) | |
| `br_scrape_partial` | gauge 0/1 | |

Task names are FreeRTOS names. When two tasks share a name, the second one is labelled `<name>#2`.

## Thread metrics

| Metric | Type | Labels |
| --- | --- | --- |
| `br_thread_role` | state set | `role` = disabled, detached, child, router, leader |
| `br_thread_role_entered_total` | counter | `role` |
| `br_thread_partition_changes_total`, `br_thread_attach_attempts_total` | counter | |
| `br_thread_routers`, `br_thread_children`, `br_thread_neighbors` | gauge | |
| `br_mac_tx_frames_total`, `br_mac_tx_retries_total`, `br_mac_rx_frames_total`, `br_mac_rx_duplicated_total` | counter | |
| `br_mac_tx_errors_total` | counter | `reason` = cca, abort, busy_channel |
| `br_mac_rx_errors_total` | counter | `reason` = fcs, security, no_frame, unknown_neighbor, invalid_src, other |
| `br_thread_ip6_packets_total` | counter | `direction` = tx/rx, `result` = success/failure |
| `br_border_routing_state` | state set | `state` = uninitialized, disabled, stopped, running |
| `br_dhcp6_pd_state` | state set | `state` = disabled, stopped, running, idle |
| `br_border_routing_packets_total`, `br_border_routing_bytes_total` | counter | `direction` = inbound/outbound, `cast` = unicast/multicast |
| `br_border_routing_ra_total`, `br_border_routing_rs_total` | counter | `event` = rx, tx_success, tx_failure |
| `br_srp_server_state` | state set | `state` = disabled, running, stopped |
| `br_srp_hosts`, `br_srp_services` | gauge | |
| `br_trel_enabled`, `br_trel_active` | gauge | |
| `br_trel_peers`, `br_trel_peers_other` | gauge | TREL peers of this Thread network, and of other networks on the same link |
| `br_trel_packets_total`, `br_trel_bytes_total` | counter | `direction` = tx, rx |
| `br_trel_tx_failures_total` | counter | |
| `br_thread_collect_age_seconds` | gauge | |
| `br_thread_collect_failures_total` | counter | |

NAT64 translation counters are not exported. NAT64 itself works (verified in the bring-up), but the translation does not
run in OpenThread's own NAT64 translator, which these counters belong to: it is not in the linked libraries
(`otNat64GetCounters` is absent).

## Routing metrics

See [Routing to the mesh](networking.md#routing-to-the-mesh).

| Metric | Type | Labels |
| --- | --- | --- |
| `br_routing_method` | state set | `method` = pd, ripng, bgp, off |
| `br_routing_announced_prefixes` | gauge | |
| `br_routing_omr_custom` | gauge 0/1 | |
| `br_bgp_peer_state` | state set | `peer`, `state` = idle, connect, active, opensent, openconfirm, established |
| `br_bgp_peer_uptime_seconds` | gauge (0 when down) | `peer` |
| `br_bgp_received_dropped_total` | counter | `peer` |
| `br_bgp_session_resets_total` | counter (drops from Established) | `peer` |
| `br_ripng_updates_sent_total`, `br_ripng_requests_answered_total`, `br_ripng_responses_dropped_total` | counter | |

The `br_bgp_*` series are served only with method `bgp`, one per configured peer (`peer` is its address, RFC 5952
text); the `br_ripng_*` series only with method `ripng`. `br_ripng_responses_dropped_total` also counts invalid
packets. The counters start again from 0 when the board reboots. A peer's `br_bgp_*` counters also start again when its
session is set up anew: the peer or its ASN changed, or the local ASN, hold time or router ID changed.

## Zigbee metrics

Served only while a radio is in Zigbee mode, see [Zigbee coordinator](zigbee.md).

| Metric | Type | Labels |
| --- | --- | --- |
| `zigbee_frames_total` | counter | `dir` = rx, tx |
| `zigbee_tx_results_total` | counter | `result` = no_ack, cca, failed |
| `zigbee_rcp_resets_total` | counter | |
| `zigbee_heap_bytes` | gauge | `kind` = current, peak |
| `zigbee_client_connected` | gauge 0/1 | |

`zigbee_tx_results_total` counts failed transmits only; successful ones are the difference to
`zigbee_frames_total{dir="tx"}` less the failures that never reached the radio, so it is not exact. The counters start
again from 0 when the board reboots.

## Example alerts

```yaml
groups:
  - name: thread-border-routers
    rules:
      - alert: BorderRouterRadioFault
        expr: br_mode{mode="RADIO_FAULT"} == 1
        for: 2m
      - alert: BorderRouterNoThreadFigures
        expr: br_mode{mode="BR_RUNNING"} == 1 unless on(instance) br_thread_role
        for: 5m
      - alert: BorderRouterLowMemory
        expr: br_heap_min_free_bytes{region="internal"} < 20000
      - alert: BorderRouterBgpDown
        expr: br_bgp_peer_state{state="established"} == 0
        for: 5m
```

## Useful queries

- CPU load per core: `rate(br_cpu_seconds_total{mode="busy"}[5m])`.
- Share of time the OpenThread lock is held: `rate(br_ot_lock_hold_seconds_total[5m])`.
- 802.15.4 retry ratio: `rate(br_mac_tx_retries_total[5m]) / rate(br_mac_tx_frames_total[5m])`.
