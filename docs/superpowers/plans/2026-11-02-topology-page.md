# Topology page redesign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Topology page with one that finds every Thread device and its routable addresses: a mesh scan on
the board, a deterministic map, a searchable device table and a device dialog, with the last result kept in the
browser.

**Architecture:**
- A new component `mesh_scan` runs OpenThread mesh diagnostics as a callback-driven state machine.
  - It keeps one result in PSRAM and renders it as JSON.
  - All OpenThread calls go through an ops struct, so the state machine and the renderer are host-tested.
- `device_api` exposes the scan as `POST`/`GET /device/topology`.
- The page is a rewritten `topology.html` with `static/topology.js`, split the way the device pages are:
  - `TopologyLogic`: pure, tested under node.
  - `TopologyPage`: DOM, tested with the fake-DOM harness.

**Tech Stack:** ESP-IDF 6.1 (C, FreeRTOS, OpenThread `mesh_diag.h` and `srp_server.h`), Unity host tests on the `linux`
target, plain ES5-style JS, pytest with node (`make test-tools`).

**Spec:** `docs/superpowers/specs/2026-11-02-topology-page-design.md` (approved sketches linked at its top).

**Styleguide:** `STYLEGUIDE.md`.
- C: `snake_case`, prefix `mesh_scan_`, one `TAG` per file, `esp_err_t` with `ESP_RETURN_ON_ERROR`/`ESP_GOTO_ON_ERROR`,
  astyle via `make lint`.
- Host tests in `<component>/test_host/`, named `"<scenario>: <effect>"`. Ops-struct seams only where a host test needs
  them.
- Frontend:
  - Follow the device pages: `XLogic` plus `XPage(env)`, `module.exports` for node, `DeviceCommon`/`DeviceKit` helpers.
  - No `innerHTML` with data.
  - Tests in `tools/tests/test_<page>_page.py` whose class docstrings name the BDD module, one test per effect.
- Vendored frontend files: list every change in `p4/components/PATCHES.md`.

## Global Constraints

- Branch `feat/topology-page`. Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- **No flashing and no on-board runs inside tasks.** Target tests are written in Task 7 but run only in Task 8, the
  batched hardware pass after the whole-branch review and its fixes. Flash budget: one P4 app flash.
- Long commands go through `logrun` (for example `logrun make test-host`).
- **Scan:**
  - one at a time;
  - capped at 60 s (`MESH_SCAN_CAP_MS 60000`);
  - per-router queries in router ID order, one at a time;
  - result in PSRAM.
- **`partial` is true** when any of these holds:
  - the cap was hit;
  - a router has `responded: false`;
  - a router has `children_detail: false`.
- **Endpoint:**
  - `POST /device/topology`: 202 with the state object, also when it joins a running scan.
  - 409 `busy`, with one of these `detail` texts:
    - `mode <MODE>` when the mode is not `BR_RUNNING` (e.g. `mode SNIFFER`);
    - the long-op slot's `device_api_op_busy_detail` text while OTA, a push or a ROM recovery holds it;
    - `mesh diagnostics are in use` on `OT_ERROR_BUSY`.
  - `GET /device/topology`: 200 with the state object.
- **State object:** exactly as in spec 3.3, plus `responded` on each router (spec 3.1, 2a).
- **Page:**
  - polls `GET` every 1000 ms while `running`;
  - ping is `/ping` with `count 4`, `size 32`;
  - storage keys `topology:last` = `{saved_at, result}` and `topology:names` = `{ext: nickname}`.
- **Copy:** use the spec's wording for every user-visible text (spec 4.1–4.6).
- **LQI colors:** 3 `#1e7e34`, 2 `#a35f00`, 1 `#c62828` (dashed). Accent `#b35c00`. Router `#1f5f99`. No `Math.random` in
  the page.

## Review Focus

1. **Untrusted strings.** SRP host names and service instance names (any bytes, quotes, `<`) must come out as valid,
   escaped JSON from the firmware and as text, never markup, on the page. The tests go in Task 1 (JSON escaping) and
   Task 6 (rendering an SRP name containing `<b>"x"</b>`).
2. **Corrupt or old saved data.** A `topology:last` holding garbage, an older shape, or one that throws on read must
   fall back to "no saved result", not a broken page. The test goes in Task 5.
3. **Size limits.** A router with more children or addresses than the caps must be truncated, keep `partial` honest and
   not overflow. The test goes in Task 2 (children over `MESH_SCAN_MAX_CHILDREN`).
4. **Mid-scan reload or a second tab.** A `POST` while running joins the scan, and a page opened during a scan shows
   progress, not a second start. The tests go in Task 4 (join gives 202) and Task 6 (open during `running` polls without
   `POST`).
5. **Links to routers missing from the result.** A `links` entry to a router ID that is neither a responding router nor
   a stub must not be drawn and must not crash the pairing. The test goes in Task 5.

