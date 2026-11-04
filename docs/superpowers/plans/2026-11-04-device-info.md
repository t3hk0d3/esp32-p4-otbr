# Device info in the topology scan: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
> After approval this file is copied to `docs/superpowers/plans/2026-11-04-device-info.md` on `feat/device-info`.

## Context

The Topology page names devices from SRP records. Matter devices on the HA server, plain routers and sensors that
register nothing stay "Unknown" or "Matter device 0xNNNN", and nothing says what chip or stack a device runs. On
2026-11-04 every device on `ha-thread-bbc1` answered a Network Diagnostic Get for the vendor TLVs. That included a
sleepy child, which answered through its parent after its next poll. The **Thread stack version** names the vendor and
chip on every device; vendor name and model are filled in by some. This board sends a junk vendor name, "RD:".

**Goal:** `GET /device/topology` carries per router and per child an `info` object: EUI-64, vendor name, model, SW
version, Thread stack version, app URL, OUI and a state. The scan waits for sleepy children and says so in its progress.
This board sends its own vendor strings. The Topology page shows the details, uses them in names, the kind line and
search.

**Architecture:** the scan's child address phase (`PHASE_CHILD_DIAG` in `scan.c`) becomes a device info phase with two
queues: awake devices (4 in flight, 5 s) and sleepy children (16 in flight, the child's timeout, a 300 s sleepy cap).
`ot_ops.c` sends one diag GET per device (vendor TLVs, plus TLV 8 when addresses are missing) and parses the answer.
`render.c` writes `info` and the progress counts. `topology.js` gains pure decoders and dialog facts.
`esp_ot_custom_config.h` and `br_core` give this board its strings.

**Tech stack:** ESP-IDF v6.1, OpenThread netdiag (through `netdiag_mux`), Unity host tests, node page tests.

**Spec:** `docs/superpowers/specs/2026-11-04-device-info-design.md` (`dbb4897`, branch `feat/device-info`, stacked on
`feat/srp-details`). Read it with this plan.

**Styleguide:** `STYLEGUIDE.md`:
- astyle via `make lint`, `snake_case`, the `mesh_scan_` / `br_core_` prefixes, `esp_err_t` + `ESP_RETURN_ON_ERROR`;
- host tests in `<component>/test_host/main/`, named `"<scenario>: <effect>"`, tagged `[mesh_scan]` or `[br_core]`;
- the ops seam is mesh_scan's only test seam;
- `topology.js` stays ES5, page tests in `tools/tests/test_topology_page.py`;
- vendored `esp_ot_br_server` changes get a `p4/components/PATCHES.md` entry (none expected beyond the page).

## Rulings against the spec (amend it in Task 1)

1. **No mesh-diag lock to release.** The spec's "Mesh diagnostics while waiting" row assumed the scan holds the 409
   `busy` lock while it waits. It doesn't:
   - the 409 comes from OpenThread's single `otMeshDiag` slot (`ot_ops.c:355`), which is free once the child tables
     are done;
   - the info phase uses only `netdiag_mux_send_get`.

   Replace the row with: "During the info phase the scan holds no `otMeshDiag` operation, so REST `/topology` and
   diagnostics work. `POST /device/topology` joins the running scan (202), as today."
2. **SW version is set at run time.** The spec says strings are set "at build time". A build-time SW version would be a
   compile definition that changes with every commit and rebuilds every component. Instead:
   - name, model and app URL are build-time defines in `esp_ot_custom_config.h`;
   - `OPENTHREAD_CONFIG_NET_DIAG_VENDOR_INFO_SET_API_ENABLE 1` is defined explicitly;
   - `br_core` calls `otThreadSetVendorSwVersion` after `esp_openthread_init` with `YYYYMMDD-<7 hex>`, derived from
     `esp_app_get_description()->version`.
3. **Turning off the reference device has known side effects**, found in OpenThread's source. Pin them so nothing
   changes silently:
   - with the flag off, SRP client buffer limits fall from 8 to 2 services and from 10 to 2 host addresses. The
     firmware uses no SRP client, but `esp_ot_custom_config.h` pins today's values anyway (`..._MAX_SERVICES 8`,
     `..._MAX_HOST_ADDRESSES 10`);
   - the TMF origin filter becomes unconditional;
   - `otThreadGetParentInfo` needs the child role (unused here);
   - the test build's OT CLI loses its test-harness commands. Task 3 greps `p4/pytest` for `ot <cmd>` uses against the
     gated CLI commands; if any is used, keep the flag on and use the name `RD:ESP32-P4-WIFI6-POE-ETH`.

