# Topology page redesign: design

Approved sketches: https://claude.ai/artifact/4jeZzZxszV9jdU41BVMicU (canvas "Topology page redesign": the page in its
start, loading and loaded states, the device dialog, and the page at phone width).

## 1. Goal

The Topology page should answer two questions quickly: which devices are on the Thread network, and how do I reach each
of them from my home network (their global and ULA addresses)? It should also tell more about each device than an RLOC16.

Today's page (`topology.html`) falls short:
- Nothing shows until **Load Topology** is clicked, and the old result comes back from `sessionStorage` without a time.
- It draws a router link for every Route TLV entry, so a partial mesh can look like a full mesh.
- Link quality is fetched but never drawn.
- The layout is random, so nodes move on every load.
- This board is not marked.
- End devices carry no data: the collection drops replies from children, so there are no ext addresses, modes or
  addresses.
- Details open in a panel below the map, with raw fields.
- There is no list, so finding a device in a big mesh means hunting by RLOC16.

Success: a user can find any device by name or address, copy its routable address, and see how it hangs off the mesh.
Reloading the browser costs no mesh traffic.

Non-goals:
- A live, auto-refreshing map. Every load sends diagnostics into the mesh, so loading stays a deliberate click.
- Pan and zoom on the map.
- Vendor name and model (Thread 1.4 diagnostic TLVs 25–28).
- Editing anything on the devices.

Anti-goals (it fails even if it works):
- The page refetches the topology by itself on open or reload.
- Links drawn between routers that are not neighbors.
- Map positions that jump between two loads of the same mesh.

## 2. Scope

- A new firmware component that scans the mesh with OpenThread's mesh diagnostics, plus a new endpoint
  `/device/topology` in `device_api`.
- A rewritten `topology.html` with a new script `static/topology.js`. These are vendored files, so the change is listed in
  `p4/components/PATCHES.md`.
- The upstream `/topology` endpoint stays as it is for compatibility, but the page no longer uses it.
- Styleguide: `STYLEGUIDE.md` (component prefix, `esp_err_t`, host tests in `test_host/`). The frontend follows the
  device pages' conventions: plain ES5-style script, no markup built from unescaped strings, shared helpers from
  `static/device_common.js`.

## 3. Data collection (firmware)

### 3.1 Component `mesh_scan`

New component `p4/components/mesh_scan/`, prefix `mesh_scan_`. Public API:
- `esp_err_t mesh_scan_start(void)`
- `mesh_scan_state_t mesh_scan_get(...)`: copies out the state and the progress.
- A render function that writes the JSON of 3.3 into a caller's buffer or chunk sink. The builder picks the exact
  signature.

The collection runs in the OpenThread task through callbacks. Its results are kept in PSRAM until the next scan
starts.

A scan, in order:
1. **Discover.** `otMeshDiagDiscoverTopology` with `mDiscoverIp6Addresses` and `mDiscoverChildTable`. Each router
   reported gives:
   - ext address, RLOC16, router ID and Thread version;
   - the flags this-device, leader and border router;
   - `mLinkQualities[]`, its incoming LQ from every other router (0 = no link);
   - its IPv6 addresses;
   - its children (RLOC16, mode, incoming LQ at the child).
2. **Per router, one at a time** (OpenThread allows one mesh-diag query at once), in router ID order:
   - `otMeshDiagQueryChildTable` gives each child's ext address, timeout, age, connection time, average and last RSSI,
     link margin, frame and message error rates, version, supervision interval, and CSL period.
   - `otMeshDiagQueryChildrenIp6Addrs` gives each child's IPv6 addresses. The router answers for its children, so
     sleepy devices are not woken.

   A router that does not answer a query keeps its discover data and is marked `children_detail: false`.

   If a query fails to start, the reason decides what happens (my call):
   - `OT_ERROR_NO_BUFS` or `OT_ERROR_BUSY`: that router is marked `children_detail: false` and the scan moves on.
   - `OT_ERROR_INVALID_STATE` (the board detached): the scan fails with `stopped`.
