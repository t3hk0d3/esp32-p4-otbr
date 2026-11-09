# Documentation review for end users: plan

Date: 2026-11-10. Scope: `README.md`, `docs/*.md`, `docs/images/`, `mkdocs.yml`. Out of scope: `docs/superpowers/`,
`docs/phase0/`, the landing page `docs/overrides/home.html` (only its radios diagram is reused).

Styleguide: `STYLEGUIDE.md`, section "Commands and docs" (README is a short overview, one topic per file in `docs/`,
indexed in `docs/README.md` and the `nav` of `mkdocs.yml`; commands go through `make` targets). Existing doc
conventions kept: images in `docs/images/`, screenshots as PNG with a full `alt` text, diagrams as standalone SVG in the
style of `docs/images/hero.svg` (own `<style>`, light and dark scheme, teal = radio, copper = wire), MkDocs admonitions
for warnings. New rule this plan adds to `STYLEGUIDE.md`: user pages lead with the web UI, then `curl`; a page is either
a **guide** (task-first, for someone running the board) or **reference** (API, metrics, internals).

BDD skeleton: skipped, docs-only change. Verification is `make docs` (strict build, broken links fail) plus a read-through.

## Findings

### 1. Developer and test material inside user pages

The biggest problem. A user reading these pages meets test checklists, heap budgets and source file names.

| Page | What does not belong to an end user |
| --- | --- |
| `radios.md` | "Verifying on a board" (about 70 lines of on-board test procedures, heap budgets, task stack sizes, PATCHES.md patch numbers, dated bench results); the CH34x status-packet decoding note ("not confirmed against a datasheet; confirm it on a board"); `channel_state` / `mode_state` internals; "Remaining risks" race windows; the one-task-for-both-channels scheduling note |
| `troubleshooting.md` | latency measurements of the topology scan (pytest file, dated); "the old `/topology`"; "Not verified on hardware: the `br_core` give-up path"; the `-dirty` version-stamp note; `/api/node` upstream quirk with a PATCHES.md pointer; core dump via `idf.py` |
| `updates.md` | `update_source` test-build console command; `MBEDTLS_HAVE_TIME_DATE`; version-comparison rules in detail; `tools/make_bundle.py --help` |
| `maintenance-jumper.md` | retry baud ladder; "Base image version" (content hash inputs, `otadata`, slot boot order) |
| `diagnostics.md` | `esp_openthread_lock_acquire/release`, `libopenthread_br.a`, task pinning per core, the 300 µs range-switch note |
| `first-install.md` | `rcp_sync:` log lines, `jumper.base_installed` checks, offsets from `c6_board.h`, `p4/main/board.h`; Path 1 timing internals |
| `building.md` | partition history (`rcp_0`/`rcp_1`); base-version hashing rules |
| `zigbee.md` | "built and host-tested, on-board tests written but not run"; `p4/pytest/test_zigbee.py` as the list of checks |
| `api.md` | the Web UI section and "Creating a Thread network" / "Adding more border routers" / "Choosing the leader" are user guides buried in the API reference |
| `docs/README.md` | lists `docs/superpowers/` and `docs/phase0/` next to user pages |

### 2. Reference written where a guide is needed

- `radios.md` and `networking.md` describe endpoints, states and error codes first; the web UI comes later or not at
  all. A user wants "plug in a dongle, open Radios, pick a mode".
- `first-install.md` mixes three audiences: release users (browser flasher), checkout users (`make`), and people
  debugging the path. Release users should never see `make`.
- `building.md` sits under "Setting up", although an end user never builds.
- The Topology page, the Thread Console, the Network page (form/join/leader) and the Routing page have no user
  section of their own; they appear only inside `api.md` or `networking.md`.

### 3. Undefined jargon

RCP, spinel, OMR prefix, ULA, TREL, SRP, NVS, otadata, base image, ROM recovery, ESP-Hosted, Border Agent, dataset,
leader weight. Worst: **channel** means both a radio slot (`builtin-c6`, `usb-…`) and an 802.15.4 channel (11–26),
sometimes on the same page (`radios.md`, `sniffer.md`, `zigbee.md`).

### 4. Duplication

- Creating a network: `first-install.md` and `api.md` (same Python snippet twice).
- The Dashboard description: `api.md` and `zigbee.md`.
- Recovery steps: `first-install.md`, `troubleshooting.md`, `maintenance-jumper.md`.

## Screenshots and diagrams

### Missing screenshots