---

## File structure

| File | Responsibility |
| --- | --- |
| `p4/components/mesh_scan/include/mesh_scan.h` | Public API: init, start, cancel, render state; state and error enums |
| `p4/components/mesh_scan/scan_types.h` | Result structs and caps (`mesh_scan_result_t`, `mesh_scan_router_t`, `mesh_scan_child_t`, `mesh_scan_srp_host_t`, `mesh_scan_network_t`) |
| `p4/components/mesh_scan/render.c`, `render.h` | Pure JSON render of the state and the result through a chunk sink |
| `p4/components/mesh_scan/scan.c`, `scan.h` | State machine over `mesh_scan_ops_t`: discover → per-router queries → snapshot; cap, cancel, partial |
| `p4/components/mesh_scan/ot_ops.c` | The real ops: OpenThread mesh diag, router table, network data, SRP server, esp_timer, br_core mode |
| `p4/components/mesh_scan/CMakeLists.txt` | `linux` builds `render.c` + `scan.c` only (like `metrics`) |
| `p4/components/mesh_scan/test_host/` | Unity: `test_render.c`, `test_scan.c`, `fake_ops.c` |
| `p4/components/device_api/topology.c` | Handlers and the pure start-result mapping |
| `p4/components/device_api/device_api.c` | Routes; `mesh_scan_cancel()` before push, ROM recovery and pcap start; `mesh_scan_init()` at start |
| `p4/components/device_api/test_host/main/test_topology.c` | Mapping tests |
| `p4/components/esp_ot_br_server/frontend/topology.html` | New markup (rewritten) |
| `p4/components/esp_ot_br_server/frontend/static/topology.js` | `TopologyLogic` + `TopologyPage` |
| `p4/components/esp_ot_br_server/frontend/static/style.css` | Map, table, chips, dialog, pills (appended, existing tokens) |
| `tools/tests/device_pages_harness.js` | Registers `topology`; fake `<dialog>` (`showModal`/`close`) and `localStorage` |
| `tools/tests/test_topology_page.py` | Logic and page tests |
| `tools/tests/test_device_pages.py` | Adds `topology` to `PAGES` |
| `p4/pytest/test_topology.py`, `p4/pytest/test_latency.py` | Target tests (run in Task 8 only) |
| `docs/api.md`, `docs/troubleshooting.md`, `p4/components/PATCHES.md` | Docs |

## BDD skeleton

