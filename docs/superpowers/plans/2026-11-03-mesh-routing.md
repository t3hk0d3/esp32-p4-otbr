# Mesh routing (custom OMR prefix, RIPng, BGP) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** let the owner set the OMR prefix (auto or one custom /64, optionally with the self-generated ULA), and tell
the core router how to reach the mesh by exactly one method: DHCPv6-PD, RIPng, BGP or off.

**Architecture:** a new component `p4/components/routing/`. Pure, host-tested modules hold all the logic: settings,
the announcement set, the BGP codec and state machine, the RIPng codec and scheduler, and the prefix publisher behind
an OpenThread ops seam. A single target-only task owns the sockets, polls OpenThread every 500 ms and drives
whichever sender the method selects. `device_api`, `metrics`, the console and a new `routing.html` page sit on top.

**Tech Stack:** ESP-IDF 6.1 (C, FreeRTOS, lwIP BSD sockets, NVS, cJSON), OpenThread border routing API, Unity host
tests on the `linux` target, plain JS page tested under node, pytest-embedded with Docker macvlan containers (FRR,
BIRD) for the deferred on-board pass, scapy for test vectors.

**Spec:** `docs/superpowers/specs/2026-11-03-mesh-routing-design.md`, with the agreed page layout in
`docs/superpowers/specs/2026-11-03-routing-page-mockup.html`. Read both before starting a task.