2a. **Routers that didn't answer discover** (my call). Discover only reports routers that reply. A router in the
   board's own router table that never replied still gets an entry, built from the local table: `router_id`, `rloc16`,
   `responded: false`, and no other fields. Links from routers that did answer still point to it. The map draws it as
   a grey disc labelled "didn't answer", and the table shows it with the type "Router (no answer)".
3. **Snapshot.**
   - Network: name, channel, partition ID, leader router ID, mesh-local prefix, and this board's RLOC16.
   - The on-mesh prefixes in the network data that have SLAAC set (the OMR prefixes).
   - The SRP server's hosts: `otSrpServerGetNextHost` → full name with the `.default.service.arpa.` domain removed, its
     addresses, and its services (instance name and service type). Deleted hosts are skipped.

Limits:
- A whole scan is capped at 60 s. When the cap is hit, the scan finishes with whatever it has.
- `partial: true` (my call) means any of three things:
  - the cap was hit;
  - a router has `responded: false`;
  - a router has `children_detail: false`.
  The page then says "some routers didn't answer".
- Memory is bounded by Thread's own limits: 32 routers, and the child table size of each router. The result is
  allocated once per scan from PSRAM.

Concurrency:
- One scan at a time.
- `mesh_scan_start` while a scan runs returns `ESP_ERR_INVALID_STATE`; the endpoint maps this to joining the running
  scan.
- If OpenThread answers `OT_ERROR_BUSY` (the Thread REST `/api/devices` collection holds mesh diagnostics), the scan
  fails with `busy`. The REST side already retries its own start on later passes, so no vendored change is needed there.
- A mode change away from `BR_RUNNING` cancels a running scan (`otMeshDiagCancel`) and leaves it `failed`, error
  `stopped`.
  - While a scan runs, a 1 s watchdog checks the mode. This catches faults that happen outside `device_api`.
  - When the watchdog fails the scan this way, it does not call OpenThread. OpenThread may already be gone.
- Other long operations (my call):
  - While the `device_api` long-op slot is held (OTA, radio firmware push, ROM recovery), `POST` answers 409 `busy`
    with that operation's `detail`.
  - Starting a radio push or a ROM recovery cancels a running scan the same way as a mode change, so OpenThread is
    never called during a push.
  - The scan does not take the long-op slot itself, so it never blocks an update.

### 3.2 Endpoint

- `POST /device/topology` starts a scan. It answers 202 with the state object (3.3). If a scan already runs, it answers
  202 with that scan's state.
  - 409 `busy` when the mode is not `BR_RUNNING`, with the mode in `detail`.
  - 409 `busy` when mesh diagnostics are held elsewhere.
- `GET /device/topology` returns the state object. It sends no mesh traffic.
- Both use the device API's gate, error format (`{"error","detail"}`) and chunked JSON output. Add both rows to
  `docs/api.md`.

### 3.3 State object

This shape is the contract between the firmware and the page:

```
{
  "state": "idle" | "running" | "done" | "failed",
  "error": "busy" | "stopped" | "no_memory",           // failed only
  "progress": {"routers": 5, "queried": 3},              // running only; routers = 0 until discover ends
  "age_s": 42,                                           // done/failed: seconds since the scan ended
  "result": {                                            // done only
    "partial": false,
    "network": {"name": "MyThread", "channel": 15, "partition_id": 1243556871, "leader_router_id": 9,
                "mesh_local_prefix": "fdde:ad00:beef:0::/64", "this_rloc16": 34816,
                "omr_prefixes": ["2001:db8:4a2:1::/64", "fd7a:91c3:22e0:1::/64"]},
    "routers": [{
      "rloc16": 9216, "router_id": 9, "ext_address": "d61a7c03f4e2b918", "version": 5,
      "leader": true, "this_device": false, "border_router": false,
      "links": [{"router_id": 34, "lq_in": 3}],          // only entries with lq_in > 0
      "addresses": ["fdde:ad00:beef:0:0:ff:fe00:2400", "..."],
      "responded": true,                                 // false: stub from the local router table (3.1, 2a)
      "children_detail": true,
      "children": [{
        "rloc16": 9217, "ext_address": "1c88f02ab4d6e5a3",
        "mode": {"rx_on_when_idle": false, "full_thread_device": false, "full_network_data": false},
        "lq_in": 3, "timeout_s": 240, "age_s": 12, "connected_s": 86400,
        "rssi_avg": -61, "rssi_last": -63, "link_margin_db": 39,
        "frame_error_pct": 0.4, "message_error_pct": 0.0, "version": 4, "csl": false,
        "addresses": ["2001:db8:4a2:1:2be4:6c19:a0f7:3381", "..."]
        // "addresses_reported": false only when the child was asked directly and did not answer (3.4)
      }]
    }],
    "srp_hosts": [{"name": "kitchen-plug", "addresses": ["..."],
                   "services": [{"instance": "kitchen-plug", "type": "_matter._tcp"}]}]
  }
}
```

