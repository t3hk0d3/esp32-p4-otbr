# One Dashboard for Thread and Zigbee — design

Branch: `feat/zigbee-mode`. Mockup (approved by the user 2026-11-07): https://claude.ai/artifact/XtTxw4gB7qY86w9M3gjdZR —
two artboards, "Thread on the C6, Zigbee off" and "Zigbee on the C6, Thread off".

## Why

The web UI has a Dashboard (`frontend/index.html`, `static/status.js`) that is Thread-centric and carries a small
Zigbee card, and a separate Zigbee page (`frontend/zigbee.html`, `static/zigbee.js`) reached through a nav link that
appears only while a radio runs Zigbee. The user finds the two confusing: board status, Thread status and Zigbee
status are split across pages in a way that does not match how the board is used now that each radio channel picks
its mode (radio channel mux). They want one page that answers "what is this board doing" for both stacks.

## Goals

- One page, the Dashboard, shows the board, Thread and Zigbee status. (User.)
- It has three sections, in this order: **Device and network**, **Thread**, **Zigbee**. (User.)
- A section whose stack is not running shows a one-line placeholder in its place ("Thread is not active: …",
  "Zigbee is not active: …"), never an empty or missing section. (User.) The placeholder says why in one clause and
  links to the Radios page (Device menu) where a radio's mode is set. (Mockup, approved.)
- Everything the Zigbee page shows today moves into the Zigbee section: the state line (running / waiting for Home
  Assistant / radio resetting / no radio), radio, running since, radio resets, the "Connect Home Assistant" box with
  the `ws://<board>/zigbee` path and its Copy button and the no-login warning, client, network, radio traffic
  counters, stack memory, radio firmware — every field it shows today. (Mockup, approved; the mockup's shorthand
  labels do not drop or add fields.)
- Nothing the Dashboard shows today is removed: each item moves into one of the three sections. (My call: the
  mockup was shorthand.)
- The separate Zigbee page goes away: `zigbee.html` and its nav link are removed. (Mockup, approved.)

## Non-goals

- No change to the device API (`GET /device/status`, `/device/zigbee`, `/device/channels`, …): the page uses what
  exists.
- No change to the Thread tool pages (Network, Topology, Routing, Commissioner, Addresses, Tools, Console) or to the
  Thread menu's existing rule (shown only while a radio runs Thread).
- No new Zigbee features (no pairing, device list or network settings on the board: ZHA owns those).
- No visual redesign of the site's style: the page keeps the existing `style.css` look; the mockup's colors and font
  are the mockup's, not a new theme.

## Anti-goals (failure even if it "works")

- Thread controls or Thread-only wording visible while Thread is off, beyond the placeholder.
- A Zigbee poll running when no radio runs Zigbee (today's Dashboard polls `/device/zigbee` only while
  `zigbee_channel` is set; keep that).
- A section that flickers between placeholder and content on each poll, or a page that shows stale stack data after
  the stack stopped.
- A bookmark or link to `/zigbee.html` that ends on a blank page or 404 without a way on.

## Layout (as approved in the mockup)

1. **Device and network**
   - A status line for the board: health ("Board OK", or a board-wide fault), uptime, last reset reason, and the
     Reboot button (today's `#rebootBtn` with its confirm and wait flow). No Thread wording here.
   - **Today's `/device/status` `mode` is the Thread status, not the board's (user, 2026-11-07).** It moves to the
     Thread section. The Device line shows only faults that concern the whole board, and shows them whether or not a
     radio runs Thread: the board unreachable (today's offline notice), storage unavailable, and the C6 running
     factory ESP-Hosted firmware or holding no RCP firmware (today these arrive as `RADIO_FAULT` reasons). Every
     other `RADIO_FAULT` reason, and `STARTING`, `BR_RUNNING`, `SNIFFER`, `NO_RADIO`, belong to the Thread section.
   - Cards: **Radios** — always exactly two static rows, the board's two radio slots (user, 2026-11-07): "C6 (on
     board)" and "USB radio", each with its mode (Thread, Zigbee, RCP proxy, off) from `/device/channels`. The USB
     row says "Not connected" when no USB radio is plugged in (no USB channel present: absent, or listed as
     unavailable). The rows never grow or disappear. It replaces today's "Thread runs on" header chip. **Ethernet** (IPv4, IPv6),
     **Board firmware** (app, slot, storage), **Load and memory** (CPU 10 s, internal RAM free, PSRAM free, chip
     temperature).
   - The trial / OTA-pending and other board-wide banners stay at the top of the page, as today.
2. **Thread** — when a radio runs Thread (`otbr_channel` not null):
   - Status line: today's hero, driven by `mode` and its reasons (role, network name, radio, channel, and today's
     action link such as "Open firmware" for a fault), minus the board-wide faults above.
   - Cards: **Thread network**, **Identifiers**, **Radio co-processor**, **Home Assistant**, **IPv6 prefix
     delegation**, **Packet capture**, and the **Thread addresses** table with its link to Manage (today's cards,
     moved).
   - When no radio runs Thread: placeholder "Thread is not active: no radio runs the Thread border router." + link
     "Set a radio to Thread on the Radios page".