**Styleguide:** `STYLEGUIDE.md`, enforced by `make lint`:
- astyle (ESP-IDF rules) for C, ruff for Python;
- `snake_case`; public symbols prefixed with the component name (new: `routing_`); one `TAG` per file;
- `esp_err_t` returns with `ESP_RETURN_ON_ERROR` / `ESP_GOTO_ON_ERROR`;
- host tests in `<component>/test_host/` named `TEST_CASE("<scenario>: <effect>", "[routing]")`;
- target tests in `p4/pytest/test_<feature>.py`;
- every routine task through a Makefile target;
- ops structs only where a host test needs them (here: the publisher's OpenThread seam);
- changes to vendored `esp_ot_br_server` are listed in `p4/components/PATCHES.md`.

## Global Constraints

- One active method: `pd` (default), `ripng`, `bgp`, `off`. DHCPv6-PD runs only with `pd`.
- OMR: `auto` (default) or `custom` (one /64, preference medium). The extra ULA next to a custom prefix: SLAAC on-mesh,
  preference low.
- Announced (RIPng, BGP only): custom prefix if set, ULA if the box is ticked, each only while in Network Data.
  Nothing while the border router is not running (`SNIFFER`, `RADIO_FAULT`, Thread disabled or detached).
- Prefixes stay on the mesh whatever a session or RIPng does.
- Backbone only (`netcfg_backbone_netif()`); nothing on `mgmt` in split mode.
- Settings apply live (no reboot, no trial) and live in NVS namespace `routing`, key `settings`, as JSON text. BGP
  settings are kept while another method is active.
- BGP: at most 2 peers; IPv6 TCP port 179, both directions; hold time default 90, allowed 0 or 3..65535; keepalive =
  hold / 3; backoff 5 s doubling to 120 s; max message 4096 bytes; capabilities Multiprotocol AFI 2 / SAFI 1 and
  4-octet ASN; AS_TRANS 23456; LOCAL_PREF 100 on iBGP only; next-hop global + link-local (32 bytes), link-local only
  (16 bytes) without a global address.
- RIPng: UDP 521 both ends, to `ff02::9`, from the backbone link-local address, hop limit 255; RTE length 64, tag 0,
  metric 1; regular update every 30 s + random 0..5 s; triggered updates at most every 5 s; withdrawn prefixes sent
  with metric 16 in the triggered update and the next 4 regular updates; stop sends metric 16 once.
- Withdrawal on a border-router mode change within 1 s.
- Inbound routes are counted and dropped, never installed.
- No flashing inside tasks. On-board checks run only in the batched hardware pass after review and fixes.
- Long commands through `logrun` (for example `logrun make test-host`).
- Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Decisions this plan makes (the spec left them to the builder)

- **State changes:** the routing task polls OpenThread every 500 ms, taking the lock with a 10 ms timeout (as the
  metrics collector does); a busy lock skips that tick. That meets the 1 s withdrawal.
- **`::/0` published by us:** iterate Network Data external routes (`otNetDataGetNextRoute`) for `::/0` with our
  RLOC16.
- **Prefix in Network Data:** `otNetDataGetNextOnMeshPrefix` over the leader Network Data.
- **Socket scope:** every routing socket is bound to the backbone netif with `SO_BINDTODEVICE` (name from
  `esp_netif_get_netif_impl_name`). An accepted TCP connection whose local address does not belong to the backbone
  netif is closed as a second guard. Link-local peers use the backbone netif's zone.
- **br_core hook:** br_core stops enabling PD itself and calls a callback registered with
  `br_core_set_border_router_up_cb(void (*cb)(otInstance *))`, with the OpenThread lock held, at the point where it
  enabled PD today. `app_main` registers `routing_on_border_router_up`. That avoids a br_core → routing dependency.
- **Task:** `routing`, core 1, priority `tskIDLE_PRIORITY + 2`, stack 6144 bytes. Static per-peer 4096-byte receive
  buffers.
- **Sockets:** `CONFIG_LWIP_MAX_SOCKETS` 10 → 16 in `p4/sdkconfig.defaults`.
- **BGP timers not fixed by the spec:** connect timeout 10 s; OpenSent hold timer 240 s (RFC 4271's suggested large
  value).
- **HTTP slots:** `EXTRA_HANDLER_SLOTS` 16 → 20 (master has 13 routes plus the catch-all; the topology branch adds 2;
  this adds 2).
- **Test vectors:** `tools/routing_vectors.py` builds the BGP and RIPng reference messages with scapy
  (`scapy.contrib.bgp`, `scapy.layers.inet6` RIPng) and writes `p4/components/routing/test_host/main/vectors.h`. The
  generated header is committed, and the script is kept for regeneration (`make routing-vectors`). scapy is an
  independent implementation, which is what the spec's "capture from FRR or BIRD" was for.

## Review Focus

Inputs the spec implies but its skeleton does not test, most likely to bite first. Each one has a test in the task
that owns the code (marked (+) in the skeleton below):

1. **A custom prefix that is already on-link on the backbone** (for example the LAN's own `2a06:9801:c7:aa01::/64`).
   It would put the same /64 on two links. Expect a 400 naming `omr.prefix` (Task 1, checked against the backbone
   prefixes passed in).
2. **Stored settings that do not parse** (written by another firmware version, or corrupted). Expect defaults, so
   the board boots with today's behavior (method `pd`) and logs a warning (Task 1).
3. **A peer that negotiates hold time 0.** Expect no keepalives sent, no hold timer, and the session staying up
   (Task 4).
4. **The router dropping the TCP connection in Established** (router reboot). Expect Idle, then a retry after 5 s,
   not after an earlier, larger backoff (Task 4).
5. **A BGP peer address that is multicast, unspecified, or one of the board's own addresses.** Expect a 400 naming
   `bgp.peers` (Task 1, own addresses passed in).

## File structure

```
p4/components/routing/
  CMakeLists.txt            linux target: pure files only; target: all
  include/routing.h         public: start, apply, state JSON, metrics view, br-up callback
  routing_types.h           shared plain types (settings, prefix sets, status)
  settings.c / settings.h   defaults, JSON parse/validate/render, diff (pure); NVS load/store (target, #ifdef)
  announce.c / announce.h   announcement set and diff (pure)
  bgp_msg.c / bgp_msg.h     BGP codec (pure)
  bgp_fsm.c / bgp_fsm.h     BGP session state machine (pure)
  ripng.c / ripng.h         RIPng codec and scheduler (pure)
  publisher.c / publisher.h prefix publisher over routing_ot_ops_t (pure logic)
  ot_ops.c                  real routing_ot_ops_t on OpenThread (target)
  task.c                    routing task: sockets, polling, method switching (target)
  console.c                 `routing show` (target)
  test_host/                Unity project: test_settings.c, test_announce.c, test_bgp_msg.c, test_bgp_fsm.c,
                            test_ripng.c, test_publisher.c, fake_ops.c/.h, vectors.h
tools/routing_vectors.py    scapy test-vector generator
p4/components/esp_ot_br_server/frontend/routing.html, static/routing.js
tools/tests/test_routing_page.py
p4/pytest/test_routing.py
```

Modified: `br_core` (hook instead of PD enable), `device_api` (routes, status), `metrics` (series), `esp_br_web.c`
(slots), `static/api.js` (nav), `p4/main/app_main.c`, `p4/main/console.c`, `p4/main/CMakeLists.txt`,
`p4/sdkconfig.defaults`, `Makefile`, docs.

## Shared types (`routing_types.h`, the contract between tasks)

- `ROUTING_MAX_PEERS 2`, `ROUTING_MAX_ANNOUNCED 2`.
- `routing_method_t`: `ROUTING_METHOD_PD`, `ROUTING_METHOD_RIPNG`, `ROUTING_METHOD_BGP`, `ROUTING_METHOD_OFF`.
- `routing_omr_mode_t`: `ROUTING_OMR_AUTO`, `ROUTING_OMR_CUSTOM`.
- `routing_prefix64_t { uint8_t b[8]; }`: a /64 by its first 8 bytes.
- `routing_prefix_set_t { routing_prefix64_t p[ROUTING_MAX_ANNOUNCED]; uint8_t count; }`.
- `routing_bgp_peer_t { uint8_t addr[16]; uint32_t asn; }` (link-local when `addr` is in `fe80::/10`).
- `routing_settings_t`:
  - `method`, `omr_mode`, `omr_prefix`, `announce_ula`;
  - `local_asn`, `router_id_set`, `router_id` (host order IPv4), `hold_time`;
  - `peer_count`, `peers[ROUTING_MAX_PEERS]`.
- `routing_bgp_state_t`: `ROUTING_BGP_IDLE`, `_CONNECT`, `_ACTIVE`, `_OPENSENT`, `_OPENCONFIRM`, `_ESTABLISHED`.
- `routing_status_t`:
  - `omr_prefix`, `ula_prefix` (each with a `_known` flag);
  - `announced` (`routing_prefix_set_t`);
  - `bgp_router_id`, `bgp_blocked` (`const char *`, NULL when not blocked);
  - `peers[]` with `state`, `uptime_s`, `last_error[48]`, `received_dropped`, `resets`;
  - `ripng`: `last_update_ms_ago` (`int64_t`, -1 never), `updates_sent`, `requests_answered`, `responses_dropped`.
- `routing_metrics_view_t`: method, `announced_count`, `omr_custom`, the BGP peer figures and the RIPng counters.

## BDD skeleton

The contract is spec section 7, reproduced here grouped by task, with this plan's additions marked (+). One host test
per effect, named `"<scenario>: <effect>"`.

```
routing_settings (Task 1)
    routing_settings_defaults
        Always
            - method pd, omr auto, announce_ula false, no peers, hold time 90, router id unset
    routing_settings_from_json
        Valid body
            - fills every field
        Body without "bgp"
            - keeps the current BGP settings
        Unknown method, or omr.mode not auto/custom
            - returns ESP_ERR_INVALID_ARG with bad_field "method" / "omr.mode"
        Custom mode without a prefix, or a prefix that is not a /64, link-local or multicast
            - returns ESP_ERR_INVALID_ARG with bad_field "omr.prefix"
        (+) Custom prefix equal to a backbone on-link prefix
            - returns ESP_ERR_INVALID_ARG with bad_field "omr.prefix"
        ASN 0, 23456 or above 4294967295
            - bad_field "bgp.local_asn" / "bgp.peers"
        router_id 0.0.0.0 or not dotted IPv4
            - bad_field "bgp.router_id"
        hold_time 1, 2 or above 65535
            - bad_field "bgp.hold_time"
        More than 2 peers, or duplicate peer addresses
            - bad_field "bgp.peers"
        (+) Peer address multicast, unspecified, or one of the board's own addresses
            - bad_field "bgp.peers"
        Method bgp with no peers
            - bad_field "bgp.peers"
        Text that is not JSON, or trailing text
            - returns ESP_ERR_INVALID_ARG with bad_field "body"
    routing_settings_to_json
        Settings and status
            - renders the GET shape of spec section 6; status.bgp null unless bgp, status.ripng null unless ripng
        Round trip
            - from_json(to_json(s)) equals s
    routing_settings_diff
        Only omr or announce_ula changed
            - reports PREFIXES only (no session reset)
        Peers, ASNs, router id or hold time changed
            - reports BGP_SESSIONS
        Method changed
            - reports METHOD
    routing_settings_load (target)
        Nothing stored
            - returns defaults
        (+) Stored text does not parse
            - returns defaults and logs a warning
        Stored settings
            - returns them unchanged after a reboot

routing_announce (Task 2)
    routing_announce_compute
        Custom OMR, ULA box ticked, both in Network Data, border router running and attached
            - returns the custom prefix and the ULA
        Custom OMR, ULA box clear
            - returns the custom prefix only
        Auto OMR, ULA box ticked, ULA in Network Data
            - returns the ULA
        Auto OMR, ULA box clear
            - returns an empty set
        A selected prefix is not in Network Data
            - leaves that prefix out
        Border router not running or Thread detached
            - returns an empty set
        Method pd or off
            - returns an empty set
        Custom prefix equal to the ULA
            - returns it once
    routing_announce_diff
        Set changed
            - returns the added and removed prefixes separately
        Set unchanged
            - returns empty added and removed sets

routing_bgp_msg (Task 3): spec section 7 "routing_bgp_msg (codec)" unchanged, every encoder compared byte for byte
with the scapy vectors; plus
    routing_bgp_decode_notification
        NOTIFICATION 6/2
            - returns code 6, subcode 2
    encoders
        Buffer too small
            - return 0 and write nothing past cap

routing_bgp_fsm (Task 4): spec section 7 "routing_bgp_fsm (session state machine)" unchanged; plus
    open received
        (+) Peer offers hold time 0, own 90
            - negotiates 0; no keepalive deadline and no hold deadline afterwards
    connection lost
        (+) TCP reset in Established after the backoff had grown to 40 s earlier
            - moves to Idle, records last_error, schedules a retry after 5 s
    connect timeout
        No connection after 10 s
            - same effects as connect failed
    uptime and counters
        Established for 125 s
            - uptime_s is 125; resets counts every drop from Established

routing_ripng (Task 5): spec section 7 "routing_ripng (codec and scheduler)" unchanged, encoders compared with the
scapy vectors; plus
    decode
        Datagram truncated mid-RTE (length not 4 + 20 n)
            - is dropped and counted
        Request with source port other than 521
            - is answered to that port

routing_publisher (Task 6): spec section 7 "routing_publisher (mesh side)" unchanged; plus
    apply
        Same settings applied twice
            - makes no OpenThread calls the second time
    prefix_state
        After apply
            - reports ula, ula_known, and whether the custom prefix and the ULA are in Network Data

routing task, API, page and board (Tasks 7-10): spec section 7 "routing_settings put" and "routing task (target)"
unchanged; their effects are checked by the device_api and node tests and by p4/pytest/test_routing.py.
```

## Tasks

### Task 1: Component skeleton and settings

**Files:**
- Create: `p4/components/routing/{CMakeLists.txt, include/routing.h, routing_types.h, settings.c, settings.h}`
- Create: `p4/components/routing/test_host/{CMakeLists.txt, main/CMakeLists.txt, main/test_settings.c}`

**Interfaces:**
- Produces:
  - `routing_types.h` as above;
  - `void routing_settings_defaults(routing_settings_t *out)`;
  - `esp_err_t routing_settings_from_json(const char *json, const routing_settings_t *current, const routing_validate_ctx_t *ctx, routing_settings_t *out, const char **bad_field)`. `routing_validate_ctx_t` holds the backbone on-link prefixes (up to 4 `routing_prefix64_t`) and the board's own addresses (up to 12 × 16 bytes);
  - `char *routing_settings_to_json(const routing_settings_t *s, const routing_status_t *status)` (malloc'd; caller frees);
  - `uint32_t routing_settings_diff(const routing_settings_t *a, const routing_settings_t *b)` returning flags `ROUTING_CHANGE_METHOD | ROUTING_CHANGE_PREFIXES | ROUTING_CHANGE_BGP_SESSIONS`;
  - target only: `esp_err_t routing_settings_load(routing_settings_t *out)` and `esp_err_t routing_settings_store(const routing_settings_t *s)`.

- [ ] **Step 1:** Create the component and the host-test project, following `p4/components/metrics` (linux target:
  pure files only). Write `test_settings.c` with one `TEST_CASE` per effect of the routing_settings subtree above.
  Assertions use the spec's values: hold 90 default; ASN 23456 rejected; `"bgp.peers"` for 3 peers; `omr.prefix`
  rejected for `fe80::/64`, `ff02::/64`, `2001:db8::/48`, and for `2a06:9801:c7:aa01::/64` when it is in
  `ctx`. The round trip compares with `memcmp` over a fully populated struct.
- [ ] **Step 2:** `logrun make test-host`. Expected: the routing project fails to link (functions missing).
- [ ] **Step 3:** Implement `settings.c` with cJSON, like `netcfg/config.c` (`cJSON_ParseWithOpts(..., true)`,
  bad-field reporting). `load`/`store` sit under `#ifndef CONFIG_IDF_TARGET_LINUX` (or the linux `CMakeLists` branch
  leaves them out) and use NVS like `netcfg/netcfg.c`; a parse failure on load logs `ESP_LOGW` and falls back to
  defaults.
- [ ] **Step 4:** `logrun make test-host`. Expected: the routing suite passes, the other suites still pass.
- [ ] **Step 5:** Commit `feat(routing): settings model, JSON and validation`.

### Task 2: Announcement set

**Files:** Create `p4/components/routing/announce.{c,h}`, `test_host/main/test_announce.c`.

**Interfaces:**
- Consumes: `routing_types.h`.
- Produces:
  - `routing_announce_input_t { routing_method_t method; routing_omr_mode_t omr_mode; routing_prefix64_t custom; bool announce_ula; bool ula_known; routing_prefix64_t ula; bool custom_in_netdata; bool ula_in_netdata; bool br_up; }`;
  - `void routing_announce_compute(const routing_announce_input_t *in, routing_prefix_set_t *out)`;
  - `void routing_announce_diff(const routing_prefix_set_t *old_set, const routing_prefix_set_t *new_set, routing_prefix_set_t *added, routing_prefix_set_t *removed)`.

- [ ] **Step 1:** Write a test per effect of the routing_announce subtree.
- [ ] **Step 2:** `logrun make test-host`; expect a link failure.
- [ ] **Step 3:** Implement; set order is custom first, then ULA.
- [ ] **Step 4:** `logrun make test-host`; expect pass.
- [ ] **Step 5:** Commit `feat(routing): announcement set`.

### Task 3: BGP message codec and test vectors

**Files:** Create `tools/routing_vectors.py`, `p4/components/routing/bgp_msg.{c,h}`,
`test_host/main/{vectors.h,test_bgp_msg.c}`; modify `Makefile` (target `routing-vectors` with a `## ` help line).

**Interfaces:**
- Consumes: `routing_prefix_set_t`.
- Produces:
  - constants `ROUTING_BGP_HDR_LEN 19` and `ROUTING_BGP_MAX_MSG 4096`, and message types OPEN 1, UPDATE 2,
    NOTIFICATION 3, KEEPALIVE 4;
  - `routing_bgp_err_t { uint8_t code; uint8_t subcode; }`, where code 0 means OK;
  - `routing_bgp_nexthop_t { uint8_t global[16]; bool has_global; uint8_t link_local[16]; }`;
  - `routing_bgp_open_t { uint32_t asn; uint32_t router_id; uint16_t hold_time; bool ipv6_unicast; }`;
  - encoders, each returning the bytes written, or 0 when `cap` is too small:
    - `size_t routing_bgp_encode_open(uint8_t *buf, size_t cap, uint32_t local_asn, uint16_t hold_time, uint32_t router_id)`
    - `size_t routing_bgp_encode_keepalive(uint8_t *buf, size_t cap)`
    - `size_t routing_bgp_encode_update(uint8_t *buf, size_t cap, bool ibgp, uint32_t local_asn, const routing_bgp_nexthop_t *nh, const routing_prefix_set_t *announce)`
    - `size_t routing_bgp_encode_withdraw(uint8_t *buf, size_t cap, const routing_prefix_set_t *withdraw)`
    - `size_t routing_bgp_encode_notification(uint8_t *buf, size_t cap, uint8_t code, uint8_t subcode)`
  - decoders:
    - `routing_bgp_err_t routing_bgp_decode_header(const uint8_t *buf, size_t len, uint8_t *type, uint16_t *msg_len)`
    - `routing_bgp_err_t routing_bgp_decode_open(const uint8_t *msg, size_t len, uint32_t expected_asn, routing_bgp_open_t *out)`
    - `routing_bgp_err_t routing_bgp_count_update_nlri(const uint8_t *msg, size_t len, uint32_t *count)`
    - `routing_bgp_err_t routing_bgp_decode_notification(const uint8_t *msg, size_t len, uint8_t *code, uint8_t *subcode)`

- [ ] **Step 1:** Write `tools/routing_vectors.py`. It runs with `uv run --with scapy`, and `make routing-vectors`
  calls it. It writes `vectors.h` with named `static const uint8_t` arrays:
  - OPEN for ASN 4200000001 (My AS 23456) and for ASN 65010, hold 90, router ID 192.168.1.117;
  - KEEPALIVE;
  - UPDATE for iBGP and eBGP announcing `2001:db8:42:100::/64` and `fd98:2656:4740:1::/64` with next-hop
    `2001:db8:42::17` + `fe80::82f1:b2ff:fed2:cafc`, plus a link-local-only variant;
  - an MP_UNREACH withdraw of the same two prefixes;
  - NOTIFICATION 6/2;
  - a received UPDATE carrying 3 IPv6 NLRI and 2 IPv4 NLRI;
  - malformed cases: a bad marker, length 18, length 4097, an attribute length past the end, and prefix length 129.

  Run `make routing-vectors` and commit the generated header with the script.
- [ ] **Step 2:** Write `test_bgp_msg.c`: each encoder output is `TEST_ASSERT_EQUAL_HEX8_ARRAY` against its vector;
  each decoder gets the case and expected error code/subcode from the routing_bgp_msg subtree (1/1, 1/2, 3/1, 3/10,
  2/2, 2/6, 2/7). The count on the received UPDATE is 5.
- [ ] **Step 3:** `logrun make test-host`; expect a link failure.
- [ ] **Step 4:** Implement `bgp_msg.c`. The UPDATE walker reads withdrawn length, attribute length and each
  attribute's flags, type and length (extended length bit). It counts prefixes in MP_REACH_NLRI (AFI 2 / SAFI 1, after
  the next-hop and the reserved byte) and in the trailing IPv4 NLRI, and checks every length against the message end.
- [ ] **Step 5:** `logrun make test-host`; expect pass.
- [ ] **Step 6:** Commit `feat(routing): BGP message codec with scapy vectors`.

### Task 4: BGP session state machine

**Files:** Create `p4/components/routing/bgp_fsm.{c,h}`, `test_host/main/test_bgp_fsm.c`.

**Interfaces:**
- Consumes: `routing_bgp_open_t`, `routing_bgp_err_t`, `routing_bgp_state_t`.
- Produces:
  - `routing_bgp_fsm_config_t { uint32_t local_asn; uint32_t peer_asn; uint32_t local_router_id; uint16_t hold_time; }`;
  - events `routing_bgp_event_t`, a tagged union of:
    - `START`
    - `STOP {uint8_t subcode}`
    - `CONNECTED {bool incoming}`
    - `CONNECT_FAILED {const char *why}`
    - `CONN_LOST`
    - `OPEN_RX {routing_bgp_open_t open}`
    - `OPEN_BAD {routing_bgp_err_t err}`
    - `KEEPALIVE_RX`
    - `UPDATE_RX {uint32_t nlri}`
    - `NOTIFICATION_RX {uint8_t code, subcode}`
    - `INCOMING_WHILE_BUSY`
    - `ANNOUNCE_CHANGED`
    - `TICK`
  - actions `routing_bgp_actions_t`: up to 4 items of `CONNECT`, `SEND_OPEN`, `SEND_KEEPALIVE`,
    `SEND_NOTIFICATION {code, subcode}`, `SEND_FULL_SET`, `SEND_DIFF`, `CLOSE`, `ADOPT_INCOMING`, `REJECT_INCOMING`;
  - `void routing_bgp_fsm_init(routing_bgp_fsm_t *f, const routing_bgp_fsm_config_t *cfg)`;
  - `void routing_bgp_fsm_handle(routing_bgp_fsm_t *f, const routing_bgp_event_t *ev, uint32_t now_ms, routing_bgp_actions_t *out)`;
  - `uint32_t routing_bgp_fsm_next_deadline_ms(const routing_bgp_fsm_t *f)` (`UINT32_MAX` when none);
  - accessors for state, uptime, `last_error`, `received_dropped`, `resets`.

Collision handling, which the tests and RFC 4271 section 6.8 fix. As pseudocode:

```
on INCOMING_WHILE_BUSY:
    if state in {Connect, Active}: ADOPT_INCOMING (drop the outgoing attempt), send OPEN, -> OpenSent
    if state in {OpenSent, OpenConfirm}: remember "pending incoming"; the task reads its OPEN and sends OPEN_RX
        tagged incoming; then
        if local_router_id > peer_router_id: REJECT_INCOMING with NOTIFICATION 6/7
        else: SEND_NOTIFICATION 6/7 + CLOSE on the outgoing, ADOPT_INCOMING, continue from OpenSent on it
    if state == Established: REJECT_INCOMING with NOTIFICATION 6/7
```

- [ ] **Step 1:** Write a test per effect of the routing_bgp_fsm subtree, driving events with explicit `now_ms` and
  asserting state, the action list and `next_deadline_ms`:
  - backoff 5000, 10000, 20000, 40000, 80000, 120000, 120000;
  - keepalive deadline 30000 for hold 90;
  - hold expiry at 90000 gives NOTIFICATION 4/0 and last_error "hold timer expired";
  - plus the (+) cases.
- [ ] **Step 2:** `logrun make test-host`; expect a link failure.
- [ ] **Step 3:** Implement `bgp_fsm.c`; no socket or codec calls.
- [ ] **Step 4:** `logrun make test-host`; expect pass.
- [ ] **Step 5:** Commit `feat(routing): BGP session state machine`.

### Task 5: RIPng codec and scheduler

**Files:** Create `p4/components/routing/ripng.{c,h}`, `test_host/main/test_ripng.c`; extend
`tools/routing_vectors.py` with RIPng vectors and regenerate `vectors.h`.

**Interfaces:**
- Consumes: `routing_prefix_set_t`, `routing_prefix64_t`.
- Produces:
  - `ROUTING_RIPNG_PORT 521`, `ROUTING_RIPNG_METRIC 1`, `ROUTING_RIPNG_INFINITY 16`, `ROUTING_RIPNG_MAX_PAYLOAD 1232` (1280 − 40 − 8);
  - `routing_ripng_rte_t { routing_prefix64_t prefix; uint8_t metric; }`;
  - `size_t routing_ripng_encode_response(uint8_t *buf, size_t cap, const routing_ripng_rte_t *rtes, size_t n, size_t *consumed)`
    writes one message of as many entries as fit `cap` and reports how many it took;
  - `routing_ripng_kind_t routing_ripng_classify(const uint8_t *buf, size_t len)`, which returns `INVALID`,
    `RESPONSE`, `REQUEST_ALL` or `REQUEST_SOME`;
  - `size_t routing_ripng_answer_request(const uint8_t *req, size_t len, const routing_prefix_set_t *announced, routing_ripng_rte_t *out, size_t out_cap)`;
  - scheduler `routing_ripng_sched_t`:
    - `void routing_ripng_sched_start(routing_ripng_sched_t *s, const routing_prefix_set_t *set, uint32_t now_ms, uint32_t (*rand32)(void))`
    - `void routing_ripng_sched_set_changed(routing_ripng_sched_t *s, const routing_prefix_set_t *set, uint32_t now_ms)`
    - `size_t routing_ripng_sched_poll(routing_ripng_sched_t *s, uint32_t now_ms, routing_ripng_rte_t *out, size_t out_cap)`,
      which returns the entries to send now (0 for nothing);
    - `size_t routing_ripng_sched_stop(routing_ripng_sched_t *s, routing_ripng_rte_t *out, size_t out_cap)`
    - `uint32_t routing_ripng_sched_next_deadline_ms(const routing_ripng_sched_t *s)`

  `rand32` is injected so tests fix the 0..5 s offset.

- [ ] **Step 1:** Add the RIPng vectors to the script:
  - a response with the two prefixes at metric 1, and with one at metric 16;
  - a whole-table request (one RTE `::/0`, length 0, metric 16);
  - a specific request;
  - a short datagram, version 2, and one truncated mid-RTE.

  Regenerate the header.
- [ ] **Step 2:** Write a test per effect of the routing_ripng subtree:
  - full update at start;
  - next at 30000 + offset, with `rand32` returning values that give offset 0 and offset 5000;
  - a triggered update no sooner than 5000 ms after the previous one;
  - metric 16 present in the triggered update and the next 4 regular updates, absent from the 5th;
  - stop returns all announced at 16 and then polls return 0;
  - 62 entries split into messages of 61 + 1 under cap 1232.
- [ ] **Step 3:** `logrun make test-host`; expect a link failure.
- [ ] **Step 4:** Implement `ripng.c`.
- [ ] **Step 5:** `logrun make test-host`; expect pass.
- [ ] **Step 6:** Commit `feat(routing): RIPng codec and scheduler`.

### Task 6: Prefix publisher, OpenThread ops and br_core hook

**Files:**
- Create: `p4/components/routing/{publisher.c,publisher.h,ot_ops.c}`, `test_host/main/{fake_ops.c,fake_ops.h,test_publisher.c}`
- Modify: `p4/components/br_core/br_core.c` (replace `otBorderRoutingDhcp6PdSetEnabled(instance, true)` and its log
  line with the callback), `p4/components/br_core/include/br_core.h`

**Interfaces:**
- Consumes: `routing_settings_t`, `routing_prefix64_t`.
- Produces:
  - `routing_ot_ops_t`, function pointers with an `otError`-like `int` result (0 = OK):
    - `set_omr_config(routing_omr_mode_t, const routing_prefix64_t *, int8_t pref)` with pref -1 low, 0 medium;
    - `get_omr_prefix(routing_prefix64_t *)`;
    - `add_on_mesh(const routing_prefix64_t *, int8_t pref, bool default_route)`;
    - `remove_on_mesh(const routing_prefix64_t *)`;
    - `register_netdata(void)`;
    - `pd_set_enabled(bool)`;
    - `bool netdata_has_prefix(const routing_prefix64_t *)`;
    - `bool default_route_published(void)`;
  - `void routing_publisher_init(routing_publisher_t *p, const routing_ot_ops_t *ops)` (records the ULA through
    `get_omr_prefix`; called while the config is still auto, at border-router up);
  - `void routing_publisher_apply(routing_publisher_t *p, const routing_settings_t *s)`;
  - `void routing_publisher_sync_default_route(routing_publisher_t *p)`;
  - `void routing_publisher_prefix_state(const routing_publisher_t *p, routing_announce_input_t *io)` (fills
    `ula`, `ula_known`, `custom_in_netdata`, `ula_in_netdata`);
  - br_core: `void br_core_set_border_router_up_cb(void (*cb)(otInstance *instance))`.

- [ ] **Step 1:** Write `fake_ops` (it records every call with its arguments, has configurable return values and a
  Network Data membership table) and a test per effect of the routing_publisher subtree:
  - custom apply: `set_omr_config(CUSTOM, prefix, 0)`;
  - ULA add: `add_on_mesh(ula, -1, default_route)`, then `register_netdata`;
  - the default-route flag flips after `default_route_published` changes and `sync_default_route` re-adds the entry;
  - the method mapping to `pd_set_enabled`;
  - no calls on a repeated apply.
- [ ] **Step 2:** `logrun make test-host`; expect a link failure.
- [ ] **Step 3:** Implement `publisher.c` (pure). Implement `ot_ops.c` on `otBorderRoutingSetOmrConfig`,
  `otBorderRoutingGetOmrPrefix`, `otBorderRouterAddOnMeshPrefix` (preferred, SLAAC, on-mesh, stable),
  `otBorderRouterRemoveOnMeshPrefix`, `otBorderRouterRegister`, `otBorderRoutingDhcp6PdSetEnabled`, and the two Network
  Data iterations from "Decisions". Add the br_core callback and its setter, following `br_core_set_sniffer_end_cb`.
- [ ] **Step 4:** `logrun make test-host`; expect pass. `logrun make build`; expect a clean build (br_core compiles
  without the PD call; nothing registers the callback yet, so PD stays off on this intermediate commit, which is fine
  because the branch is not flashed).
- [ ] **Step 5:** Commit `feat(routing): prefix publisher and border-router hook`.

### Task 7: Routing task and wiring

**Files:**
- Create: `p4/components/routing/{task.c,console.c}`; fill `include/routing.h`
- Modify: `p4/main/app_main.c`, `p4/main/console.c`, `p4/main/CMakeLists.txt`, `p4/sdkconfig.defaults`

**Interfaces:**
- Consumes: everything above; `netcfg_backbone_netif()`, `br_core_get_mode()`, `br_core_set_border_router_up_cb`.
- Produces (`routing.h`):
  - `esp_err_t routing_start(void)`: loads the settings and starts the task;
  - `void routing_on_border_router_up(otInstance *instance)`: init the publisher, apply the PD and OMR settings;
  - `esp_err_t routing_apply(const routing_settings_t *s)`: store, then hand over to the task. Returns
    `ESP_ERR_INVALID_STATE` before start;
  - `esp_err_t routing_get(routing_settings_t *settings, routing_status_t *status)`;
  - `routing_validate_ctx_t` filled by `routing_validate_ctx_fill(routing_validate_ctx_t *ctx)` from the backbone
    netif and the received RA prefix table (`otBorderRoutingGetNextPrefixTableEntry`, on-link entries);
  - `esp_err_t routing_metrics_view(routing_metrics_view_t *out)`;
  - `esp_err_t routing_console_register(void)`.

Task loop, which the tests can't pin. As pseudocode:

```
loop:
    wait on: sockets readable (select, timeout = min(next deadlines, 500 ms)) or a settings mailbox message
    if settings message: diff old/new
        METHOD: stop old sender (BGP: STOP 6/2 per peer, flush, close; RIPng: sched_stop, send, close), start new one
        BGP_SESSIONS: STOP 6/3 removed or changed peers, START new ones
        PREFIXES: publisher_apply under the OT lock
    every 500 ms: OT lock (10 ms timeout) -> publisher_sync_default_route, publisher_prefix_state, br_up = mode is
        BR_RUNNING and role attached; compute the set; on a change: BGP ANNOUNCE_CHANGED per peer / RIPng set_changed
    feed socket events and deadlines to the FSMs / scheduler; carry out their actions with the codec
```

- [ ] **Step 1:** Implement `task.c` and `console.c` against the pseudocode and the "Decisions" section. The status
  shown is read under a mutex the task owns. `bgp_blocked` is `"no router id"` when no IPv4 address is on the
  backbone and none is configured. An accepted connection from an address that is not a configured peer is closed without
  sending anything.
- [ ] **Step 2:** Wiring:
  - `app_main.c`: call `br_core_set_border_router_up_cb(routing_on_border_router_up)` before `br_core_start`, and
    `routing_start()` after `metrics_start()`, logging a failure and carrying on;
  - `console.c`: `routing_console_register()`;
  - `CONFIG_LWIP_MAX_SOCKETS=16` in `sdkconfig.defaults`.
- [ ] **Step 3:** `logrun make build`; expect a clean build of the release image, and `logrun make build-test` for the
  test image.
- [ ] **Step 4:** Commit `feat(routing): routing task, sockets and wiring`.

### Task 8: Device API

**Files:**
- Modify: `p4/components/device_api/device_api.c` (handlers and routes), `status_json.c`, `device_api_priv.h`,
  `CMakeLists.txt` (requires `routing`), `test_host/main/test_status.c`
- Modify: `p4/components/esp_ot_br_server/src/esp_br_web.c` (`EXTRA_HANDLER_SLOTS 20`), `p4/components/PATCHES.md`

**Interfaces:**
- Consumes: `routing_get`, `routing_apply`, `routing_validate_ctx_fill`, `routing_settings_from_json`,
  `routing_settings_to_json`.
- Produces:
  - `GET /device/routing` returns 200 with the section 6 shape, or 503 `not_started`;
  - `PUT /device/routing` returns:
    - 200 with the new GET shape;
    - 400 `invalid_request` with the bad field as detail;
    - 411 or 413 on length, as for `/device/network`, with body max 2048;
  - `device_api_status_t.routing` rendered as `"routing": {"method", "omr_mode", "announced_count", "peers_established", "peers_configured"}`, with the peer fields null unless the method is bgp.

- [ ] **Step 1:** Add `test_status.c` cases:
  - "routing bgp: renders method, counts and peers";
  - "routing ripng: peer fields are null";
  - "routing not started: routing is null".
- [ ] **Step 2:** `logrun make test-host`; expect failure.
- [ ] **Step 3:** Implement the status fields, then the two handlers following `network_get_handler` and
  `network_put_handler` (gated like every `/device/*` route). Raise the slots and log the change in `PATCHES.md`.
- [ ] **Step 4:** `logrun make test-host` and `logrun make build`; expect pass and a clean build.
- [ ] **Step 5:** Commit `feat(device_api): /device/routing and routing status`.

### Task 9: Metrics

**Files:** Modify `p4/components/metrics/{metrics_types.h,render.c,scrape.c,CMakeLists.txt}`,
`test_host/{CMakeLists.txt,main/sample_input.c,main/test_render.c}`, `tools/tests/test_metrics_format.py`.

**Interfaces:**
- Consumes: `routing_metrics_view_t`, `routing_metrics_view()`.
- Produces: `metrics_input_t.routing` (`const routing_metrics_view_t *`, NULL when routing is not started). The spec
  section 8 series:
  - `br_routing_method{method}`, a state set;
  - `br_routing_announced_prefixes`;
  - `br_routing_omr_custom`;
  - `br_bgp_peer_state{peer,state}`;
  - `br_bgp_peer_uptime_seconds{peer}`;
  - `br_bgp_received_dropped_total{peer}`;
  - `br_bgp_session_resets_total{peer}`;
  - `br_ripng_updates_sent_total`, `br_ripng_requests_answered_total`, `br_ripng_responses_dropped_total`.

  Peer series appear only with method bgp, RIPng series only with ripng.

- [ ] **Step 1:** Add render tests:
  - "routing bgp: one state series per state and peer";
  - "routing ripng: no bgp series";
  - "routing absent: no routing series";

  Extend the sample input so `sample.prom` contains them, and add the names and types to the catalogue in
  `test_metrics_format.py`.
- [ ] **Step 2:** `logrun make test-host`; expect failure.
- [ ] **Step 3:** Implement the families in `render.c`, and fill the view in `scrape.c`.
- [ ] **Step 4:** `logrun make test-host` and `logrun make test-tools`; expect pass.
- [ ] **Step 5:** Commit `feat(metrics): routing, BGP and RIPng series`.

### Task 10: Routing page

**Files:** Create `p4/components/esp_ot_br_server/frontend/routing.html`, `frontend/static/routing.js`,
`tools/tests/test_routing_page.py`; modify `frontend/static/api.js` (nav entry "Routing" after "Ethernet").

**Interfaces:**
- Consumes: `GET/PUT /device/routing`.
- Produces: `routing.js` exposes its logic the way `ethernet.js` does for `tools/tests/device_js.py`:
  - `RoutingLogic.draftFromState(state)`;
  - `RoutingLogic.bodyFromDraft(draft)`, the PUT body: `bgp` always included, the status left out;
  - `RoutingLogic.visibleSections(draft)`, returning `{bgp, ripng, customPrefix}`;
  - `RoutingLogic.methodHint(state)`;
  - `RoutingLogic.dirtyCount(saved, draft)`.

- [ ] **Step 1:** Write `test_routing_page.py` with the `device_js` helpers:
  - "method bgp: bgp section visible, ripng hidden";
  - "method pd: neither section";
  - "auto omr: no custom prefix field";
  - "body: carries method, omr, announce_ula, bgp";
  - "unchanged draft: dirty count 0";
  - "method hint pd soliciting: 'DHCPv6 PD: soliciting, nothing delegated yet.'";
  - a page scenario: load, pick RIPng, save; the PUT carries `"method":"ripng"`;
  - "400 from PUT: shows the bad field next to its input".
- [ ] **Step 2:** `logrun make test-tools`; expect failure.
- [ ] **Step 3:** Build `routing.html` from the mockup: drop the yellow strip and the illustrative failing peer, and
  use the shared `style.css` plus the mockup's small extra rules (four-column cards, `.now.off`, the chip at the card
  bottom, the peer row grid) added to `style.css`. Write `routing.js`, polling the status every 5 s and keeping
  unsaved edits.
- [ ] **Step 4:** `logrun make test-tools`; expect pass. `make lint`.
- [ ] **Step 5:** Commit `feat(web): Routing page`.

### Task 11: On-board test, docs, review

**Files:** Create `p4/pytest/test_routing.py`; modify `docs/networking.md`, `docs/api.md`, `docs/metrics.md`,
`docs/README.md` (only if a new file), `CHANGELOG.md` (Unreleased).

- [ ] **Step 1:** Write `p4/pytest/test_routing.py`, following `test_pd.py` (a module-scoped macvlan network,
  containers `frrouting/frr` and a BIRD image, skipping when Docker or the network is unavailable). Tests:
  - "bgp ibgp: router learns both prefixes with the board's next-hop";
  - "bgp ebgp: as path is the board's ASN";
  - "bgp: sniffer mode withdraws within 1 s";
  - "bgp: omr change causes no session reset";
  - "bgp: 500k-route feed leaves free heap and Thread running", where FRR announces a generated table and the test
    checks `/device/diag` free heap and `/device/status` mode;
  - "bgp link-local peer: FRR accepts the 16-byte next-hop", with the result for BIRD written into
    `docs/networking.md`;
  - "ripng: router learns both prefixes via the board's link-local, metric 2";
  - "ripng: switching to off withdraws at once";
  - "ripng: a changed set shows within 5 s";
  - "custom prefix with ULA: the C6 holds addresses from both";
  - "put: bgp with no peers is 400";
  - "put: every method but pd shows pd.state disabled".

  Do not run it here.
- [ ] **Step 2:** Docs (no BDD: docs only):
  - `networking.md`: the method setting, the OMR settings, and example FRR configs (BGP neighbor with a prefix-list
    and maximum-prefix; `ripngd` with `network <backbone if>`), plus a BIRD RIPng snippet;
  - `api.md`: `/device/routing` and the status field;
  - `metrics.md`: the series;
  - `CHANGELOG.md`.
- [ ] **Step 3:** `logrun make test`, `logrun make build`, `logrun make lint`; expect everything green.
- [ ] **Step 4:** Commit `docs: mesh routing` (with the pytest file).
- [ ] **Step 5:** Whole-branch review on the most capable model. Fix the Critical and Important findings, each
  test-first, and commit.
- [ ] **Step 6 (batched hardware pass, only on the owner's go):** check `/device/status` `app.version` in its own step
  first. Then flash once, run `make test-target TEST=p4/pytest/test_routing.py`, and do one manual check: BGP to the
  UDM Pro with the custom prefix, reaching the C6 from another VLAN.

## Verification

- `logrun make test-host`: the routing suites (settings, announce, bgp_msg, bgp_fsm, ripng, publisher), device_api
  and metrics are green.
- `logrun make test-tools`: the routing page logic and the metrics format check pass.
- `logrun make build` and `logrun make build-test`: both images build.
- `logrun make lint`: clean.
- Hardware, deferred and batched: `p4/pytest/test_routing.py` on the rig, plus the UDM Pro check.
