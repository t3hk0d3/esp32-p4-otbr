# Device info in the topology scan: design

Date: 2026-11-04. Status: draft. Branch `feat/device-info`, stacked on `feat/srp-details` (PR #32): it builds on that
branch's scan result, SRP step and page code.

## 1. Purpose and scope

The Topology page names a device from its SRP records (spec `2026-11-04-srp-details-design.md`). That leaves gaps:
- Matter devices on the HA server give no friendly name, only "Matter device 0xNNNN".
- A device that registers nothing has no record at all: plain routers, Thread-only sensors.
- Nothing says which chip or SDK a device runs.

Every Thread device answers the standard Network Diagnostic Get. Its vendor TLVs (name, model, software version,
Thread stack version, app URL, OUI) and its factory EUI-64 often identify the device even when SRP is silent. The
**Thread stack version** names the stack, SDK and chip family on every device seen so far.

This change makes the scan ask every router and every child for those TLVs, carries them in `GET /device/topology`, and
shows them on the Topology page. It also gives this board vendor strings of its own: today it sends "RD:".

In scope:
- the scan step;
- the new per-device fields;
- this board's own vendor strings;
- decoding and display on the page;
- docs.

Out of scope:
- Caching device info between scans (see 6).
- A table mapping EUI-64 prefixes (OUIs) to company names.
- Asking for other diagnostic TLVs: counters, battery, supply voltage.
- Matter or HA APIs.

## 2. What we found (2026-11-04, `ha-thread-bbc1`)

Asked through the board's existing REST action (`POST /api/actions`, `getNetworkDiagnosticTask` to each RLOC). Every
device answered within a second, except the sleepy one, which answered within one poll period:

| Device | Vendor name / model / SW version | Thread stack version | EUI-64 |
| --- | --- | --- | --- |
| 0x1000 HA OTBR | `Home Assistant` / `OpenThread Border Router` / empty | `OPENTHREAD/cb18e0f; POSIX; Oct  6 2026 20:28:48` | 8c73dafffe952cfb |
| 0x3000, 0xE400 (EFR32 Matter) | all empty | `SL-OPENTHREAD/2.4.2.0_GitHub-7074a43e4; EFR32; Oct 30 2024 14:04` | not sent |
| 0x6400 ESPHome C6 | `RD:` / empty / empty | `openthread-esp32/-; esp32c6;  2026-11-04 00:42:30 UTC` | acebe6fffe0bffa4 |
| 0x6800 this board | `RD:` / empty / empty | `openthread-esp32/fff9895c-b678a4f; esp32p4;  2026-11-04 10:00:04` | d885acfffef9cb4c |
| 0x1012 sleepy child of 0x1000 (C6 ot_cli, mode `-`, 5 s poll) | `RD:` / empty / empty | `openthread-esp32/fff9895c-b678a4f; esp32c6;  2026-11-03 21:47:25` | acebe6fffe0bffa4 |

What follows from this:
- The stack version is the field every device fills in. Vendor strings are optional; most devices leave them empty.
- Not every device answers the EUI-64 TLV.
- Answers can contain double spaces (ESP-IDF's format) and empty strings.
- "RD:" is OpenThread's default vendor name for a reference device. ESP-IDF's FTD config enables
  `OPENTHREAD_CONFIG_REFERENCE_DEVICE_ENABLE` by default, and with it any vendor name must start with "RD:" (a
  `static_assert` at build time, `OT_ERROR_INVALID_ARGS` when set at run time).
- A sleepy child answers through its parent's indirect queue, so the answer comes after the child's next poll.
  Matter ICDs and battery sensors can poll every 30 s or less often. 5 s was the only interval tested.
- TLVs 35 (app URL) and 44 (vendor OUI) weren't tested: the REST action has no names for them.

## 3. Decisions

| Topic | Decision |
| --- | --- |
| Who is asked | every router that answered discover, and every child, rx-on or sleepy, except this board |
| This board | read locally: `otThreadGetVendorName/Model/SwVersion/AppUrl`, `otGetVersionString`, `otLinkGetFactoryAssignedIeeeEui64` |
| TLVs asked | EUI-64 (23), vendor name (25), model (26), SW version (27), Thread stack version (28), app URL (35), vendor OUI (44). Children without addresses also get IPv6 address list (8) in the same request, which replaces today's address-only child request (my call: one request per child, not two) |
| Request | `netdiag_mux_send_get` to the device's RLOC (mesh-local prefix + `0:ff:fe00:<rloc16>`), unfiltered, as `op_query_child_diag` does today. The answer is matched to the device by the peer's RLOC16 |
| When | a phase after the child tables, replacing today's child address step (`PHASE_CHILD_DIAG`). It covers routers and children (my call: the queue needs the child tables, and one phase keeps the order simple) |
| Concurrency | two queues. **Awake** (routers and rx-on children): 4 in flight, 5 s deadline each, as today's child step. **Sleepy** (rx-off children): the scan **waits for them** (user, 2026-11-04). All sleepy children are asked at once, up to 16 in flight (my call: each request waits in its parent's indirect queue and costs no airtime until the child polls). Each one waits until the child's own timeout from the child table (a child must poll within it), bounded by the sleepy cap below. The sleepy queue doesn't block the awake one. The phase ends when both are empty and nothing is in flight |
| Which children | every child, with or without addresses. `rx_on_when_idle` from the child table picks the queue. A child that already has addresses gets the request without TLV 8 |
| Order | awake queue: first the rx-on children without addresses (in router ID and child order, as today's step), then routers by router ID, then the other rx-on children (my call: under the cap, addresses come before vendor strings, as today). Sleepy queue: router ID and child order |
| Answered | any reply from the device counts as `answered`, even one with none of the asked TLVs (fields then absent). `no_answer` means only a deadline or a failed send |
| Cap | the existing 60 s scan cap bounds everything except the wait for sleepy answers. Once only sleepy queries are left, the scan keeps waiting up to `MESH_SCAN_SLEEPY_CAP_MS` = 300 s from scan start (my call: covers the 240 s child timeout seen here). A cap finishes the scan with what it has: devices still in flight get `info.state = "no_answer"`, devices still queued `"cut_short"`. Missing info never sets `partial`. `partial` keeps today's meaning: the cap hit while a router or child table, or an rx-on child's addresses (a child without addresses that hasn't answered yet), was still missing (my call) |
| Progress | while running, `progress` gains `info: {total, answered, sleepy_waiting, wait_s}`. `wait_s` is the seconds until the latest sleepy deadline, bounded by the sleepy cap; 0 when no sleepy device waits. The page's status line says e.g. "Waiting for 3 sleepy devices (they answer when they next wake, up to 4 min)", with the minutes from `wait_s` rounded up (left out when `wait_s` is 0 or absent). Cancel works as today |
| Mesh diagnostics while waiting | during the info phase the scan holds no `otMeshDiag` operation, so REST `/topology` and diagnostics work. (The 409 `busy` comes from OpenThread's single `otMeshDiag` slot, which is free once the child tables are done; the info phase uses only Network Diagnostic Gets through `netdiag_mux`.) `POST /device/topology` joins the running scan (202), as today |
| Fields kept | as OpenThread parses them: strings up to 32/32/16/64/96 bytes, OUI as a 24-bit value (other OUI lengths dropped), EUI-64 as 8 bytes. Empty strings are treated as absent |
| Storage | inline fixed-size fields per router and per child: about 245 bytes each, about 47 KB more per result in PSRAM for 63 routers and 128 children (my call: simpler than the TXT pool, and the result already lives in PSRAM) |
| Text safety | strings are written to JSON escaped. Bytes that aren't valid UTF-8 become U+FFFD (my call). The page shows them only through `textContent` |
| This board's own strings | name, model and app URL are build-time defines in `p4/main/esp_ot_custom_config.h`; `OPENTHREAD_CONFIG_NET_DIAG_VENDOR_INFO_SET_API_ENABLE 1` is defined explicitly. Vendor name = the board name `ESP32-P4-WIFI6-POE-ETH` (user, 2026-11-04); model `OpenThread Border Router` (my call, as the HA OTBR); app URL `https://github.com/t3hk0d3/esp32-p4-otbr` (my call). The SW version is set at run time: `br_core` calls `otThreadSetVendorSwVersion` after `esp_openthread_init` with `YYYYMMDD-<7 hex>` (16 characters, the limit; `-dirty` dropped), derived from `esp_app_get_description()->version` (`PROJECT_VER` is 24+ characters). It is not a build-time define: that would change with every commit and rebuild every component. The "RD:" prefix: see 6 |
| Page: summary | a pure `stackInfo(stackVersion)` turns a known stack string into `{vendor, chip, sdk, version}`. Known patterns: `SL-OPENTHREAD/<v>; <chip>;` → Silicon Labs; `openthread-esp32/<hash>; <chip>;` → Espressif; `OPENTHREAD/<hash>; POSIX;` → OpenThread POSIX (a border router host); others → raw string only |
| Page: dialog | a "Hardware" fact: vendor name + model when present, else the stack summary (e.g. "Silicon Labs EFR32 · SL-OpenThread 2.4.2.0"); "Stack" with the raw Thread stack version; "Software" with the vendor SW version; "EUI-64"; the app URL as a link only when it is `http` or `https`. A device that didn't answer gets "Device info: no answer" (sleepy: "no answer (sleepy device)") |
| Page: names | after the SRP names (spec section 3, items 1-5) and before "Matter device 0xNNNN": vendor name + model, when the model is set and the name isn't `RD:` alone, e.g. "Home Assistant OpenThread Border Router" (my call). The stack summary never becomes a name |
| Page: kind line | when SRP gives no kind: the stack summary (e.g. "Espressif esp32c6") |
| Page: search | also matches vendor name, model and the stack string |
| API | additive fields in `GET /device/topology`; no new endpoint |
| Older results | results saved in the browser by older firmware have no `info`; the page shows nothing new for them |

## 4. Scan result

Additions only; existing fields keep their meaning. While running, `progress.info` as in 3.

- Each router and each child gains `info`:
  - `state`: `answered`, `no_answer` (asked, the deadline passed or the send failed), `cut_short` (still queued when
    the cap hit), `not_asked` (a router that didn't answer discover).
  - Present only when answered and non-empty: `eui64` (16 hex digits), `vendor_name`, `vendor_model`,
    `vendor_sw_version`, `thread_stack_version`, `vendor_app_url`, `vendor_oui` (6 hex digits).
- This board's router entry always has `state: answered` with its local values.

Example (a router):

```json
"info": {"state": "answered", "eui64": "8c73dafffe952cfb", "vendor_name": "Home Assistant",
         "vendor_model": "OpenThread Border Router",
         "thread_stack_version": "OPENTHREAD/cb18e0f; POSIX; Oct  6 2026 20:28:48"}
```

## 5. Architecture

- `mesh_scan`:
  - `scan_types.h`: a `mesh_scan_info_t` (state plus the fields above), one inside `mesh_scan_router_t` and one inside
    `mesh_scan_child_t`.
  - The ops seam: `query_child_diag(rloc16, gen)` becomes `query_info(rloc16, want_addrs, gen)`, and its callback
    `mesh_scan_on_info(gen, peer_rloc16, info, addrs, n)` carries the parsed TLVs and any addresses. A second deadline
    op serves the sleepy queue (or one deadline timer armed for the earliest slot of both queues; the builder's choice).
  - `scan.c`: the phase, both queues, matching an answer to a router or a child by RLOC16, and the states.
  - `ot_ops.c`: building the request, walking the TLVs in `on_child_diag` (renamed), and the local read for this
    board in `op_snapshot`.
  - `render.c`: `info`, with JSON escaping and UTF-8 repair.
- `p4/main/esp_ot_custom_config.h`: the vendor defines, and whatever the reference-device question (6) needs.
- `topology.js`:
  - `TopologyLogic`: `stackInfo`, `infoName`, and extensions to `nameInfo`, `kindLine`, `matches`.
  - `TopologyPage`: the dialog facts.
- Tests:
  - host Unity tests for the phase (both queues, deadlines, cap, stale generation, answers matched by RLOC16) and for
    rendering;
  - node page tests with the strings recorded in section 2 as fixtures.
- Docs: `docs/api.md` (topology fields), `CHANGELOG.md`, `p4/components/PATCHES.md` if vendored code changes.

### Phase sketch

```
start_info_phase: awake = rx-on children without addresses, then responding routers (not this board), then other
                  rx-on children; sleepy = rx-off children
                  fill(awake, 4, 5 s); fill(sleepy, 16, child timeout)
fill(queue):      while a slot is free and queue not empty: send; on NO_BUFS/BUSY mark no_answer and go on;
                  on INVALID_STATE fail the scan (stopped), as today
on_info:          drop unless gen matches and the peer's RLOC16 is a device in flight; store; free its slot; fill
deadline:         every slot past due -> no_answer; fill
cap (60 s):       if only sleepy queries are left, keep waiting until the sleepy cap (300 s); else finish
both queues empty, nothing in flight: mesh phases over (finish, or wait for the SRP step as today)
```

## 6. Risks and open points

- **Long scans.** A network with a slow sleeper keeps the scan running for up to 5 minutes. The page shows why, and
  every other answer is already in when it waits. Remembering answers between scans would shorten that; it stays out
  of scope.
- **The "RD:" prefix.** ESP-IDF's FTD config enables `OPENTHREAD_CONFIG_REFERENCE_DEVICE_ENABLE`, which forces the
  prefix. The plan turns it off in `esp_ot_custom_config.h`, after checking that nothing in this firmware (main,
  components, the vendored `esp_ot_br_server`, the CLI build) uses the test-harness features it gates. If something
  does, it stays on and the name is `RD:ESP32-P4-WIFI6-POE-ETH` (my call). Turning it off has known side effects
  (from OpenThread's source), pinned so nothing changes silently:
  - SRP client buffer limits fall from 8 to 2 services and from 10 to 2 host addresses. The firmware uses no SRP
    client, but `esp_ot_custom_config.h` pins today's values anyway (`..._MAX_SERVICES 8`,
    `..._MAX_HOST_ADDRESSES 10`).
  - The TMF origin filter becomes unconditional.
  - `otThreadGetParentInfo` needs the child role (unused here).
  - The test build's OT CLI loses its test-harness commands. The plan greps `p4/pytest` for `ot <cmd>` uses of the
    gated CLI commands; if any is used, the flag stays on and the name is `RD:ESP32-P4-WIFI6-POE-ETH`.
- **Traffic.** One extra request per router and per child, each a few hundred bytes; on a 30-device mesh that is about
  30 messages over a few seconds. Mesh diagnostics already send more than that.
- **Scan time.** It grows by the awake devices divided by 4 times their round trip (well under a second each here),
  plus the slowest sleepy answer. It is measured in the on-board pass after review, not inside tasks.
- **Untested inputs.** App URL and OUI TLVs, non-UTF-8 bytes and very long strings were not seen on the test network.
  They are tested with built inputs only.
- **Test rig.** The aux C6 runs ot_cli again and is a sleepy child of 0x1000. Its NVS keeps that mode, so the on-board
  pass can check the sleepy path with it.