- SRP service details (`srp_hosts` extended, `srp_servers`) are specified in `2026-11-04-srp-details-design.md`.
- Each router reports one direction of each link. The page pairs them up: the LQ out of A towards B is B's `lq_in` for
  A.
- The fields from `otMeshDiagQueryChildTable` (`timeout_s` through `csl`, and `ext_address`) are left out when
  `children_detail` is false. They are also left out for a child the router's child table did not return.
- Unknown values are left out, not given a fake number (amendment, 2026-11-03):
  - `version` when the router or child reports no Thread version;
  - `frame_error_pct` and `message_error_pct` when the child does not track error rates.

  The page shows "–" for a missing value.
- IPv6 addresses and prefixes are in RFC 5952 short form (`fdde:ad00:beef::/64`). The addresses in the sample above
  are only illustrative.

### 3.4 Amendment 2026-11-03: findings from the hardware pass

On the rig and on a populated network, a child that is a full Thread device (a REED, `full_thread_device: true`) came
back with an empty address list. Only minimal (MTD) children register their addresses with their parent, so
`otMeshDiagQueryChildrenIp6Addrs` has nothing for a REED child.

**Asking children directly.** After the per-router queries, and before the snapshot, the scan runs one more step:
- **Which children:** every child that is rx-on-when-idle and has no addresses. Sleepy children are skipped. They are
  MTDs, so their parent already covers them, and asking them would mean waiting for them to wake.
- **The query:** a unicast Network Diagnostic Get (`otThreadSendDiagnosticGet`) to the child's RLOC address
  (mesh-local prefix + `0:ff:fe00:<rloc16>`), asking only for the IPv6 Address List TLV (8).
- **Concurrency:** at most 4 of these queries are in flight at a time. Answers fill that child's `addresses`, up to
  the existing cap.
- **The result:** a child that was asked and gave no answer (an error, a timeout, or a cap hit) keeps an empty list
  and gets `"addresses_reported": false`. Every other child leaves the key out.
  - `partial` keeps its meaning (the cap, or a router flag). A child's flag doesn't set it.
- **Progress:** progress gains `"children": {"total": N, "queried": M}` during this step. Before the step, `total` is
  0.
- **Cancel, cap and watchdog** cover this step the same way. A cap hit during the step marks the children that are
  still waiting as not reported.

**The page:**
- **A child with `addresses_reported: false`:**
  - the table's Routable addresses cell reads "Not reported" in grey, not the amber "None yet";
  - the dialog says "This device didn't report its addresses." instead of the "No routable address yet" note.
- **The loading view** shows "Asking devices for their addresses: M of N" while `progress.children.total > 0`.
- **Other border routers** (`border_router: true` and not `this_device`):
  - on the map: a router disc with an orange outer ring;
  - in the legend: "Other border router";
  - Type in the table: "Border router";
  - in the dialog: a "Border router" chip.
- **The "Home network · Ethernet" box** must not overlap any node or node label. It takes the nearest free spot
  around the board, trying wider rings when the close ones are taken. The map grows if it must.

### 3.5 Amendment 2026-11-03: findings from the code review