```
mesh_scan_render (render.c)
    mesh_scan_render_state
        On idle
            - writes {"state":"idle"} only
        On running before discover ends
            - writes progress {"routers":0,"queried":0}
        On running during queries
            - writes progress with routers = discovered count and queried = routers finished
        On done
            - writes age_s and the full result in the spec 3.3 field names and order
            - writes links only for lq_in > 0
            - writes a stub router as router_id, rloc16, responded:false and nothing else
            - omits the child-table fields of children under children_detail:false
            - writes rloc16 and partition_id as unsigned numbers, ext addresses as 16 lower-case hex digits
            - writes IPv6 addresses in RFC 5952 short form and prefixes as "<addr>/64"
            - writes error rates as percent with one decimal
        On failed
            - writes error ("busy" | "stopped" | "no_memory") and age_s, no result
        On a string with quotes, backslashes, control bytes or non-ASCII
            - escapes it so the output parses as JSON and round-trips the text
        On a result larger than one chunk
            - calls the sink with chunks of at most 2048 bytes whose concatenation is the whole document
        On the sink failing
            - stops and returns its error

mesh_scan (scan.c)
    mesh_scan_start
        When idle, done or failed and the mode is BR_RUNNING
            - frees the previous result, allocates a new one, calls ops.discover, state becomes running
            - returns ESP_OK
        When a scan is running
            - returns ESP_ERR_INVALID_STATE and leaves the scan untouched
        When ops.discover returns OT_ERROR_BUSY
            - state becomes failed with error busy
            - returns ESP_ERR_NOT_FINISHED
        When the result cannot be allocated
            - state becomes failed with error no_memory
            - returns ESP_ERR_NO_MEM
        When the mode is not BR_RUNNING
            - returns ESP_ERR_INVALID_STATE without calling ops
    discover callback
        On each router reported
            - stores ext address, rloc16, router id, version, the three flags, link qualities, addresses (up to the cap) and children (rloc16, mode, lq_in)
        On discover finished
            - adds a responded:false stub for every router id in ops.router_table that did not report
            - starts the child-table query of the lowest router id
        On discover finished with no router
            - goes straight to the snapshot and finishes done
    per-router queries
        On a child-table entry
            - fills that child's ext address, timeout, age, connection time, RSSI avg/last, link margin, error rates, version, csl (csl = CSL period > 0)
        On a child-table entry for an RLOC16 discover did not list
            - adds the child with the table data and mode from the entry
        On the child-table query done
            - starts the children-addresses query of the same router
        On a children-addresses report
            - stores the addresses of the child with that RLOC16 (up to the cap)
        On the addresses query done
            - increments progress.queried and starts the next router, or the snapshot after the last
        On a query timing out or answering with an error
            - marks that router children_detail:false and moves on
        On a query that fails to start with OT_ERROR_NO_BUFS or OT_ERROR_BUSY
            - marks that router children_detail:false and moves on
        On a query that fails to start with OT_ERROR_INVALID_STATE
            - state becomes failed with error stopped
        On more children than MESH_SCAN_MAX_CHILDREN for a router
            - keeps the first MESH_SCAN_MAX_CHILDREN and marks the router children_detail:false
    snapshot
        On reaching it
            - copies the network fields, the SLAAC on-mesh prefixes and the SRP hosts (domain removed, deleted hosts skipped)
            - sets partial per the global rule, records the end time, state becomes done
    mesh_scan_cancel
        While running
            - calls ops.cancel, state becomes failed with error stopped
        Otherwise
            - does nothing
    cap timer
        On 60 s elapsed while running and the mode is BR_RUNNING
            - calls ops.cancel, takes the snapshot with partial:true, state becomes done
        On 60 s elapsed while the mode is not BR_RUNNING
            - state becomes failed with error stopped
    any callback
        When the mode is no longer BR_RUNNING
            - calls ops.cancel, state becomes failed with error stopped
    age_s
        After done or failed
            - is whole seconds from the end time to ops.now_ms

device_api topology (topology.c)
    device_api_topology_start_reply
        On ESP_OK or ESP_ERR_INVALID_STATE with mode BR_RUNNING
            - 202, no error
        On a mode other than BR_RUNNING
            - 409 busy, detail "mode <MODE>"
        On the long-op slot held
            - 409 busy, detail = device_api_op_busy_detail text
        On ESP_ERR_NOT_FINISHED
            - 409 busy, detail "mesh diagnostics are in use"
        On ESP_ERR_NO_MEM
            - 500 no_memory
    POST /device/topology
        On 202
            - body is the state object
    GET /device/topology
        Always
            - 200, body is the state object, no OpenThread call
    radio push, ROM recovery, pcap start
        Before they touch OpenThread
            - call mesh_scan_cancel

TopologyLogic (static/topology.js)
    addressClass(addr, network)
        On an address under a 2000::/3 OMR prefix
            - "global"
        On an address under an fc00::/7 OMR prefix
            - "ula"
        On mesh-local with IID 0:ff:fe00:xxxx, xxxx < 0xfc00
            - "rloc"
        On mesh-local with IID 0:ff:fe00:fcxx
            - "aloc"
        On other mesh-local
            - "mleid"
        On fe80::/10
            - "linklocal"
        On ff00::/8
            - "multicast"
        Otherwise
            - "other"
    devices(result)
        On a result
            - returns one device per router, stub and child, each with kind br|leader|router|stub|med|sed, parent id, display fields
        On this board being a child (no this_device router)
            - marks the child with rloc16 == this_rloc16 as this board
    routerLinks(result)
        On two routers that list each other
            - one link with a = lower router id, lqAB = b's lq_in for a, lqBA = a's lq_in for b
        On a link listed by one side only
            - one link with the missing side 0
        On a link to a router id that is in no router entry
            - no link
    linkText(link)
        On equal sides
            - "3"
        On different sides
            - "3/2" (a's view first)
    displayName(device, names, srpHosts)
        On a nickname for the ext address
            - the nickname
        On an SRP host sharing an address
            - the host name
        Otherwise
            - "Unnamed 0xNNNN"
    rows(devices, filter, query, names, srpHosts)
        On "all"
            - this board, its children, then each router by router id followed by its children
        On "routers" / "end" / "sleepy"
            - only those kinds, same order
        On a query
            - keeps rows whose name, RLOC16, ext address, SRP host name or any address contains it, case-insensitive
    layout(devices, links)
        On the same input twice
            - the same coordinates
        On any input
            - this board (or the leader when this board is a child) at the centre; every child within its parent's fan; the map size grows with the node count
    savedResult(storage)
        On a valid topology:last
            - {savedAt, result}
        On missing, unparsable, wrong-shaped data or a throwing storage
            - null
    pickLast(saved, boardState, nowMs)
        On a board result newer than the saved one
            - the board result, saved at nowMs - age_s*1000
        Otherwise
            - the saved one (or null)
    conflictText(status, body)
        On 409 with detail "mode X"
            - "The Thread network isn't running (mode X)"
        On 409 with detail "mesh diagnostics are in use"
            - "Another mesh scan is running, try again in a few seconds"
        On 409 with another detail
            - that detail
    pingSummary(result, sleepy)
        On replies
            - "R of 4 replies", loss %, min/avg/max ms, plus the sleepy line for sleepy devices

TopologyPage (static/topology.js)
    init
        On open with no saved result and board idle
            - shows the start view with only Load topology and the "No saved result" line
        On open with a saved result
            - shows Show last result and Load topology with the "Last result" line
        On open while the board is running
            - shows the loading view and polls GET without sending POST
    Load topology
        On click
            - sends POST, shows the loading view, polls GET every second while running
        On done
            - stores topology:last, shows the loaded view with the green pill "Loaded at HH:MM"
        On failed
            - shows the error in the start view and keeps the last result
        On 409
            - shows conflictText in the start view
    Show last result
        On click
            - shows the loaded view with the amber pill "Saved result from HH:MM, N min ago" and no request
    loaded view
        On partial
            - the pill adds "some routers didn't answer"
        On a device clicked on the map or its name in the table
            - opens the dialog for that device, dims everything outside its neighbourhood, tints its row
        On filter chip or search input
            - re-renders the rows; on no match shows "No device matches '<q>'"
        On Copy
            - writes the address to the clipboard and the button reads "Copied"
    dialog
        On a neighbour, child or parent link
            - switches to that device
        On Ping
            - POSTs /ping {address, count:4, size:32} and shows pingSummary
        On Rename then Save
            - stores the nickname under the ext address; an empty name removes it; names update everywhere
        On close (×, Close, Esc, backdrop)
            - closes, keeps the selection ring and row tint
        On Show on map
            - closes and scrolls to the map
        On a child whose parent gave no detail
            - shows "Details unavailable: its parent didn't answer"
        On no routable address
            - shows the amber "No routable address yet" note
    storage unavailable
        On a throwing localStorage
            - works without saved result and names and says names can't be saved
```

