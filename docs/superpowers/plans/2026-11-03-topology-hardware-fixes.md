# Topology hardware-pass fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Fix the three gaps the hardware pass found:
- full-device children with no addresses;
- the Home-network box overlapping nodes;
- other border routers not marked.

**Architecture:** One more scan step in `mesh_scan` (scan.c state machine plus ot_ops.c glue) asks rx-on children with
no addresses for their IPv6 Address List with a unicast Network Diagnostic Get. The page reads the new
`addresses_reported` and `progress.children` fields, marks other border routers, and places the Home box clear of
every node.

**Tech Stack:** as the topology plan (ESP-IDF 6.1, OpenThread `netdiag.h`, Unity host tests, ES5 page script, pytest
with node).

**Spec:** `docs/superpowers/specs/2026-11-02-topology-page-design.md`, section 3.4 (amendment 2026-11-03), plus 3.3.

**Styleguide:** `STYLEGUIDE.md`, the same rules as `docs/superpowers/plans/2026-11-02-topology-page.md`.

## Global Constraints

- **Git:** branch `feat/topology-page`. Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- **No board:** no flashing and no on-board runs inside tasks. The board re-check comes after the review: one OTA app
  flash, no C6 writes.
- **Lock order:** OpenThread lock, then scan mutex, as before. The ops never take the OpenThread lock.
- **The new scan step:**
  - only rx-on children with no addresses are asked;
  - at most 4 queries are in flight;
  - only TLV 8 is requested;
  - the destination is mesh-local prefix + `0:ff:fe00:<rloc16>`.
- **JSON:**
  - `"addresses_reported": false` is written only for a child that was asked and gave no answer;
  - `progress.children` = `{"total", "queried"}` is written only while running.
- **Copy:** a device with no name reads "Unknown 0xNNNN" (was "Unnamed 0xNNNN"; user request 2026-11-03); "Not reported", "This device didn't report its addresses.", "Asking devices for their addresses: M of N",
  "Other border router", "Border router".

## Review Focus

1. **A late answer after cancel or cap.** A diagnostic answer that arrives after a cancel, the cap, or a newer scan
   must be dropped, because `otMeshDiagCancel` does not cover netdiag. The test goes in Task A: a stale-generation
   answer is ignored.
2. **A child that leaves between steps.** A query to an unknown RLOC times out, and the scan must still finish. Task
   A's tests cover it.
3. **A Home box crowded on every side.** It must still be placed without overlap, even if the map grows. The test goes
   in Task B.

## BDD skeleton

```
mesh_scan (scan.c), child address step
    after the last router's queries
        On rx-on children with no addresses
            - starts ops.query_child_diag for up to 4 of them, progress.children.total = their count
        On no such child
            - goes straight to the snapshot
        On sleepy children with no addresses
            - does not ask them
    mesh_scan_on_child_diag(gen, rloc16, addrs, n, ot_error)
        On an answer with addresses
            - stores them on that child (up to MESH_SCAN_MAX_CHILD_ADDRS), queried++, starts the next waiting child
        On an error or timeout
            - marks that child addresses_reported false, queried++, starts the next
        On the last answer
            - takes the snapshot, state done
        On a stale generation, or a state other than running in this step
            - is ignored
    ops.query_child_diag start failure
        On NO_BUFS / BUSY
            - marks that child not reported and moves on
        On INVALID_STATE
            - fails the scan stopped
    cap during the step
        - children still waiting are marked not reported; snapshot; done (partial per the existing rule)
mesh_scan_render
    On a child with addresses_reported false
        - writes "addresses_reported":false after "addresses"
    On running in the child step
        - writes progress {"routers","queried","children":{"total","queried"}}
TopologyLogic
    devices(result)
        On a child with addresses_reported false
            - device.addrsReported = false (true otherwise)
        On a router with border_router and not this_device
            - kind stays router/leader, device.otherBr = true
    layout(devices, links)
        On the Home box near other nodes
            - the box overlaps no node or label footprint; it is the nearest free spot around the board
TopologyPage
    table
        On a child with addrsReported false
            - routable cell shows "Not reported" (grey), not "None yet"
        On another border router
            - type "Border router"
    dialog
        On a child with addrsReported false
            - shows "This device didn't report its addresses."
        On another border router
            - shows a "Border router" chip
    map and legend
        On another border router
            - the node has an orange outer ring; the legend has "Other border router"
    loading view
        On progress.children.total > 0
            - shows "Asking devices for their addresses: M of N"
```

### Task A: firmware — the child address step

**Files:** `p4/components/mesh_scan/{scan.h,scan.c,scan_types.h,render.c,ot_ops.c}`,
`p4/components/mesh_scan/test_host/main/{test_scan.c,test_render.c,fake_ops.*}`.

**Interfaces:**
- New op: `int (*query_child_diag)(uint16_t rloc16, uint32_t gen)`, which sends the netdiag Get. The OpenThread
  callback context carries `gen` and the RLOC16.
- New entry point:
  `void mesh_scan_on_child_diag(uint32_t gen, uint16_t rloc16, const uint8_t (*addrs)[16], uint8_t n, int ot_error)`.
- `mesh_scan_child_t` gains `bool addresses_reported` (true by default) and an internal "asked" mark.
- `mesh_scan_view_t` gains `children_total` and `children_queried`.

- [ ] Write the failing tests for every BDD effect of "child address step" and the two render effects, plus Review
  Focus 1 and 2.
- [ ] `logrun make test-host`. Expected: the new tests fail.
- [ ] Implement it. The netdiag callback in ot_ops.c parses TLV 8 with `otThreadGetNextDiagnosticTlv`, using the
  existing iterator pattern.
- [ ] Run `logrun make test-host` and `logrun make build`; both must pass with no warnings. Run `make lint`, then
  commit `feat(mesh_scan): ask full-device children for their addresses`.

### Task B: page

**Files:** `p4/components/esp_ot_br_server/frontend/{topology.html,static/topology.js,static/style.css}`,
`tools/tests/test_topology_page.py`, `tools/topology_preview.py` (its fixture shows one not-reported child and one
other border router).

- [ ] Rename the no-name fallback "Unnamed 0xNNNN" to "Unknown 0xNNNN" everywhere (logic, page, tests, preview).
- [ ] Write the failing tests for every `TopologyLogic` and `TopologyPage` effect above, plus Review Focus 3. Use a
  fixture with the real-mesh geometry: 4 routers, two of them border routers, the board in the middle, and a router
  hanging off another on a weak link.
- [ ] `logrun make test-tools`. Expected: the new tests fail.
- [ ] Implement it.
- [ ] Run `logrun make test-tools` and `make lint`, then commit `feat(web): topology marks border routers, places the
  Home box clear, shows unreported addresses`.
- [ ] The controller checks it by eye in the browser pane against the preview server.

### Task C: board re-check (controller, after review)

- [ ] Build the test image. Read `/device/status` `app.version` in its own step, then OTA.
- [ ] On the populated network, with the peer joined: a scan shows the peer's OMR/ULA address, other border routers are
  marked, and the Home box is clear.
- [ ] Re-run `p4/pytest/test_topology.py` on the rig network only if the user asks.
- [ ] Take `docs/images/topology.png`.