## Global constraints

- TLVs asked: 23 (EUI-64), 25, 26, 27, 28, 35 (app URL), 44 (vendor OUI); plus 8 (IPv6 address list) for children with
  no addresses.
- Who: every responding router except this board, and every child. `rx_on_when_idle` picks the queue.
- Awake queue order:
  1. rx-on children without addresses (router ID, child order);
  2. routers by router ID;
  3. the other rx-on children.

  Sleepy queue: router ID, then child order.
- Awake queue: 4 in flight, 5000 ms deadline each (`MESH_SCAN_CHILD_QUERIES`, `MESH_SCAN_CHILD_DEADLINE_MS`).
- Sleepy queue: `MESH_SCAN_SLEEPY_QUERIES` 16 in flight. Each one's deadline is the child's `timeout_s` from the child
  table (0 → 240 s), bounded by `MESH_SCAN_SLEEPY_CAP_MS` = 300 000 ms from scan start.
- 60 s cap: when it hits with only sleepy queries left (awake queue empty, nothing awake in flight, mesh phases done
  otherwise), the scan keeps running until the sleepy cap. Otherwise it finishes as today.
- `info.state`:
  - `answered`: any reply from the device;
  - `no_answer`: deadline passed or the send failed;
  - `cut_short`: still queued at a cap;
  - `not_asked`: a router that didn't answer discover.
