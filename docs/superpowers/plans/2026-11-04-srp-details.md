# Service details in the topology scan: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
> Approved 2026-11-04.

## Context

The Topology page names a device only when it registered with this board's SRP server, and shows only service types.
On `ha-thread-bbc1` most devices register with the Home Assistant OTBR, so they stay "Unknown 0xNNNN". On 2026-11-04 we
checked on hardware that every border router answers DNS-SD browse/resolve from the mesh, that devices answer no mDNS,
that Matter host names are the Thread extended address, and what ESPHome/Matter/meshcop TXT records contain (ESPHome
test device on the aux C6). This plan makes the scan collect records from every SRP server, keep raw TXT, port,
subtypes and lease, and lets the page decode and show them.

**Goal:** `GET /device/topology` carries every SRP server's service records (raw TXT base64, port, subtypes,
lease/TTL, source) plus an `srp_servers` list; the Topology page decodes them into names, a "Device" summary,
"Registered with", service ports and a raw-records view.

**Architecture:** `mesh_scan` gets an SRP step that runs **in parallel** with discover and the child phases (DNS
client queries to the other servers' DNS-SD, port 53), filters and stages the answers, and merges them with this
board's own SRP snapshot at finish. Filtering, parsing, merging and the TXT pool are pure and host-tested through the
existing ops seam. `topology.js` gains pure decoders in `TopologyLogic` and the dialog/table additions in
`TopologyPage`.

**Tech stack:** ESP-IDF v6.1, OpenThread DNS client (`CONFIG_OPENTHREAD_DNS_CLIENT=y`, service discovery on by
default) and SRP server APIs, Unity host tests, node page tests.

**Spec:** `docs/superpowers/specs/2026-11-04-srp-details-design.md` (`156b4b5`). Mockup:
`docs/superpowers/specs/2026-11-04-srp-details-mockup.html`. Read both with this plan.

**Styleguide:** `STYLEGUIDE.md`:
- astyle via `make lint`, `snake_case`, the `mesh_scan_` prefix, `esp_err_t` + `ESP_RETURN_ON_ERROR`, one `TAG` per file;
- host tests in `p4/components/mesh_scan/test_host/`, named `"<scenario>: <effect>"`, tagged `[mesh_scan]`;
- the ops seam is the only test seam;
- `topology.js` stays ES5 like the other `static/*.js`, page tests in `tools/tests/test_topology_page.py`;
- every change to vendored `esp_ot_br_server` gets a `p4/components/PATCHES.md` entry.

## Ruling against the spec (amend it in Task 1)

The spec says the SRP step runs **first**, before discover. The plan runs it **in parallel** from scan start instead:
- running it first would make `POST /device/topology` answer before discover starts, losing today's synchronous
  409 `busy` "mesh diagnostics are in use";
- in parallel it still never loses time to a slow child phase, which was the spec's reason.

`finish()` waits until both the mesh phases and the SRP step are over (or the cap hits). Cost if wrong: DNS and diag
messages share the message pool for a few seconds; at most 4 DNS queries in flight.

## Global constraints

- Service types asked on other servers, in this order: `_matter._tcp`, `_matterc._udp`, `_esphomelib._tcp`,
  `_hap._udp`, `_http._tcp`, `_meshcop._udp`, `_trel._udp`.
- Servers: network data services with enterprise number 44970 and service data type `0x5d` (DNS/SRP unicast), either
  service-data form (address + port in service data) or server-data form (address + port in server data). Anycast
  (`0x5c`) is ignored. This board's own entry is recognised by an address that is one of this board's unicast
  addresses, and is read locally, never queried.
- DNS queries: server address, port 53, 3000 ms response timeout, 2 attempts, `SRV_TXT_OPTIMIZE` service mode.
- Limits: 4 queries in flight, 48 resolves per scan, 15 000 ms SRP budget from scan start (inside the 60 s cap),
  8 servers, 64 hosts, 8 addresses and 8 services per host, 6 subtypes per service, 512 TXT bytes per service (cut at
  a string boundary, `txt_truncated: true`), one 24 KiB pool per result for TXT and subtype labels.
- "Described" by a browse answer: SRV found, TXT found (possibly empty), and at least one address. Otherwise resolve.
- Remote hosts kept: an address under an OMR prefix of the network data or the mesh-local prefix. Exception:
  `_meshcop._udp` / `_trel._udp` records kept when TXT `xp` equals this network's extended PAN ID and `xa` is not this
  board's extended address.
- De-duplication by host name, case-insensitive: host fields from this board's record, else the first server; services
  merge by instance and type, each with its own `source`.
- Server states: `answered`, `no_answer`, `cut_short`. None of them sets `partial`.
- JSON field names exactly as spec section 4: `srp_servers[]` {`rloc16`, `address`, `port`, `this_board`, `state`,
  `hosts`}; hosts gain `source`, `lease_s` + `key_lease_s` (local) or `ttl_s` (remote); services gain `port`,
  `source`, `subtypes`, `txt` (base64, `""` when no TXT), `txt_truncated`.
- Display name order: nickname; ESPHome `friendly_name`; HomeKit instance name; meshcop instance name; SRP host name
  unless it equals the extended address, then "Matter device 0xNNNN"; "Unknown 0xNNNN". This board keeps "This border
  router".
- No flashing inside tasks; one batched hardware pass after review.
- Commits end with only `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review focus

1. **A listed server never answers** (a border router switched off but still in the network data). Its queries time out,
   it ends `no_answer`, the other server's records are intact, and the scan finishes no later than its mesh phases or
   the 15 s budget, whichever is later. Owner: Task 3.
2. **The same device reaches us twice** (registered with this board and proxied by the HA server, or re-registered on
   both). One host, local fields win, services merged, no duplicate service rows. Owner: Task 3.
3. **Oversized or odd TXT**: a 700-byte record, a pool already full, key-only entries (`at`, `sb`), binary values,
   empty strings, a `<b>` in `friendly_name`. Board: cut at a string boundary, flags set, valid JSON. Page: key-only as
   flags, binary as hex, text only through `textContent`. Owners: Tasks 1, 2, 5, 6.
4. **Late DNS callbacks** after the budget, after the cap, or for an older scan generation are dropped and never touch
   a freed or newer result. Owner: Task 3.
5. **A result saved in the browser by older firmware** (no `srp_servers`, no `txt`) still renders as before.
   Owner: Task 6.

## Design notes

### Types (`scan_types.h`)

- `mesh_scan_srp_server_t`: `uint16_t rloc16`, `mesh_scan_ip6_t addr`, `uint16_t port`, `bool this_board`,
  `mesh_scan_srp_state_t state` (`ANSWERED`, `NO_ANSWER`, `CUT_SHORT`), `uint8_t hosts`.
- `mesh_scan_srp_host_t` gains `uint16_t source`, `bool local`, `uint32_t lease_s, key_lease_s, ttl_s`.
- Each service gains `uint16_t port`, `uint16_t source`, `uint16_t txt_off, txt_len`, `bool has_txt, txt_truncated`,
  `uint16_t sub_off`, `uint8_t n_sub` (labels stored NUL-separated in the pool).
- `mesh_scan_result_t` gains `srp_servers[MESH_SCAN_MAX_SRP_SERVERS]`, `n_srp_servers`, `uint8_t *pool`,
  `uint16_t pool_used`. The pool is a second `ops.alloc` block owned by the result and released with it.

### Pure helpers (`srp.c`, new, host-built)

- `bool mesh_scan_srp_server_parse(uint32_t enterprise, const uint8_t *svc, uint8_t svc_len, const uint8_t *srv,
  uint8_t srv_len, uint16_t rloc16, mesh_scan_srp_server_t *out);`
- `bool mesh_scan_srp_addr_kept(const mesh_scan_ip6_t *addrs, uint8_t n, const mesh_scan_prefix_t *omr, uint8_t n_omr,
  const uint8_t ml_prefix[8]);`
- `bool mesh_scan_txt_find(const uint8_t *txt, uint16_t len, const char *key, const uint8_t **val, uint8_t *val_len);`
  (`val_len` 0 and `val` NULL for a key-only entry)
- `uint16_t mesh_scan_txt_cut(const uint8_t *txt, uint16_t len, uint16_t max);` the longest prefix of whole strings
  that fits `max`.
- `bool mesh_scan_br_record_kept(const char *type, const uint8_t *txt, uint16_t len, const uint8_t ext_pan_id[8],
  const uint8_t own_ext[8]);`
- `bool mesh_scan_pool_put(mesh_scan_result_t *r, const uint8_t *data, uint16_t len, uint16_t *off);` false when full.

### New ops (`scan.h`) and callbacks

- `uint8_t (*srp_servers)(mesh_scan_srp_server_t out[MESH_SCAN_MAX_SRP_SERVERS], uint8_t ext_pan_id[8],
  uint8_t own_ext[8]);` this board's entry marked `this_board`.
- `int (*dns_browse)(const mesh_scan_srp_server_t *srv, const char *type, uint32_t gen, uint16_t job);`
- `int (*dns_resolve)(const mesh_scan_srp_server_t *srv, const char *instance, const char *type, uint32_t gen,
  uint16_t job);`
- `void (*arm_srp_deadline)(uint32_t ms, uint32_t gen);` also stopped by `disarm_cap`.
- Callbacks from the OT task: `mesh_scan_on_srp_record(uint32_t gen, uint16_t job, const mesh_scan_srp_record_t *rec)`
  (instance, host, port, addresses, TTL, TXT bytes and truncation, `described`), `mesh_scan_on_srp_query_done(uint32_t
  gen, uint16_t job, int ot_error)`, `mesh_scan_on_srp_deadline(uint32_t gen)`.

### SRP step (pseudocode)

```
start:   servers = ops.srp_servers(); jobs = browse(each remote server × each type)
         if jobs empty: srp_done = true else arm_srp_deadline(15000); pump()
pump:    while in_flight < 4 and queue not empty: issue next job (browse or resolve)
record:  drop unless gen matches and job in flight
         if browse and not described and resolves < 48: queue resolve(server, instance, type)
         else keep-check (address rule, or br-record rule) -> stage into remote hosts (dedupe by host name)
done:    in_flight--, mark server answered on NONE; pump(); if queue empty and in_flight 0: srp_done
deadline: servers with queries in flight -> cut_short; srp_done; queued jobs dropped
srp_done: servers never answered -> no_answer; if mesh phases over: finish()
finish:  (as today) + local snapshot first, then merge staged remote hosts; per-server host counts
```

## BDD skeleton

```
mesh_scan_srp (srp.c, host)
    mesh_scan_srp_server_parse
        Enterprise 44970, service data 0x5d + 16-byte address + port
            - true, address and port from service data, rloc16 copied
        Service data 0x5d only, server data 16-byte address + port
            - true, address and port from server data
        Service data 0x5c (anycast)
            - false
        Other enterprise number
            - false
        Truncated service or server data
            - false
    mesh_scan_srp_addr_kept
        An address under an OMR prefix
            - true
        Only a mesh-local EID
            - true
        Only LAN addresses (fdaa:20::69, 2a06:…)
            - false
        No addresses
            - false
    mesh_scan_txt_find
        Key present with value
            - true, value pointer and length
        Key-only entry ("at")
            - true, NULL value, length 0
        Key absent, or only a longer key with the same prefix ("xpa")
            - false
        A string length running past the buffer
            - false, no read beyond len
    mesh_scan_txt_cut
        Fits
            - len unchanged
        Longer than max
            - longest prefix of whole strings within max
        First string alone longer than max
            - 0
    mesh_scan_br_record_kept
        _meshcop._udp with xp = this network, xa = another router
            - true
        xp of another network
            - false
        xa = this board's extended address
            - false
        No xp
            - false
        Any other type
            - false
    mesh_scan_pool_put
        Room left
            - true, offset returned, pool_used grows
        Would overflow 24 KiB
            - false, pool unchanged

mesh_scan core (scan.c, host, fake ops)
    start
        Two servers in the network data, one is this board
            - browses only the remote one, 7 jobs, at most 4 in flight
            - arms the SRP deadline with 15000 ms and the current generation
            - discover still starts at once; returns ESP_OK
        No remote server
            - no DNS query, SRP step counts as done
        ops.dns_browse fails for one job
            - that job counts as done, the rest go on
    on_srp_record
        Browse answer described (SRV, TXT, address) and address kept
            - staged as a remote host with source = server rloc16, ttl_s, port, TXT in the pool
        Browse answer not described
            - a resolve is queued for that instance and type
        49th resolve wanted
            - not queued; the browse data is kept if it passes the filter, else dropped
        Address only on the LAN
            - dropped
        meshcop record with this network's xp from a LAN address
            - kept
        Same host name already staged from another server
            - one host, services merged, host fields of the first kept
        TXT of 700 bytes
            - stored cut to whole strings within 512, txt_truncated true
        Pool full
            - service kept without TXT, txt_truncated true
        Stale generation or a job not in flight
            - ignored, nothing written
    on_srp_query_done
        NONE
            - server state answered; next queued job issued
        Every query of a server timed out
            - server state no_answer at the end
        Last job of the step done while child phases still run
            - scan stays running
        Last job done after the mesh phases are over
            - scan finishes (state done)
    on_srp_deadline
        Queries in flight
            - their servers cut_short; queued jobs dropped; later callbacks ignored
        Mesh phases still running
            - scan keeps running until they end
    finish
        Mesh phases over, SRP step not over
            - waits (state running)
        Cap hit during the SRP step
            - finishes with what is staged; servers with queries in flight cut_short; partial unchanged by SRP
        Local snapshot holds the same host as a remote one
            - one host, source = this board, lease_s/key_lease_s set, ttl_s absent, services merged
        64 hosts already
            - further remote hosts dropped
        srp_servers
            - one entry per network data server, hosts = kept hosts from it
    result release
        - the pool block is released with the result (allocs == releases)

mesh_scan render (render.c, host)
    srp_servers
        - each server with rloc16, address, port, this_board, state string
    host from this board
        - source, lease_s, key_lease_s; no ttl_s
    host from another server
        - source, ttl_s; no lease fields
    service
        - port, source, subtypes array, txt base64, txt_truncated
        - empty TXT renders "txt": ""
        - base64 of 1, 2 and 3 trailing bytes padded correctly
    result without SRP data
        - "srp_servers": [], hosts as before plus empty new fields

TopologyLogic (topology.js, node)
    txtEntries
        Recorded ESPHome TXT (2026-11-04)
            - entries in order, friendly_name "C6 Thread Test"
        Key-only entry
            - {key, value: null}
        Binary value (meshcop xa)
            - value as bytes, hexText "ee1babae73de277b"
        Empty or invalid base64
            - []
    describe
        ESPHome host
            - kind "ESPHome", version, board, platform, project, encrypted API, web port
        Matter host with two instances on two fabrics
            - kind "Matter", fabrics [{fabric, node}] ×2, SII/SAI/SAT in ms, ICD when present
        Matter commissionable
            - vendor id, product id, device type, device name
        HomeKit host
            - model, category name from ci (1-36), paired from sf
        meshcop host
            - vendor, model, Thread version, extended address from xa
        Unknown service type
            - kind null, raw entries still available
    srpHostsFor
        Address match
            - that host
        Host name equals the device's ext address (any case)
            - that host
        meshcop/trel xa equals the device's ext address
            - that host
        Border router with its own SRP host and a meshcop host
            - both, this board's record first
        No match
            - []
    displayName
        Nickname set
            - nickname
        ESPHome friendly_name
            - it
        HomeKit instance
            - it
        meshcop instance
            - it
        Host name = ext address
            - "Matter device 0xNNNN"
        Nothing
            - "Unknown 0xNNNN"; this board "This border router"
    rows (search)
        Query matches an ESPHome friendly_name
            - that device listed

TopologyPage (topology.js, node)
    device table
        Device with a description
            - kind line under the name
        srp_servers all answered
            - hint "Service records from N SRP servers: …"
        A server no_answer or cut_short
            - amber note naming it
    device dialog
        ESPHome device
            - Device, Project, Node name, Registered with "This board · lease ends in …", services with ports, Open link
              to http://[first OMR address]:80/
        No OMR address
            - no Open link
        Matter device from the HA server
            - Fabrics list, retry timing, Registered with the server's display name and RLOC16, pairing-mode note
        Other border router
            - Device vendor/model/Thread version, SRP server line
        Show raw records
            - every TXT entry per service, hex for binary, text via textContent
    saved result from older firmware
        - renders without errors, no Device lines, no servers hint
```

## Tasks

### Task 1: Pure SRP helpers and the spec amendment

**Files:** create `p4/components/mesh_scan/srp.c`, `srp.h` (private, next to `scan.h`); modify
`p4/components/mesh_scan/CMakeLists.txt` and `test_host/main/CMakeLists.txt`; create
`test_host/main/test_srp.c`; modify the spec (the "Timing" row: in parallel from scan start, with the reason).

**Interfaces:** produces the six helpers and `MESH_SCAN_*` limits above (in `scan_types.h`).

- [ ] Write `test_srp.c` from the `mesh_scan_srp` branch, one `TEST_CASE` per effect; meshcop fixture = the bytes
  recorded on 2026-11-04 (xp `5965b57e30c0def4`, xa `7e287c8b8b140ee6`).
- [ ] `logrun make test-host`: the mesh_scan suite fails to build.
- [ ] Implement `srp.c`.
- [ ] `logrun make test-host`: green.
- [ ] Amend the spec's Timing row; commit `feat(mesh_scan): SRP record helpers`.

### Task 2: Result types, local snapshot fields, rendering

**Files:** modify `scan_types.h`, `ot_ops.c` (`snapshot_srp_host`: port, TXT via `otSrpServerServiceGetTxtData`, subtype
labels via `otSrpServerServiceGetSubTypeServiceNameAt` + `otSrpServerParseSubTypeServiceName`, lease via
`otSrpServerHostGetLeaseInfo`, `source` = this board's RLOC16, `local` true), `render.c` (a static base64 encoder,
`srp_servers`, new fields), `test_host/main/sample_result.c`, `test_render.c`.

**Interfaces:** consumes Task 1 (`mesh_scan_pool_put`, `mesh_scan_txt_cut`). The pool is allocated in
`mesh_scan_core_start` next to the block and released with it (scan.c change limited to alloc/release here).

- [ ] Extend `test_render.c` from the `mesh_scan render` branch; extend `sample_result.c` with one local and one
  remote host, and a scan-start allocation test for the pool (alloc failure → `no_memory`).
- [ ] `logrun make test-host`: fails.
- [ ] Implement; `logrun make test-host` and `logrun make build`: green.
- [ ] Commit `feat(mesh_scan): service port, TXT, subtypes and lease in the scan result`.

### Task 3: The SRP step in the scan state machine

**Files:** modify `scan.h` (ops, callbacks), `scan.c`, `test_host/main/fake_ops.c/.h` (scripted servers, recorded
browse/resolve calls, deadline), `test_scan.c`.

**Interfaces:** consumes Tasks 1-2. Produces the ops and callbacks in Design notes for Task 4.

- [ ] Write the `mesh_scan core` branch tests (Review focus 1, 2, 4).
- [ ] `logrun make test-host`: fails.
- [ ] Implement per the pseudocode; keep the lock order (OpenThread lock, then scan mutex; ops never take the
  OpenThread lock).
- [ ] `logrun make test-host`: green. Commit `feat(mesh_scan): ask every SRP server during the scan`.

### Task 4: Real ops in ot_ops.c

**Files:** modify `p4/components/mesh_scan/ot_ops.c`.

**Interfaces:** consumes Task 3's ops.
- `srp_servers`: `otNetDataGetNextService` + `mesh_scan_srp_server_parse`; `this_board` by comparing with
  `otIp6GetUnicastAddresses`; extended PAN ID from `otThreadGetExtendedPanId`; own ext from `otLinkGetExtendedAddress`.
- `dns_browse` / `dns_resolve`: `otDnsClientBrowse` / `otDnsClientResolveService` with an `otDnsQueryConfig` per the
  Global constraints. The context pointer encodes gen and job. The callback walks
  `otDnsBrowseResponseGetServiceInstance` / `GetServiceInfo` / `GetHostAddress` (or
  `otDnsServiceResponseGetServiceInfo`) into a stack `mesh_scan_srp_record_t` with a 512-byte TXT buffer, then calls
  the scan callbacks.
- `arm_srp_deadline`: a one-shot `esp_timer` like the child deadline, dispatched to the OT task the same way.

- [ ] Implement; `logrun make build`: OK; `logrun make test-host`: still green.
- [ ] Check the OpenThread message pool and DNS client query limits in the IDF 6.1 config; note the values in the
  commit message if any must change (none expected).
- [ ] Commit `feat(mesh_scan): DNS-SD queries to the other SRP servers`.

### Task 5: Page logic

**Files:** modify `p4/components/esp_ot_br_server/frontend/static/topology.js` (`TopologyLogic` only: `txtEntries`,
`describe`, `srpHostsFor` replacing `srpHostFor`, `displayName`, `matches`), `tools/tests/test_topology_page.py`
(fixture: recorded ESPHome/Matter/meshcop TXT as base64; HomeKit and Matter commissionable fixtures marked synthetic).

- [ ] Write the `TopologyLogic` tests; `logrun make test-tools`: fail.
- [ ] Implement; `logrun make test-tools`: green. Commit `feat(web): decode SRP records on the Topology page`.

### Task 6: Page UI

**Files:** modify `topology.js` (`TopologyPage`: table kind line, servers hint/note, dialog facts, raw-records toggle,
Open link), `topology.html` (containers if needed), `static/style.css` (a few `t-` classes, mockup look),
`test_topology_page.py`, `p4/components/PATCHES.md` (entry 16 follow-up or a new entry), the page-size budget in
`tools/tests/test_device_pages.py` only if exceeded (with a comment).

- [ ] Write the `TopologyPage` tests (Review focus 3, 5); `logrun make test-tools`: fail.
- [ ] Implement to match the mockup; `logrun make test-tools`, `logrun make build`: green.
- [ ] Commit `feat(web): device details from SRP records on the Topology page`.

### Task 7: Docs

**Files:** `docs/api.md` (topology row + a "Service records" subsection with the new fields), topology spec 3.3 pointer
to the new spec, `CHANGELOG.md`, the plan copy under `docs/superpowers/plans/`.

- [ ] Write; `logrun make lint`. Commit `docs: SRP service details in the topology scan`.

### Task 8: Review, then the hardware pass

- [ ] Whole-branch review on the most capable model; fix Critical/Important findings test-first.
- [ ] Batched hardware pass on `ha-thread-bbc1` (aux C6 still on ESPHome; board OTA after checking `app.version`):
  - scan; check `srp_servers` (this board + `0x1000`, both `answered`), the ESPHome host with decoded TXT, both Matter
    hosts with `source` 0x1000, the HA meshcop host kept, no AirPlay/LAN hosts, and the SRP step's duration;
  - the page in a browser: names for 0x1000, 0x3000, 0x6400, 0xE400; dialogs as in the mockup;
  - existing `p4/pytest/test_topology.py` (needs the C6 back on ot_cli: ask before restoring the backup).

## Verification

- `logrun make test-host`: mesh_scan suite green (new `test_srp.c`, extended render/scan tests).
- `logrun make test-tools`: topology page tests green.
- `logrun make build`, `logrun make lint`: OK.
- Hardware (deferred, batched): the Task 8 checks.