---

### Task 1: `mesh_scan` result types and JSON render

**Files:**
- Create: `p4/components/mesh_scan/{CMakeLists.txt,include/mesh_scan.h,scan_types.h,render.h,render.c}`
- Create: `p4/components/mesh_scan/test_host/{CMakeLists.txt,main/CMakeLists.txt,main/test_render.c,main/sample_result.c}`

**Interfaces:**
- Produces:
  - `mesh_scan.h`:
    - `typedef enum { MESH_SCAN_IDLE, MESH_SCAN_RUNNING, MESH_SCAN_DONE, MESH_SCAN_FAILED } mesh_scan_state_t;`
    - `typedef enum { MESH_SCAN_ERR_NONE, MESH_SCAN_ERR_BUSY, MESH_SCAN_ERR_STOPPED, MESH_SCAN_ERR_NO_MEMORY } mesh_scan_error_t;`
    - `typedef esp_err_t (*mesh_scan_sink_t)(void *ctx, const char *data, size_t len);`
    - `esp_err_t mesh_scan_init(void); esp_err_t mesh_scan_start(void); void mesh_scan_cancel(void); esp_err_t mesh_scan_render(mesh_scan_sink_t sink, void *ctx);`
      Task 2 implements them; this task only declares them.
  - `scan_types.h`:
    - Caps: `MESH_SCAN_MAX_ROUTERS 63` (indexed by router ID), `MESH_SCAN_MAX_CHILDREN 128`,
      `MESH_SCAN_MAX_ROUTER_ADDRS 16`, `MESH_SCAN_MAX_CHILD_ADDRS 8`, `MESH_SCAN_MAX_SRP_HOSTS 64`,
      `MESH_SCAN_MAX_SRP_ADDRS 8`, `MESH_SCAN_MAX_SRP_SERVICES 8`, `MESH_SCAN_NAME_LEN 64`, `MESH_SCAN_MAX_OMR 4`.
    - Structs:
      - `mesh_scan_child_t`: rloc16, ext[8], has_ext, mode bits, lq_in, timeout_s, age_s, connected_s, rssi_avg,
        rssi_last, link_margin_db, frame_err, msg_err (raw 0..0xffff), version, csl, addrs[], n_addrs.
      - `mesh_scan_router_t`: present, responded, rloc16, router_id, ext[8], version, leader, this_device,
        border_router, lq_in[63], addrs[], n_addrs, children_detail, children (PSRAM array), n_children.
      - `mesh_scan_srp_host_t`: name, addrs[], services[] (instance, type).
      - `mesh_scan_network_t`: name, channel, partition_id, leader_router_id, ml_prefix, this_rloc16, omr[], n_omr.
      - `mesh_scan_result_t`: network, routers[63], srp[], n_srp, partial.
    - `typedef struct { mesh_scan_state_t state; mesh_scan_error_t error; uint8_t routers, queried; uint32_t age_s; const mesh_scan_result_t *result; } mesh_scan_view_t;`
  - `render.h`: `esp_err_t mesh_scan_render_view(const mesh_scan_view_t *view, mesh_scan_sink_t sink, void *ctx);`