3. **Zigbee** — when a radio runs Zigbee (`zigbee_channel` not null):
   - Status line: state text (today's Zigbee page hero), radio, running since, radio resets.
   - Cards: **Connect Home Assistant**, **Client** (with last client), **Network** (channel, PAN ID, TX power,
     coordinator extended and short address; "devices, pairing and the extended PAN ID are in ZHA"), **Radio
     traffic** (all of today's counters, failed and energy scans included), **Memory**, **Radio firmware** (today's
     Zigbee page cards and fields, unchanged).
   - When none: placeholder "Zigbee is not active: no radio runs the Zigbee coordinator." + link "Set a radio to
     Zigbee on the Radios page".

The nav loses the Zigbee entry. The Thread menu keeps its current rule.

## Constraints

- Frontend only: `p4/components/esp_ot_br_server/frontend/` (HTML, `static/*.js`, `static/style.css`), served from
  the web partition; follow `STYLEGUIDE.md` (vanilla ES5-style JS as in the existing `static/*.js`, the
  `device_pages_harness.js` page-test pattern in `tools/tests`).
- Changes to the vendored `esp_ot_br_server` frontend are recorded in `p4/components/PATCHES.md` (our Zigbee entry,
  currently patch 33; the radio-mux branch owns the nav slot `<li id="nav-slot-zigbee">`, patch 29).
- The P4 serves the page over a slow embedded HTTP server: polling stays as today (status 3 s, diag 10 s, Thread
  10 s, Zigbee 5 s / 2 s while starting) and requests only what the visible sections need.
- The page must work at phone width as the current pages do.

## Left to the builder

- Whether `zigbee.js`'s pure helpers (state text, path, formatting) are kept as a module the Dashboard loads or are
  folded into `status.js`; the page tests must keep covering them either way.
- The exact markup and ids of the moved cards, as long as existing page tests are carried over (renamed where ids
  change) and the four Zigbee states are still tested.
- What `/zigbee.html` does after the page is removed (a redirect to the Dashboard's Zigbee section, or a short page
  that points there), within the anti-goal above.
- Doc updates: `docs/zigbee.md` (the "Zigbee page" section becomes the Dashboard's Zigbee section), `docs/README.md`
  index if it names the page, CHANGELOG.

## Calls I made (not discussed with the user)

- The Home Assistant, prefix delegation and packet capture cards belong to the Thread section (they serve Thread),
  not to Device and network. Shown in the mockup; the user approved the mockup.
- Content missing from the mockup stays and is placed: Reboot, PSRAM and chip temperature in Device and network;
  the Thread addresses table in Thread; every Zigbee page field in Zigbee; the "Thread runs on" chip is replaced by
  the Radios card.
- Field labels follow the API: the Zigbee Network card shows the coordinator's addresses (the extended PAN ID stays
  in ZHA, as today); the mockup's "Link: up" is today's hero action link; "RCP version, chip" is today's Radio
  firmware text.
- The board-wide fault list above (offline, storage unavailable, ESP-Hosted or no RCP firmware on the C6) is my
  reading of today's `RADIO_FAULT` reasons; any other reason is Thread's.
- The radio-mux branch's empty nav slot `<li id="nav-slot-zigbee">` stays in place (it is that branch's markup) but
  our `api.js` no longer fills it.
