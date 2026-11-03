# TREL visibility: peer networks and link transport: design

Date: 2026-11-03. Status: approved 2026-11-03.

Builds on the TREL design (`2026-11-03-trel-design.md`) and the Topology page (`2026-11-02-topology-page-design.md`).

## 1. Intent

TREL is a diagnostic blind spot today, in two ways:

- **Foreign peers look like ours.** OpenThread keeps every `_trel._udp` advertiser on the link in its TREL peer table,
  whatever Thread network it belongs to. The board reports all of them as peers. On the bench a TREL node of another
  network on VLAN 20 made the board report `peers: 2` with one real partner. OpenThread never links with such a peer
  (broadcasts skip other extended PAN IDs, and MLE security keeps it from becoming a neighbour), but the count and the
  list suggest otherwise.
- **The transport of a link is invisible.** The Topology page shows each router link and its link quality, but not
  whether the board reaches that neighbour over the 15.4 radio, over TREL (a tunnel through Ethernet), or both.
  The user needs this for diagnostics.

Success: from the Ethernet page, the API and the metrics, one can tell this network's TREL peers from other networks'
at a glance; on the Topology map, each of this board's router links shows whether it runs over radio, TREL, or both.

## 2. Goals, non-goals, anti-goals

Goals:

1. Expose each TREL peer's extended PAN ID and whether it is this board's network, in the API and the metrics.
2. Show it on the Ethernet page's TREL card, foreign peers marked.
3. Show the transport of this board's own router links on the Topology map and in the device details.
4. Show OpenThread's per-radio preference (TREL and 15.4) in the TREL peer list and on the map, red when TREL's is
   lower than 15.4's (user, 2026-11-03).

Non-goals:

- Transport of links between two other routers. Thread network diagnostics carry no radio information, so it cannot
  be known. Capable border routers usually share the backbone VLAN, so the board's own links are enough (user, 2026-11-03).
- Guessing or drawing "likely TREL" links between other routers.
- Transport of child (end device) links. Children attach over 15.4 in practice.
- Network names of foreign peers: the TREL TXT record carries only the extended address and the extended PAN ID.
- Periodic TREL peer re-discovery (deferred on 2026-11-03).

Anti-goals (a failure even if it technically works):

- A transport label that is a guess. Every TREL marking on the map comes from OpenThread's own neighbour state.
- A foreign peer counted or listed as if it were a partner of this board.
- A map that gets harder to read: the radio link and its LQI badge keep their current look.

## 3. Decisions

### 3.1 TREL peers: API, metrics

| Topic | Decision |
| --- | --- |
| Source | `otTrelPeer.mExtPanId` from `otTrelGetNextPeer`, compared with the board's own extended PAN ID (`otThreadGetExtendedPanId`) |
| `/device/status` `trel.peer_list[]` | each entry gains `"ext_pan_id"` (16 lowercase hex digits) and `"same_network"` (bool) |
| `trel.peers` | counts only peers of this network (meaning changes; it counted every table entry) |
| `trel.peers_other` | new: peers of other networks; `null` together with `peers` when OpenThread's state could not be read |
| `trel.peer_list[].preference` | `{"ieee802154": <0..255 or null>, "trel": <0..255 or null>}` from `otMultiRadioGetNeighborInfo` when the peer is a Thread neighbour of the board; `null` when it is not (every foreign peer, and a same-network peer not linked yet) |
| Counting | one walk of the whole TREL peer table in `br_core` gives both counts and the list; `/device/status` and the metrics both use it, so they never disagree |
| Board without an extended PAN ID (no active dataset) | every peer counts as `same_network: false` |
| `peer_list` order and size | this network's peers first, then the others; still at most 8 entries (`BR_CORE_TREL_PEERS_MAX`), so a full list never hides this network's peers behind foreign ones |
| Metrics | `br_trel_peers` counts this network only; new gauge `br_trel_peers_other` |
| Docs | `docs/api.md`, `docs/metrics.md`, `docs/networking.md` (TREL section) |

### 3.2 TREL card (Ethernet page), option A of the mockups

- The peer table gains a column "Network (ext. PAN ID)": the extended PAN ID followed by a chip, green "this" for
  `same_network`, red "other" otherwise.
- The "Peers" figure shows `peers`; when `peers_other` > 0, "+ N other network" follows in muted text.
- A "Preference" column shows the per-radio preference, e.g. "TREL 255 · 15.4 200"; "-" when `preference` is `null`.
  The TREL figure is red when it is lower than the 15.4 figure.
