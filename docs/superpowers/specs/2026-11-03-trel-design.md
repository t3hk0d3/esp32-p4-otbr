# TREL (Thread Radio Encapsulation Link): design

Date: 2026-11-03. Status: approved 2026-11-03, implemented (see `docs/superpowers/plans/2026-11-03-trel.md`).

## 1. Purpose and scope

Let the border router form Thread mesh links over Ethernet with other TREL-capable border routers of the same Thread
network on the backbone link, next to the C6 15.4 radio (OpenThread multi-radio).

In scope: build config, a persisted on/off switch carried by the netcfg config, live apply, isolation of the TREL port
from `mgmt`, status and metrics, an Ethernet page card, docs.

Out of scope: TREL-only mode (no 15.4), OpenThread-managed DNS-SD (`OPENTHREAD_CONFIG_TREL_MANAGE_DNSSD_ENABLE`), TREL on
`mgmt`, peers across routed links.

## 2. Decisions

| Topic | Decision |
| --- | --- |
| Build | `CONFIG_OPENTHREAD_RADIO_TREL=y`, port 12390 (default), `CONFIG_OPENTHREAD_TREL_BUFFER_SIZE=20` |
| Interface | the backbone netif: `thread_bb` in split mode, the single port otherwise (automatic: mDNS runs only there) |
| Default | on |
| API | no new endpoint: `"trel": true\|false` at the top level of `GET/PUT /device/network` |
| PUT, only `trel` differs from the running config | applied live, stored, no trial, no reboot: `200 {"status":"applied"}`; `{"status":"saved"}` when the OpenThread lock was not free within 1 s (applied at the next OpenThread start) |
| PUT, anything else differs | the existing trial flow; `trel` rides along |
| PUT while a trial is pending, only `trel` differs | `409 busy` "confirm or revert the network change first" |
| PUT body without `trel` | keeps the active value |
| Stored config without `trel` (older firmware) | parses as `true` |
| Storage | part of the netcfg `active` JSON in NVS; written only when the config changes (one write per click) |
| Live state | `/device/status` `trel: {"enabled","active","peers","rx_packets","tx_packets","tx_failures"}`; plus `peer_list` (at most 8: ext address, socket address) |
| Metrics | `br_trel_enabled`, `br_trel_active`, `br_trel_peers`, `br_trel_packets_total{direction}`, `br_trel_tx_failures_total` |
| Isolation | split mode: UDP to port 12390 arriving on `mgmt` is dropped |
| Modes | the switch can be changed in every mode; it takes effect at the next Thread bring-up when not `BR_RUNNING` |
| Access | like every `/device/*` write: management side only |

## 3. Architecture

- `netcfg`: `netcfg_config_t` gets `bool trel`; JSON parse/print, `netcfg_equal`, and a pure
  `netcfg_diff_only_trel(a, b)`. New runtime calls `netcfg_set_trel(bool)` (rewrites `active`, refuses during a trial)
  and `netcfg_trel_enabled()`. Isolation: pure `netcfg_ip6_udp_drop(input_is_mgmt, dport)`, applied in
  `__wrap_lwip_hook_ip6_input` when the next header is UDP.
- `br_core`: `br_core_set_trel(bool)` calls `otTrelSetEnabled` under the OpenThread lock in `BR_RUNNING`, otherwise
  remembers the value; bring-up applies `netcfg_trel_enabled()` before Thread is enabled. `br_core_get_trel(*)` fills
  the status snapshot (`otTrelIsEnabled`, `otTrelGetNumberOfPeers`, `otTrelGetCounters`, `otTrelGetNextPeer`) with a
  bounded lock wait, like the existing Thread reads. `EVENTFD_COUNT` 4 -> 5.
- `device_api`: `network_put_handler` branches on `netcfg_diff_only_trel`; `network_get_handler` and the status JSON carry
  the new fields.
- `metrics`: the collector copies the TREL counters with the other Thread figures; the renderer adds the families,
  also `br_trel_bytes_total{direction}`.
- Web UI: the "Thread over Ethernet" card on `ethernet.html` (mock). The Off/On control PUTs the active config with only
  `trel` flipped; a combined edit shows `trel` in the review table.

## 4. Risks

- Mode changes: SNIFFER/RADIO_FAULT stop Thread; `otPlatTrelDisable` must run and re-enable must work (the IDF port
  refuses a second enable). Tested on the board.
- mDNS reflector between VLANs can surface peers whose addresses are not on `thread_bb`; sends to them fail and are
  counted in `tx_failures`.
- The IDF port latches the TREL netif from the first browse result: correct while only one netif runs mDNS.
- Kconfig: the TREL menu depends on `EXAMPLE_CONNECT_ETHERNET`, satisfied through `protocol_examples_common`. A build
  check guards it (step 1).