- [ ] **Step 1:** Write `test_host` (layout as in `metrics/test_host`, `REQUIRES mesh_scan`). Add a
  `sample_result.c` holding the spec's sketch mesh: this board 0x8800, the leader 0x2400, three other routers, nine
  children, one stub router, one `children_detail:false` router, two SRP hosts (one named `a"b\<c>`), and both OMR
  prefixes. Write one Unity test per `mesh_scan_render` effect in the BDD skeleton. Compare against expected JSON
  strings, and for the escaping case also check that the output parses (using the cJSON that the IDF linux target
  ships).
- [ ] **Step 2:** `logrun make test-host`. Expected: the `mesh_scan` project fails to build or its tests fail.
- [ ] **Step 3:** Write `CMakeLists.txt` (the `linux` branch builds `render.c` only for now), `mesh_scan.h`,
  `scan_types.h` and `render.c`.
  - Writes go into a 2048-byte stack buffer that is flushed to the sink.
  - IPv6 text: `inet_ntop`-style RFC 5952 compression. On target, `otIp6AddressToString` is not usable on linux, so
    write a small local formatter.
  - Error rates: percent = raw × 100 / 65535, rounded to one decimal.
- [ ] **Step 4:** `logrun make test-host`. Expected: every project passes and `mesh_scan` reports 0 failures.
- [ ] **Step 5:** `make lint`, then commit `feat(mesh_scan): result types and JSON render`.

### Task 2: `mesh_scan` state machine

**Files:**
- Create: `p4/components/mesh_scan/{scan.h,scan.c}`, `p4/components/mesh_scan/test_host/main/{fake_ops.c,fake_ops.h,test_scan.c}`
- Modify: `p4/components/mesh_scan/CMakeLists.txt` (the `linux` branch adds `scan.c`)

**Interfaces:**
- Consumes: everything Task 1 produces.
- Produces `scan.h` (private, used by `ot_ops.c` and the tests):
  - `typedef struct mesh_scan_ops mesh_scan_ops_t;` with these function pointers:
    - `int discover(void)`: OpenThread error code.
    - `int query_child_table(uint16_t rloc16)`
    - `int query_child_addrs(uint16_t rloc16)`
    - `void cancel(void)`
    - `uint8_t router_table(uint16_t rloc16_out[63], uint8_t ids_out[63])`
    - `void snapshot(mesh_scan_result_t *r)`: fills network, OMR and SRP.
    - `bool br_running(void)`
    - `int64_t now_ms(void)`
    - `void arm_cap(uint32_t ms)` and `void disarm_cap(void)`
    - `void *alloc(size_t)` and `void release(void *)`
  - Callback entry points, called by `ot_ops.c` from the OpenThread task:
    - `void mesh_scan_on_router(const mesh_scan_router_t *r)`
    - `void mesh_scan_on_discover_done(int ot_error)`
    - `void mesh_scan_on_child_entry(const mesh_scan_child_t *c)`
    - `void mesh_scan_on_child_table_done(int ot_error)`
    - `void mesh_scan_on_child_addrs(uint16_t child_rloc16, const uint8_t (*addrs)[16], uint8_t n)`
    - `void mesh_scan_on_child_addrs_done(int ot_error)`
    - `void mesh_scan_on_cap(void)`
  - `void mesh_scan_set_ops(const mesh_scan_ops_t *ops)`
  - `void mesh_scan_view(mesh_scan_view_t *out)`: a copy made under the mutex. `result` points at the stable result
    and is valid until the next `mesh_scan_start`, which `device_api` never calls while rendering, because render and
    start both take the scan mutex.
- OpenThread error codes are plain `int` in `scan.h`, so the host test needs no OpenThread headers. The values are
  `OT_ERROR_NONE 0`, `NO_BUFS 3`, `BUSY 5`, `INVALID_STATE 13`, `RESPONSE_TIMEOUT 28`, copied as named constants
  `MESH_SCAN_OT_*` with a static assert in `ot_ops.c`.

- [ ] **Step 1:** Write `fake_ops.c`. It records calls, holds scripted return codes, keeps a manual clock and uses a
  heap allocator that can be told to fail. Then write `test_scan.c` with one test per effect under `mesh_scan`,
  `discover callback`, `per-router queries`, `snapshot`, `mesh_scan_cancel`, `cap timer`, `any callback` and `age_s`.
  Add the Review Focus 3 test: 130 child entries for one router keep 128 and set `children_detail:false` and `partial`.