| Needed for | Screenshot |
| --- | --- |
| Radios guide | Radios page: table with the C6 and a dongle |
| Radios guide | Configure dialog: mode cards (one greyed out with its reason), proxy settings |
| Radios guide | "Move Thread here" in the Configure dialog, and the move progress |
| Routing guide | Routing page: method, OMR prefix, a BGP peer with an established session |
| Dashboard (README, `api.md`, guides) | Dashboard in normal Thread operation (`BR_RUNNING`). Today's `dashboard.png` shows Zigbee mode and is used as *the* dashboard everywhere |
| Updates | Firmware page with the "Updates from GitHub" card (today's `firmware.png` predates it) |
| Home Assistant | the integration's URL dialog (step 2 of "Add the integration") |
| Zigbee | ZHA setup in Home Assistant: radio type Ziggurat and the `ws://` path |
| Topology guide | Topology page map, device table, device dialog (addresses, services, ping) |
| Flash in the browser | the ESP Web Tools dialog (port picker, erase option) |

### Stale screenshots

Every page got the Radios entry in the Device menu on 2026-11-06, and several pages changed after their screenshot:
`network.png`, `topology.png`, `capture.png`, `capture-running.png`, `firmware.png`, `firmware-radio-fault.png`,
`firmware-recovery.png`, `ethernet*.png`, `diagnostics.png`, `leader*.png`, `console.png`. Retake them all in one pass
so the menus match. The landing page's `images/home/*.webp` are retaken from the same pass.

No image in `docs/images/` is unused.

### Missing diagrams

| Diagram | Page | Shows |
| --- | --- | --- |
| Board anatomy | `first-install.md` | board photo with labels: P4 USB-C, Ethernet/PoE, USB-A (dongle), C6 antenna IPEX, H7, BOOT and RESET buttons |
| Which install path | `first-install.md` | small decision flow: factory C6 / jumper / USB-UART → path, each ending in "upload `c6_rcp.bin`" |
| Radios | `radios.md` | reuse the landing page's radios diagram as `docs/images/radios.svg` |
| RCP proxy | `radios.md` | a radio on the board served over TCP/UDP to Zigbee2MQTT or a remote `otbr-agent` |
| Split mode | `networking.md` | switch trunk with the management VLAN (browser, Prometheus) and the Thread backbone VLAN (Home Assistant, other border routers, mDNS) |
| Routing to the mesh | `networking.md` / routing guide | core router learns the OMR prefix from the board over BGP or RIPng; a host on another subnet reaches a Thread device |
| TREL | `networking.md` | two boards with separate radio islands joined over Ethernet (cut-down from the landing page hero) |

## Target structure

```
Home, Overview, Flash in the browser
Get started
    First install            (release files and web UI only; make lives in Building)
    Home Assistant
    Your first Thread network (Network page: form, join, more border routers, leader)
Using the board
    Dashboard and web UI tour (every page, one screenshot each, link to its guide)
    Radios and USB dongles   (guide)
    Zigbee coordinator
    Networking: VLANs, names, TREL
    Routing to the mesh      (split out of networking.md)
    Updates
    Packet capture           (sniffer.md, renamed)
    Topology and Thread Console
    Monitoring: diagnostics and Prometheus
    Maintenance jumper
    Troubleshooting
    Glossary
Reference
    HTTP API                 (api.md without the guides; radio channel and routing details moved here)
    Radio internals          (moved from radios.md: maintenance/reinit, unplug semantics, proxy framing and security)
    Metrics list
Working on it
    Building and flashing
    Development              (plus the on-board check lists moved from radios.md and the measurements from troubleshooting.md)
```

## Tasks

Each task is one commit; `make docs` passes after each.

1. **Styleguide rule.** Add the guide/reference rule and the diagram and screenshot conventions above to
   `STYLEGUIDE.md` "Commands and docs".
2. **Move developer material out.** Radios verification list, dated measurements, test-build commands, source and
   PATCHES pointers → `docs/development.md` ("On-board checks", "Known upstream behaviour"). Internals (reinit, unplug
   races, proxy framing/security, base image) → new `docs/reference/radio-internals.md`. User pages keep one sentence
   and a link where it matters.
3. **Split guides from `api.md`.** Web UI tour → `docs/web-ui.md`; network creation, more border routers and leader →
   `docs/thread-network.md` (one copy of the Python snippet). `api.md` keeps endpoints only.
4. **Rewrite as guides:** `first-install.md` (release-first; `make` variants in one collapsed note each; log-line
   detail moves to Troubleshooting), `radios.md`, `networking.md` + new `routing.md`, `updates.md`, `zigbee.md`,
   `diagnostics.md` + `metrics.md` intro, `maintenance-jumper.md`, `troubleshooting.md` (symptom → fix, no measurements).
   Each: what it is for, how to do it in the web UI, then `curl` and API links.
5. **Glossary and naming.** New `docs/glossary.md`; first use of each term links to it. Say "radio" for the slot and
   "Thread/Zigbee channel" for 11–26 on every user page (API field names stay as they are).
6. **Navigation.** `mkdocs.yml` nav and `docs/README.md` to the target structure; README "Documentation" paragraph and
   "Get started" (release-first) updated; `building.md` moves under "Working on it".
7. **Diagrams.** The seven SVGs above, in the `hero.svg` style, checked in light and dark scheme on the built site.
8. **Screenshots.** One capture pass for the missing and stale list (see open question 2), retake
   `images/home/*.webp` from it, write `alt` texts that describe what the reader should notice.
9. **Read-through.** Read every user page top to bottom as a new owner with a release download and no checkout; fix
   what stops them. `make docs`, then preview with `make docs-serve`.

## Open questions

1. **Where developer material goes.** Proposed: on-board checks into `docs/development.md`, internals into a
   Reference section of the site. Alternative: drop the dated bench results and checklists entirely (they live in the
   plans and memory already).
2. **How screenshots are taken.** Proposed: the frontend served by `tools/topology_preview.py`, extended with canned
   `/device/*` answers for every page, so screenshots need no board and can be retaken after every UI change. Pages
   that need real state (Home Assistant dialogs, ZHA, a dongle in the Radios table) on the board in one batched pass.