**One network-diagnostics callback for everyone.** OpenThread's netdiag client keeps a single callback, and every
`otThreadSendDiagnosticGet` replaces it. As a result, the scan's child step, the vendored `/topology` and the REST
diagnostics take each other's answers. A new component, `netdiag_mux`, owns that slot:
- **Sending:** every sender calls `netdiag_mux_send_get(dest, tlvs, count, cb, ctx, filter_peer)` instead of the
  OpenThread call. It runs on the OpenThread task, or with the OpenThread lock held.
- **Listeners:** the mux keeps one listener per callback function, with the latest context it was sent with. When
  `filter_peer` is set and `dest` is unicast, the listener also keeps `dest`. The table holds at most 8 listeners.
- **Answers** (no error, with a message) go to every listener that either has no peer filter or whose `dest` equals
  the answer's peer address. A CoAP response comes from the address the request was sent to.
- **Errors** (timeouts, no message) are dropped. Every current callback ignores them already.
- **Who filters:**
  - the vendored sends use `filter_peer = true`; multicast sends are unfiltered either way;
  - the scan's child step uses `false`, because it matches peers itself.
- **Bookkeeping:** this patches vendored code, so it gets a `PATCHES.md` entry.

**Ending a scan never calls into OpenThread from a callback or without the lock.**
- **A callback that finds the mode lost:** it fails the scan with `stopped` and does not call `otMeshDiagCancel`.
  OpenThread's single mesh-diag slot keeps a new discover out (`OT_ERROR_BUSY`) until the abandoned one ends, as the
  watchdog path already relies on.
- **`mesh_scan_cancel`:**
  - It cancels in OpenThread when it gets the lock within 100 ms.
  - Otherwise it fails the scan with `stopped` anyway (scan mutex only), so a push, recovery or capture never runs
    alongside a live scan.
  - It returns `esp_err_t`: `ESP_OK`, or `ESP_ERR_TIMEOUT` when OpenThread could not be told.

**Start results.**
- `mesh_scan_start` returns `ESP_ERR_INVALID_STATE` only when a scan is already running. The endpoint maps that to
  202, joining the scan.
- When the mode is not `BR_RUNNING`, it returns `ESP_ERR_NOT_ALLOWED`, and the endpoint answers 409 `busy`,
  `mode <MODE>`, with the mode read after the refusal.
- `POST /device/topology` also answers 409 `busy` while a capture stream holds the pcap slot, with the slot's own
  detail.

**The page.** A `done` answer to the POST counts as a fresh load only when `age_s` is at most 5 s. An older one is
shown as a saved result (amber pill), at its real time.

## 4. Page

### 4.1 Start and loading

- On open, the page reads the saved result from `localStorage` and sends one `GET /device/topology`. Neither causes
  mesh traffic.
- If the board says `running`, the page goes straight to the loading view and joins that scan.
- If the board has a `done` result newer than the saved one (judged by `age_s` against the save time), that board
  result becomes the "last result".
- **Start view:**
  - A short explanation: loading asks every router about its neighbors, children and addresses, takes a few seconds,
    and adds a little delay to Thread traffic.
  - With a last result: the line "Last result: <time> (<n> min ago), N routers and M end devices", then **Show last
    result** (outlined) and **Load topology** (filled).
  - Without one: only **Load topology**, with "No saved result in this browser yet."
- **Load topology** sends `POST /device/topology`, then polls `GET` every second while `running`.
- **Loading view:**
  - "Finding routers…" until discover ends.
  - Then "Asking each router about its children: 3 of 5", with a bar.
  - Then the elapsed time.
- A 409 shows its reason in the start view: "The Thread network isn't running (mode X)" or "Another mesh scan is
  running, try again in a few seconds". **Show last result** still works.
- A `failed` scan shows its error and leaves the last result untouched.
- When a scan ends `done`, the page saves the result and shows it as live.

### 4.2 Header and summary

- **Status pill:**
  - Amber for a saved result: "Saved result from 11:42, 38 min ago".
  - Green for a fresh load: "Loaded at 12:20".
  - With `partial`, the pill adds "some routers didn't answer".