- [ ] **Step 2:** `logrun make test-host`. Expected: the `test_scan` cases fail.
- [ ] **Step 3:** Implement `scan.c`. A FreeRTOS mutex guards the state, and every entry point takes it. The result is
  allocated through `ops.alloc`. The state machine:

  ```
  start: refuse unless br_running; refuse if running; release old; alloc; state=running; arm_cap(60000)
         err = discover(); if err==BUSY -> failed(busy); if err -> failed(stopped)
  discover_done: add stubs for router_table ids not present; next_router(lowest present responded id)
  next_router(id): if none -> finish(cap_hit=false)
                   err = query_child_table(rloc16(id))
                   NO_BUFS|BUSY -> mark no detail; next_router(following id)
                   INVALID_STATE -> failed(stopped)
  child_table_done(err): if err -> mark no detail; next_router(following) else query_child_addrs(same) (same start-error rules)
  child_addrs_done(err): if err -> mark no detail; queried++; next_router(following)
  every callback first: if !br_running -> cancel(); failed(stopped); return
  cap: cancel(); br_running ? finish(cap_hit=true) : failed(stopped)
  finish: snapshot(); partial = cap_hit || any stub || any no-detail; end=now; disarm_cap; state=done
  ```

- [ ] **Step 4:** `logrun make test-host`. Expected: all pass.
- [ ] **Step 5:** `make lint`, then commit `feat(mesh_scan): scan state machine`.

### Task 3: `mesh_scan` OpenThread glue

**Files:**
- Create: `p4/components/mesh_scan/ot_ops.c`
- Modify: `p4/components/mesh_scan/CMakeLists.txt` (the target branch adds `ot_ops.c`;
  `PRIV_REQUIRES openthread br_core esp_timer freertos log`)

**Interfaces:**
- Consumes: `scan.h` (Task 2).
- Produces: `mesh_scan_init()` (creates the mutex and the one-shot cap `esp_timer`, then installs the real ops) and the
  public `mesh_scan_start`, `mesh_scan_cancel` and `mesh_scan_render`. `mesh_scan_render` = view under the mutex, then
  `mesh_scan_render_view`.

- [ ] **Step 1:** Implement the ops.
  - `discover`: `otMeshDiagDiscoverTopology` with `mDiscoverIp6Addresses` and `mDiscoverChildTable`. Its callback
    converts each `otMeshDiagRouterInfo` (child iterator, IPv6 iterator, `mLinkQualities`) into a stack
    `mesh_scan_router_t`, calls `mesh_scan_on_router`, and on the final call (error ≠ `OT_ERROR_PENDING`) calls
    `mesh_scan_on_discover_done`.
  - `query_child_table` and `query_child_addrs`: `otMeshDiagQueryChildTable` and `otMeshDiagQueryChildrenIp6Addrs`,
    with their callbacks mapped the same way.
  - `router_table`: `otThreadGetRouterInfo` for 0..`otThreadGetMaxRouterId`.
  - `snapshot`:
    - network name, channel, partition and leader from `otThreadGetLeaderData`;
    - `otThreadGetMeshLocalPrefix` and `otThreadGetRloc16`;
    - OMR = `otNetDataGetNextOnMeshPrefix` entries with `mSlaac`;
    - SRP = `otSrpServerGetNextHost`, skipping `otSrpServerHostIsDeleted`, name from `otSrpServerHostGetFullName` cut
      at the first `.`, services via `otSrpServerHostGetNextService` (instance and service name), skipping deleted
      ones.
  - `br_running`: `br_core_get_mode() == BR_MODE_BR_RUNNING`.
  - `alloc`: `heap_caps_calloc(…, MALLOC_CAP_SPIRAM)`.
  - `arm_cap`: `esp_timer_start_once`. The timer callback posts to the OpenThread task with `esp_openthread_task_queue_post`
    (or takes the OpenThread lock) before calling `mesh_scan_on_cap`, so every scan callback runs in one context.
  - `start` and `cancel`: called by `device_api` from the httpd task, so they take the OpenThread lock
    (`esp_openthread_lock_acquire(pdMS_TO_TICKS(100))`, `ESP_ERR_TIMEOUT` on failure) around the OpenThread calls.
- [ ] **Step 2:** Add `mesh_scan` to the p4 build (`p4/main/CMakeLists.txt` or `device_api`'s `PRIV_REQUIRES`; follow
  how `metrics` is pulled in).
- [ ] **Step 3:** `logrun make build`. Expected: exit 0 with no new warnings. Then `logrun make test-host`: all pass.
- [ ] **Step 4:** `make lint`, then commit `feat(mesh_scan): OpenThread mesh diagnostics glue`.

### Task 4: `/device/topology` endpoint

**Files:**
- Create: `p4/components/device_api/topology.c`, `p4/components/device_api/test_host/main/test_topology.c`
- Modify: `p4/components/device_api/device_api.c` (routes; `mesh_scan_cancel()` at the top of the radio-push, ROM-recover
  and pcap handlers after their own checks pass and before they call `br_core`/`sniffer`; `mesh_scan_init()` where the
  server starts), `device_api_priv.h`, `CMakeLists.txt`, `docs/api.md`

**Interfaces:**
- Consumes: `mesh_scan_start`, `mesh_scan_cancel`, `mesh_scan_render` (Task 3), `device_api_op_busy_detail`, the
  existing error writer and chunk helpers.