- Existing stale/error handling of the card is unchanged.

### 3.3 Transport of the board's router links (scan data)

- During a topology scan, for each router neighbour of this board, the collector reads `otMultiRadioGetNeighborInfo`
  (OpenThread multi-radio, compiled in because 15.4 and TREL are both built). Local call, no over-the-air request.
- The board's own router entry in `GET /device/topology` carries, per entry in `links[]`, a `"transport"` object:
  `{"ieee802154": <preference 0..255 or null>, "trel": <preference 0..255 or null>}`. `null` = the neighbour does not
  use that radio. Links of other routers have no `"transport"` key (unknown, not "radio").
- The radio OpenThread sends on is derived exactly as `RadioSelector::Select` (OpenThread `radio_selector.cpp`) picks
  it: radios are tried TREL first, then 15.4; the first one whose preference is at least 220 (`kHighPreference`) is
  chosen; when none reaches 220, the higher preference wins, a tie going to TREL. A radio with `null` is not a
  candidate.
- OpenThread never clears a radio once a neighbour has used it; a broken TREL path only lowers TREL's preference
  (-35 per tx error, -100 per lost deferred ack). So `"trel"` stays non-null for a dead tunnel, and the preference is
  what tells it is failing.
- A build without TREL omits `"transport"` everywhere.

### 3.4 Topology map and details

- A link from this board whose transport includes TREL is drawn twice:
  - the straight line as today when it also runs over 15.4 (colour and dashes by link quality, LQI badge);
  - a **dashed purple curve** (Bézier) from this board through the "Home network · Ethernet" box to the neighbour,
    with a badge "TREL <preference>", e.g. "TREL 255". A TREL-only link (`ieee802154` null) has only the curve: no
    straight line is drawn for it, even when the other router reports a link quality for it.
- The TREL badge is red when TREL's preference is lower than 15.4's (user, 2026-11-03).
- The colour is distinct from the three link-quality colours, so the curve cannot be read as a quality 1 radio link
  (red dashes).
- Legend: one new entry, "dashed purple = TREL (tunnel over Ethernet)", shown only when a TREL link is on the map.
- Device details panel and the "Connected via" column of the device list name the transport of this board's link to
  that router in words, with both preferences and the radio OpenThread sends on: "15.4 255", "TREL 255", or
  "15.4 255 · TREL 55, sends over 15.4". The TREL figure is red under the same rule as the badge.
- Link quality of a link that also runs over TREL: OpenThread keeps one link quality per neighbour, fed by both radios,
  and counts every TREL frame as -20 dBm, so the value leans toward 3. The straight line keeps its LQI badge as
  reported; the details panel says "link quality includes TREL frames" for such a link. (Builder's call, see 6.)

## 4. Constraints

- Styleguide: `STYLEGUIDE.md` (naming, formatting, errors, tests). Frontend follows the existing page code
  (`topology.js`, `ethernet.js`) and its host-test harness in `tools/tests/`.
- `/device/status` still fits its 2048-byte buffer with 8 peers carrying the three new fields (host test).
- Web image budget: 384 KB (`tools/tests/test_device_pages.py`).
- Hardware: one batched on-board pass after review (see the hardware budget rule): the TREL peer rig
  (`tools/trel_peer`) for 3.1, 3.2 and the map; a foreign-network peer can be made with a second `ot-ctl` network on the
  peer (`dataset init new`).
- `test_trel.py` asserts that the board's peer is listed with `same_network: true`, not just `peers >= 1`.

## 5. Left to the builder

- Where the multi-radio read happens in `mesh_scan` (`ot_ops.c` collector) and how neighbours are matched to router
  IDs (extended address or RLOC16).
- Exact curve geometry and badge placement, as long as the curve passes through the network box and does not cover
  the LQI badge of the straight line.
- Field and type names inside C structs.

## 6. Calls made while writing (check these)

- "Sends over" mirrors OpenThread's radio selector instead of "higher preference wins": in the normal state both radios
  sit at 255 and OpenThread sends over TREL, which the simpler rule would have labelled "no preference".
- Peers that are not Thread neighbours (all foreign ones) show no preference ("-").
- The LQI badge of a link that also runs over TREL stays, with the "includes TREL frames" note in the details panel
  only. OpenThread offers no per-radio link quality.
- One TREL peer walk shared by `/device/status` and the metrics.
- A board without an extended PAN ID counts every peer as another network.
- No fading of a failing TREL curve: the user's red rule (TREL below 15.4) marks it.