- **Button:** **Load fresh** (saved result) or **Load again** (fresh load).
- **Summary tiles:**
  - Network: name, channel, partition.
  - Devices: total, "N routers · M end devices, K of them sleepy".
  - Leader: name, RLOC16.
  - Routable prefixes: each OMR prefix, labelled "global" (2000::/3) or "ULA" (fc00::/7).

### 4.3 Map

- **Nodes.** All are drawn in SVG, and each is a focusable element with an accessible name; Enter or Space opens the
  dialog.
  - This board: an orange rounded square marked "BR", with a dashed line to a "Home network · Ethernet" box.
  - Routers: blue discs.
  - The leader: a double ring.
  - End devices: small rings; sleepy ones (not rx-on-when-idle) are dashed.
- **Router links.** A link is drawn only between routers that are neighbors (`lq_in > 0` on at least one side).
  - Color comes from the lower of the two LQIs: 3 green, 2 amber, 1 red dashed.
  - A badge in the middle of the link shows the number, or `in/out` (e.g. `3/2`) when the two sides differ, in the
    first router's view.
- **Parent links.** Thin grey.
- **Labels.** Under each node: the display name (4.6), or "Unknown 0xNNNN".
- **Legend.** Node kinds, the LQI colors 3/2/1, the `3/2` notation, and the parent link.
- **Layout is deterministic.** The same set of routers and children gives the same picture:
  - This board sits in the centre. If the board is a child (`this_device` is not set on any router; the board is the
    child whose RLOC16 equals `this_rloc16`), the leader is centred instead (my call). The board is then drawn as a
    small orange square among its parent's children, with the "Home network" box attached to it. In the table it sits
    under its parent.
  - The other routers start on a ring in router ID order, then get a fixed number of force-relaxation steps (repulsion
    between routers, attraction along links) with no randomness.
  - Children fan out around their parent, on the side facing away from the centre, ordered by RLOC16.
  - The map's size grows with the node count. It scrolls horizontally inside its card on narrow screens.
  - The builder chooses the constants.
- **Focus.** While the dialog is open, everything except the selected device, its links, neighbors, parent and
  children is dimmed. After the dialog closes, the device keeps an orange selection ring until another device is
  picked.

### 4.4 Devices table

- **Rows** are grouped by parent: this board, then its children, then each router in router ID order followed by its
  children. Child names are indented with "↳".
- **Columns:**
  - Device: display name; a "This board" or "Leader" badge; RLOC16 and ext address underneath.
  - Type: Border router, Router, End device, Sleepy end device.
  - Connected via: the parent's name for a child; "N router neighbors" for a router.
  - LQI: the child's `lq_in` as a colored number; "–" for routers.
  - Routable addresses: each address under an OMR prefix, tagged Global or ULA, each with **Copy**. "None yet" (amber)
    when there is none.
- **Filters:** chips for All, Routers, End devices and Sleepy, with counts, and a search box that matches the display
  name, RLOC16, ext address, SRP host name and every address (substring, case-insensitive). With no match: "No device
  matches '<q>'".
- **Clicking the name** (a button) opens the dialog. The selected row is tinted.
- The table scrolls horizontally inside its card below about 980 px.

### 4.5 Device dialog

A native `<dialog>` opened with `showModal()`. Esc, ×, **Close** and a click on the backdrop close it. It opens the same
way from the map and from the table.

- **Header:**
  - The display name, with **Rename** (4.6).
  - Chips: the type, plus "This board" or "Leader".
- **Left column:**
  - RLOC16 and ext address.
  - SRP host (`<name>.default.service.arpa`, or "Not registered"), with the services it registered.
  - Radio: "Full Thread device, always on", "Always on, child of a router", or "Off between polls, child timeout N s".
  - Thread version.
  - For children: the parent (a link), LQI as a colored number, average and last RSSI, link margin, frame and message
    error rates, and how long it has been attached. These fields are missing when the parent didn't answer the child
    query; the dialog then says "Details unavailable: its parent didn't answer".
- **Right column (routers only):**
  - Router neighbors (links) with "LQI 3" or "LQI 3 in / 2 out".
  - Children (links) with Sleepy or Always on.