- Produces:
  - `typedef struct { int status; const char *error; char detail[64]; } device_api_topology_reply_t;`
  - `void device_api_topology_start_reply(esp_err_t start_err, br_core_mode_t mode, bool long_op_busy, const char *long_op_detail, device_api_topology_reply_t *out);`
    This is pure and host-tested.
  - Handlers `topology_post_handler` and `topology_get_handler`.
  - `POST` checks in this order: mode, then long-op slot, then `mesh_scan_start`. Only then does it map the reply.

- [ ] **Step 1:** Write `test_topology.c` with one test per effect under `device_api_topology_start_reply`. Include
  the Review Focus 4 case: `ESP_ERR_INVALID_STATE` with `BR_RUNNING` gives 202.
- [ ] **Step 2:** `logrun make test-host`. Expected: the new tests fail.
- [ ] **Step 3:** Implement `topology.c` and the route rows
  `{"/device/topology", HTTP_POST, topology_post_handler}` and `{"/device/topology", HTTP_GET, topology_get_handler}`.
  Add the cancel calls and `mesh_scan_init()`. Add the two rows to `docs/api.md`, with their errors as in Global
  Constraints.
- [ ] **Step 4:** `logrun make test-host`, then `logrun make build`. Expected: both pass.
- [ ] **Step 5:** `make lint`, then commit `feat(device_api): /device/topology`.

### Task 5: `TopologyLogic`

**Files:**
- Create: `p4/components/esp_ot_br_server/frontend/static/topology.js` (logic half only, plus the `module.exports`
  tail), `tools/tests/test_topology_page.py` (logic classes)
- Modify: `tools/tests/device_pages_harness.js` (`SCRIPTS.topology = 'topology'`), `tools/tests/device_js.py` if it
  lists the scripts

**Interfaces:**
- Produces `TopologyLogic` with:
  - `addressClass(addr, network)`
  - `devices(result)` → `[{id, kind, rloc16, ext, routerId, parentId, thisBoard, responded, detail, child: {...}, addrs, version, mode}]`
  - `routerLinks(result)` → `[{a, b, lqAB, lqBA}]`
  - `linkText(link)`
  - `displayName(device, names, srpHosts)`
  - `srpHostFor(device, srpHosts)`
  - `rows(devices, filter, query, names, srpHosts)`
  - `layout(devices, links)` → `{width, height, pos: {id: {x, y}}}`
  - `savedResult(storage)`
  - `saveResult(storage, result, nowMs)` → bool
  - `loadNames(storage)`
  - `saveName(storage, ext, name)` → bool
  - `pickLast(saved, boardState, nowMs)`
  - `conflictText(status, body)`
  - `pingSummary(pingResult, sleepy)`
  - `clockText(ms)` ("HH:MM")
  - `agoText(ms, nowMs)` ("38 min ago")
  - `lqColor(q)`

  The `kind` values are `br|leader|router|stub|med|sed`.

- [ ] **Step 1:** Write `TestTopologyLogic` tests, one per `TopologyLogic` effect, using fixtures built from the
  Task 1 sample mesh as a JS literal in the test module. Add the Review Focus tests:
  - garbage, old-shape and throwing `topology:last` → `null` (RF2);
  - a link to an unknown router ID is dropped (RF5);
  - `layout` called twice gives identical output;
  - the source contains no `Math.random`.
- [ ] **Step 2:** `logrun make test-tools`. Expected: the new tests fail.
- [ ] **Step 3:** Implement the logic. The layout follows this pseudocode, with constants of the builder's choice:

  ```
  centre = this board if it is a router, else the leader
  others = routers except centre, sorted by router id; place on a ring of radius R around the centre at equal angles
  repeat K times (fixed K, no randomness):
      for each router pair: push apart by Kr / distance^2
      for each router link: pull together by distance^2 / Kl
      move each non-centre router by its force, clamped to step S (S shrinks linearly to 0)
  for each router p with children c1..cn sorted by rloc16:
      away = angle from centre to p (centre itself: straight down)
      spread children over an arc of min(270°, n·25°) centred on away, at radius r (alternate r, r+15 when n > 6)
  width/height = bounding box + margin, at least 640×420
  ```

- [ ] **Step 4:** `logrun make test-tools`. Expected: all pass.
- [ ] **Step 5:** `make lint`, then commit `feat(web): topology logic`.

### Task 6: Topology page

**Files:**
- Modify (rewrite): `p4/components/esp_ot_br_server/frontend/topology.html`
- Modify: `static/topology.js` (adds `TopologyPage(env)`), `static/style.css`, `tools/tests/device_pages_harness.js`
  (fake `<dialog>` with `showModal`, `close` and `open`; fake `localStorage`; `navigator.clipboard.writeText` recorder),
  `tools/tests/test_topology_page.py` (page class), `tools/tests/test_device_pages.py` (`PAGES["topology"]` with
  endpoints `/device/topology`), `p4/components/PATCHES.md`