- This board is always `answered`, from local values.
- Info never sets `partial`. `partial` stays true when the cap hits while an rx-on child without addresses has not
  answered (today's behaviour).
- Strings: empty is absent; OUI only as a 24-bit value; JSON through `render.c`'s `str()` (escapes, U+FFFD repair).
- This board:
  - vendor name `ESP32-P4-WIFI6-POE-ETH`;
  - model `OpenThread Border Router`;
  - app URL `https://github.com/t3hk0d3/esp32-p4-otbr`;
  - SW version `YYYYMMDD-<7 hex>` from `PROJECT_VER`; `-dirty` dropped; a malformed version → the first 16 bytes.
- Progress JSON while running: `progress.info = {total, answered, sleepy_waiting}`.
- Page status text: "Waiting for N sleepy device(s) (they answer when they next wake, up to M min)" while only sleepy
  queries are left; "Asking devices for details: A of T" before that.
- No flashing inside tasks; one batched hardware pass after review.
- Commits end with only `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review focus

1. **Late answers.** A sleepy answer that arrives after its deadline, after the cap, after cancel, or for an older
   generation is dropped and never touches a freed or newer result. Owner: Task 2.
2. **The 60 s cap with sleepy children left.** The scan keeps running and the progress says it is waiting. Awake
   devices still queued at the cap are `cut_short`. At the sleepy cap, everything still waiting is `no_answer`.
   Owner: Task 2.
3. **Answer matching.** Answers are matched by the peer's RLOC16. A device whose RLOC16 changed mid-scan (a child that
   re-attached) or that sends an unasked reply must not overwrite another device. Owner: Task 2.
4. **Odd strings.** A 64-byte stack version, non-UTF-8 bytes, `<b>` in a vendor name, a `javascript:` app URL. Board:
   valid JSON. Page: `textContent` only, links only for `http`/`https`. Owners: Tasks 3 and 4.
5. **Older saved results** (no `info`, no `progress.info`) render as before. Owner: Task 4.

## BDD skeleton

```
br_core vendor info (vendor_info.c, host)
    br_core_vendor_sw_version(project_ver, out[17])
        "20261104T095955Z-bb169ca"
            - "20261104-bb169ca"
        "20261104T095955Z-bb169ca-dirty"
            - "20261104-bb169ca" (dirty dropped)
        Malformed or short ("abc", "")
            - first 16 bytes, NUL-terminated; never writes past out[16]

mesh_scan core (scan.c, host, fake ops)
    start of the info phase (after the last child table)
        Routers 0x1000, 0x3000, this board; rx-on child without addresses; rx-on child with addresses; two sleepy children
            - query_info for the rx-on child without addresses first, with want_addrs true
            - then routers by ID, this board skipped, want_addrs false
            - both sleepy children asked at once, want_addrs per their addresses
            - progress.info.total = 6, answered 0, sleepy_waiting 2
            - child deadline armed for the earliest due: 5000 ms awake, sleepy = child timeout_s
        Sleepy child with timeout_s 0
            - its deadline is 240 s
        More than 16 sleepy children
            - 16 in flight, the rest queued; a reply frees a slot for the next
        query_info returns NO_BUFS for one device
            - that device no_answer, the next one is asked
        query_info returns INVALID_STATE
            - scan fails stopped (as today)
        No routers besides this board, no children
            - phase ends at once; scan finishes (or waits for SRP as today)
    mesh_scan_on_info
        Reply from an in-flight router with all TLVs
            - router info answered with every field; slot freed; next device asked; answered++
        Reply from an in-flight child with addresses (TLV 8)
            - addresses appended as today; addresses_reported true; info answered
        Reply with no TLVs
            - info answered, every field absent
        Empty vendor strings
            - stored as absent
        Reply from an RLOC16 not in flight, or an older generation, or after finish
            - ignored, nothing written
        Last reply while the SRP step still runs
            - waits for SRP (PHASE_SRP_WAIT as today)
    mesh_scan_on_child_deadline
        An awake query past due
            - info no_answer; an rx-on child without addresses: addresses_reported false; next asked
        A sleepy query past due
            - info no_answer; sleepy_waiting--
    mesh_scan_on_cap (60 s)
        Awake queries still in flight or queued
            - in flight: no_answer; queued: cut_short; sleepy waiting: no_answer; finish; partial as today
        Only sleepy queries left
            - scan keeps running; progress shows sleepy_waiting; sleepy cap armed for 300 s from start
        A sleepy answer after that
            - stored; when the last one is in, the scan finishes, partial false
    sleepy cap (300 s)
        - every sleepy query still waiting: no_answer; finish
    cancel during the sleepy wait
        - scan stops as today; later answers ignored
    finish
        - this board's router info answered with the local values from ops.snapshot
        - routers that didn't respond: not_asked

mesh_scan render (render.c, host)
    router or child with info answered
        - "info": {"state":"answered", eui64 16 hex, vendor_oui 6 hex, strings} with absent fields left out
    info no_answer / cut_short / not_asked
        - "info": {"state":"<that>"} only
    string with a quote, a control byte, invalid UTF-8
        - escaped, U+FFFD, valid JSON
    running view
        - "progress" gains "info":{"total":T,"answered":A,"sleepy_waiting":S}

TopologyLogic (topology.js, node)
    stackInfo
        "SL-OPENTHREAD/2.4.2.0_GitHub-7074a43e4; EFR32; Oct 30 2024 14:04"
            - {vendor "Silicon Labs", chip "EFR32", sdk "SL-OpenThread", version "2.4.2.0"}
        "openthread-esp32/fff9895c-b678a4f; esp32c6;  2026-11-03 21:47:25"
            - {vendor "Espressif", chip "esp32c6", sdk "OpenThread", version null}
        "OPENTHREAD/cb18e0f; POSIX; Oct  6 2026 20:28:48"
            - {vendor "OpenThread", chip "POSIX", sdk "OpenThread", version null}
        Unknown or empty or absent
            - null
    infoName
        vendor name + model, name not "RD:" alone
            - "Home Assistant OpenThread Border Router"
        "RD:" prefix with a model
            - prefix stripped
        model absent, or name only "RD:"
            - null
    nameInfo
        No SRP name, info name present
            - info name (before "Matter device 0xNNNN")
        SRP friendly_name present
            - SRP name wins
        This board
            - hostname, unchanged
    kindLine
        No SRP kind, stack known
            - "Silicon Labs EFR32"
        SRP kind present
            - unchanged
    matches
        query matches vendor name, model or stack string
            - device listed
    appUrl
        http(s) URL
            - the URL
        javascript:, data:, or no scheme
            - null

TopologyPage (topology.js, node)
    progress line
        progress.info.total > 0, awake left
            - "Asking devices for details: A of T"
        only sleepy waiting
            - "Waiting for 2 sleepy devices (they answer when they next wake, up to 4 min)"
        progress without info (older firmware)
            - today's texts
    device dialog
        info answered with vendor strings
            - "Hardware" vendor + model, "Stack" raw string, "Software" SW version, "EUI-64"
        info with only the stack string
            - "Hardware" from stackInfo ("Silicon Labs EFR32 · SL-OpenThread 2.4.2.0")
        app URL http(s)
            - a link with the URL as text; other schemes: plain text, no link
        info no_answer on a sleepy child
            - "Device info: no answer (sleepy device)"
        result without info
            - none of these facts; no errors
```

## Tasks

### Task 1: Spec amendment and the result types

**Files:**
- Modify the spec (rulings 1-3).
- Modify `p4/components/mesh_scan/scan_types.h`:
  - `mesh_scan_info_state_t` (`NOT_ASKED`, `ANSWERED`, `NO_ANSWER`, `CUT_SHORT`);
  - `mesh_scan_info_t` with `state`, `has_eui64` + `eui64[8]`, `has_oui` + `oui` (24-bit), and `char` arrays
    `vendor_name[33]`, `vendor_model[33]`, `vendor_sw_version[17]`, `thread_stack_version[65]`,
    `vendor_app_url[97]` (empty = absent);
  - a `mesh_scan_info_t info` in `mesh_scan_router_t` and in `mesh_scan_child_t`;
  - `mesh_scan_view_t` gains `info_total`, `info_answered`, `sleepy_waiting`.
- Modify `render.c` (`write_info` called from `write_child` and `write_router`; progress `info`), `test_render.c` and
  `sample_result.c`.

- [ ] Write the `mesh_scan render` tests; `logrun make test-host`: fails to build.
- [ ] Implement; `logrun make test-host`: green. Commit `feat(mesh_scan): device info fields in the scan result`.

### Task 2: The info phase

**Files:**
- Modify `scan.h`:
  - the op `query_child_diag(rloc16, gen)` becomes `query_info(uint16_t rloc16, bool want_addrs, uint32_t gen)`;
  - the callback `mesh_scan_on_child_diag` becomes
    `void mesh_scan_on_info(uint32_t gen, uint16_t peer_rloc16, const mesh_scan_info_t *info,
    const uint8_t (*addrs)[16], uint8_t n)`;
  - `MESH_SCAN_SLEEPY_QUERIES` 16, `MESH_SCAN_SLEEPY_CAP_MS` 300000, `MESH_SCAN_SLEEPY_DEFAULT_MS` 240000;
  - `snapshot` also fills this board's `info` (see Task 3).
- Modify `scan.c`:
  - `PHASE_CHILD_DIAG` → `PHASE_INFO`;
  - two slot arrays, one deadline timer armed for the earliest due slot of both;
  - the cap logic above;
  - `info_*` counters into `view_locked`.
- Modify `test_host/main/fake_ops.c/.h` (record `want_addrs`) and `test_scan.c`. Today's child-step tests are renamed
  and adapted, not deleted.
- In ot_ops.c, only adapt the signature so the build passes; the real body comes in Task 3.

- [ ] Write the `mesh_scan core` tests (Review focus 1-3); `logrun make test-host`: fails.
- [ ] Implement per the pseudocode below. Keep the lock order (OpenThread lock, then scan mutex).
- [ ] `logrun make test-host`: green. Commit `feat(mesh_scan): ask every device for its info, wait for sleepy children`.

```
cap(60 s):    if awake queue empty and no awake slot in flight and sleepy slots in flight or queued:
                  waiting_sleepy = true; arm cap for (start + 300 s - now); return
              else: in-flight -> no_answer, queued -> cut_short (both queues); finish(true)
cap(300 s):   sleepy in flight -> no_answer, queued -> cut_short; finish(false)  # partial unaffected
partial:      cap_hit (60 s) and (phase before INFO, or an rx-on child without addresses still unanswered)
```

### Task 3: Real ops and this board's strings

**Files:**
- Modify `p4/components/mesh_scan/ot_ops.c`:
  - `op_query_info`: the request with the TLV list, TLV 8 only with `want_addrs`;
  - `on_info`: the TLV walk with types 8, 23, 25-28, 35, 44 into a static `mesh_scan_info_t`; OUI kept only when 24-bit;
  - `op_snapshot` fills this board's router info from `otThreadGetVendorName/Model/SwVersion/AppUrl`,
    `otGetVersionString` and `otLinkGetFactoryAssignedIeeeEui64`.
- Modify `p4/main/esp_ot_custom_config.h`:
  - `OPENTHREAD_CONFIG_REFERENCE_DEVICE_ENABLE 0`;
  - `OPENTHREAD_CONFIG_NET_DIAG_VENDOR_NAME`, `..._MODEL` and `..._APP_URL` with the values above;
  - `OPENTHREAD_CONFIG_NET_DIAG_VENDOR_INFO_SET_API_ENABLE 1`;
  - the pinned SRP client buffer values (ruling 3).
- Create `p4/components/br_core/vendor_info.c`, declaring `br_core_vendor_sw_version` in a private header. Call it from
  `br_core.c` after `esp_openthread_init`, under the OpenThread lock, with `otThreadSetVendorSwVersion` (log on error,
  never fail init).
- Add `test_host/main/test_vendor_info.c` and register it in br_core's test_host CMake.

- [ ] Grep `p4/pytest` for `ot <cmd>` commands and check each against the commands that
  `OPENTHREAD_CONFIG_REFERENCE_DEVICE_ENABLE` gates in `openthread/src/cli/*.cpp`. If any is used, take the ruling 3
  fallback and note it in the commit.
- [ ] Write the `br_core vendor info` tests; `logrun make test-host`: fails.
- [ ] Implement; `logrun make test-host`, `logrun make build`, `logrun make build-test`: green. Commit
  `feat: this board's vendor strings in network diagnostics, device info TLVs on the mesh`.

### Task 4: The page

**Files:**
- Modify `p4/components/esp_ot_br_server/frontend/static/topology.js`:
  - `TopologyLogic`: `stackInfo`, `infoName`, `appUrl`, and changes to `nameInfo`, `kindLine`, `matches`;
  - `TopologyPage`: `renderProgress`, `renderFacts`.
- `static/style.css` only if a class is needed.
- Modify `tools/tests/test_topology_page.py`, with the strings from spec section 2 as fixtures.
- `p4/components/PATCHES.md` entry 16 follow-up.

- [ ] Write the `TopologyLogic` and `TopologyPage` tests (Review focus 4-5); `logrun make test-tools`: fail.
- [ ] Implement; `logrun make test-tools`, `logrun make build`: green. Commit
  `feat(web): device info on the Topology page`.

### Task 5: Docs

**Files:** `docs/api.md` (topology `info` and `progress.info`), `CHANGELOG.md`, the plan copy under
`docs/superpowers/plans/`.

- [ ] Write; `logrun make lint` (revert the known unrelated lint noise in `supervisor.c`, `tools/pack_web.py` and
  `tools/tests/test_pack_web.py`). Commit `docs: device info in the topology scan`.

### Task 6: Review, then the hardware pass

- [ ] Whole-branch review on the most capable model; fix Critical/Important findings test-first.
- [ ] Batched hardware pass on `ha-thread-bbc1`: one P4 OTA after checking `app.version` in a separate step; no C6
  flash. The aux C6 is already a sleepy child of 0x1000.
  - Scan: every router has `info`; 0x1000 has its vendor strings; the EFR32 routers have the stack string; the sleepy
    C6 child is `answered`; measure the scan time.
  - Progress: while the sleepy child is pending, `progress.info.sleepy_waiting` is greater than 0 and the page shows
    the waiting text.
  - REST `getNetworkDiagnosticTask` to 0x6800 shows vendor name `ESP32-P4-WIFI6-POE-ETH`, the model, the app URL and
    SW version `YYYYMMDD-<hash>`.
  - The page in a browser: Hardware/Stack facts, kind lines for the EFR32 routers, search by "EFR32".
  - Leader weight and TREL checks from earlier passes still work with the reference-device flag off (`GET` the
    leader-weight value).

## Verification

- `logrun make test-host`: the mesh_scan and br_core suites green.
- `logrun make test-tools`: the topology page tests green.
- `logrun make build`, `logrun make build-test`, `logrun make lint`: OK.
- Hardware (deferred, batched): the Task 6 checks.