- **Addresses** (full width):
  - The routable addresses first, tagged Global or ULA.
  - Then a toggle, "Show internal addresses (mesh-local, RLOC, link-local)".
  - Each row has **Copy** and **Ping**. Ping uses the existing `/ping` with 4 packets of 32 bytes and shows the result
    under the list (replies, loss, min/avg/max). For a sleepy device it adds "Sleepy devices answer when they next
    wake up."
  - With no routable address, an amber note: "No routable address yet. Hosts on your home network can't reach this
    device."
- **Moving between devices:** neighbor, child and parent links switch the dialog to that device, and the map behind
  follows.
- **Footer:** **Show on map** closes the dialog and scrolls to the map. **Close**.

Address classes, decided on the page:
- Under an OMR prefix: Global or ULA, depending on the prefix.
- Mesh-local prefix with IID `0:ff:fe00:xxxx`: RLOC or ALOC (ALOC is not listed).
- Other mesh-local: mesh-local EID.
- `fe80::/10`: link-local.
- Multicast is dropped.
- Anything else: "Other", shown with the internal addresses.

### 4.6 Names

- **Display name, in order:**
  1. The user's nickname for that ext address.
  2. The SRP host name of the first host that shares an address with the device.
  3. "Unknown 0xNNNN".
- This board is named "This border router" on the map and by its SRP or host name elsewhere.
- **Rename** edits the nickname inline. **Save** stores it and an empty name removes it. Nicknames live in this
  browser only, which the dialog says.

### 4.7 Browser storage

- `localStorage["topology:last"]` = `{"saved_at": <epoch ms>, "result": <result object>}`. Only the latest is kept.
- `localStorage["topology:names"]` = `{"<ext address>": "<nickname>"}`.
- Every read and write is wrapped in try/catch. With storage unavailable, the page works without the saved result and
  names, and says that names can't be saved.

### 4.8 Responsive

- At phone width, the summary tiles stack.
- The map card scrolls horizontally.
- The table scrolls in its box.
- The dialog takes the full width minus a 16 px gutter and scrolls inside itself.

## 5. Tests

- **Host (Unity, `mesh_scan/test_host/`):**
  - The JSON render: a fake scan result → exact JSON, including `partial`, `children_detail: false` and an empty mesh.
  - The state machine: start, start while running, OpenThread busy, cancel on mode change, the 60 s cap, `age_s`.
  - OpenThread calls behind an ops struct, as the styleguide allows for host tests.
- **Endpoint (device_api host tests):** 202 on start and join; 409 for a mode other than `BR_RUNNING` and for busy;
  `GET` shapes.
- **Page (`tools/tests/test_topology_page.py`, node harness like the device pages):**
  - Address classification.
  - Pairing link directions.
  - Display-name order.
  - Grouping and filtering of rows, and search.
  - Deterministic layout (same input → same coordinates; no `Math.random`).
  - Start view with and without a saved result, and joining a running scan.
  - The 409 messages, and storage failure.
  - The static checks the device pages already have: ids exist, no unescaped markup insertion, live regions.
- **Target (`p4/pytest/`, next batched hardware pass only):**
  - The scan finds the rig's routers and children with their addresses.
  - The rig's mesh is drawn with no link between two routers that are not neighbors.
  - `test_latency.py` gains a `device_topology_requests` load next to `topology_requests`.

## 6. Docs

- `docs/api.md`: the `/device/topology` rows; the Web UI paragraph mentions the device table and dialog.
- `docs/troubleshooting.md`: the existing note on topology load latency is updated for the new scan.
- `p4/components/PATCHES.md`: the `topology.html` rewrite and the new `static/topology.js`.
- `docs/images/topology.png`: retaken in the hardware pass.

## 7. Left to the builder

- The layout constants and map sizing.
- The poll timing beyond "every second while running".
- The exact `mesh_scan` function signatures and how the result is laid out in PSRAM.
- The colors, as long as they keep the sketch's palette and pass 4.5:1 contrast for text.