**Interfaces:**
- Consumes: `TopologyLogic` (Task 5), `DeviceCommon`/`DeviceKit` (request wrapper, `setText`, `el`, `clear`), and the
  endpoints in Global Constraints.
- Produces: `TopologyPage(env)` → `{init, kit}`, and these element ids, which the tests use:
  - views: `t-start`, `t-loading`, `t-loaded`;
  - `t-last-line`, `t-show-last`, `t-load`, `t-start-note`, `t-progress`, `t-progress-bar`, `t-pill`, `t-reload`;
  - tiles: `t-tile-network`, `t-tile-devices`, `t-tile-leader`, `t-tile-prefixes`;
  - map: `t-map` (the svg), `t-legend`;
  - table: `t-chips`, `t-search`, `t-rows`, `t-empty`;
  - dialog: `t-dialog`, `t-d-name`, `t-d-rename`, `t-d-name-input`, `t-d-save`, `t-d-chips`, `t-d-facts`,
    `t-d-neighbors`, `t-d-children`, `t-d-addrs`, `t-d-more`, `t-d-ping`, `t-d-showmap`, `t-d-close`.

- [ ] **Step 1:** Write `TestTopologyPage`, one test per `TopologyPage` effect in the BDD skeleton. Add the Review
  Focus tests:
  - an SRP name `<b>"x"</b>` appears only as text (no markup insertion; RF1);
  - opening while the board is `running` sends no `POST` (RF4).

  Add `topology` to `PAGES` in `test_device_pages.py`, so it gets the ids-exist, no-`innerHTML`, live-region and
  endpoint checks.
- [ ] **Step 2:** `logrun make test-tools`. Expected: the new tests fail.
- [ ] **Step 3:** Rewrite `topology.html` to the sketch: nav, header with the pill, the three views, tiles, the map
  card with legend, the table card, and `<dialog id="t-dialog">`. Scripts: `api.js`, `device_common.js`,
  `topology.js`. Implement `TopologyPage`:
  - SVG nodes are `<g tabindex="0" role="button" aria-label>` with Enter and Space handlers.
  - Text goes in with `textContent` only.
  - Append the styles to `style.css` using the existing tokens.
  - Add a `PATCHES.md` entry: "`topology.html` rewritten, `static/topology.js` added (topology page redesign, spec
    2026-11-02); `/topology` no longer used by the page".
- [ ] **Step 4:** `logrun make test-tools`. Expected: all pass.
- [ ] **Step 5:** Check the page by eye against a canned result. Start any static server on the frontend directory with
  a stub `/device/topology` that returns the Task 1 sample JSON, and walk through start, loading, loaded, dialog, search
  and phone width (375 px) in the browser pane. Compare with the sketch. Look only; there is no board.
- [ ] **Step 6:** `make lint`, then commit `feat(web): topology page redesign`.

### Task 7: Target tests and docs

**Files:**
- Create: `p4/pytest/test_topology.py`
- Modify: `p4/pytest/test_latency.py` (load `device_topology_requests`: `POST`, then poll `GET` until done, repeated),
  `p4/pytest/test_br_boot.py` (add `/device/topology` GET to the served-paths list), `docs/troubleshooting.md`
  (topology latency note now says the scan sends one unicast query per router; the old `/topology` multicast is no
  longer used by the page), `docs/api.md` (Web UI paragraph: device table and dialog)

- [ ] **Step 1:** Write `test_topology.py` against the rig (see the ESP32 OTBR test rig memory).
  - The `POST` then `GET` loop ends `done` within 60 s.
  - Every rig router appears.
  - No two routers are linked unless one lists the other with `lq_in > 0`.
  - The rig's end device appears with at least one OMR address.
  - `POST` during `running` returns 202.
  - `POST` in `SNIFFER` returns 409 `mode SNIFFER`.

  Make sure it collects: `logrun pytest --collect-only p4/pytest/test_topology.py` (no board).
- [ ] **Step 2:** Make the docs edits listed above.
- [ ] **Step 3:** `logrun make test`. Expected: all host, tools and vendored tests pass.
- [ ] **Step 4:** `make lint`, then commit `test(topology): target tests and docs`.

### Task 8: Hardware pass (only after the whole-branch review and its fixes)

- [ ] **Step 1:** Write the flash budget: one P4 app flash of a `build-test` image, no C6 writes.
- [ ] **Step 2:** `logrun make test-target TEST=p4/pytest/test_topology.py`, then
  `logrun make test-target TEST=p4/pytest/test_latency.py MARK=<the topology loads only>`. Expected: pass; the
  `device_topology_requests` p99 added latency is reported next to `topology_requests`.
- [ ] **Step 3:** Open the page on the board. Take `docs/images/topology.png` in the loaded view with the dialog closed.
- [ ] **Step 4:** Commit `docs: topology screenshot`.
