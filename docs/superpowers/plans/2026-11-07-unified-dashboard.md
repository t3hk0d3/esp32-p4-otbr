# One Dashboard for Thread and Zigbee — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Merge the Dashboard and the Zigbee page into one Dashboard with three sections — Device and network, Thread,
Zigbee — where a stack that is not running shows a one-line placeholder.

**Architecture:** Frontend only, in the vendored `esp_ot_br_server` frontend. `status.js` keeps the page
(`StatusPage`) and gains pure helpers for the board line, the Thread status line and the two-row Radios card.
`zigbee.js` stops being a page: it keeps `ZigbeeLogic` and turns `ZigbeePage` into `ZigbeeSection`, which renders
the Zigbee section inside the Dashboard. `zigbee.html` becomes a stub that sends visitors to the Dashboard.

**Tech Stack:** vanilla ES5-style browser JS (`static/*.js`, `'use strict'`, IIFE logic modules that also `require`
under node), `DeviceCommon.DeviceKit`, page tests in Python driving node (`tools/tests/device_js.py`,
`device_pages_harness.js`), `ruff` for the Python tests.

**Spec:** `docs/superpowers/specs/2026-11-07-unified-dashboard-design.md` (approved 2026-11-07; mockup
https://claude.ai/artifact/XtTxw4gB7qY86w9M3gjdZR).

## Styleguide

`STYLEGUIDE.md` applies. For this frontend in particular (vendored code, kept in upstream formatting, excluded
from formatters): follow the existing `static/*.js` conventions — a pure `XLogic` IIFE that `module.exports` under
node, a page factory taking `env`, ids in kebab-case with the section prefix (`t-` board/Thread tiles, `zg-` Zigbee),
`kit.setText` / `kit.show` / `kit.el` only (never `innerHTML` with data). Page tests: one pytest class per BDD
module, one test per effect, test names say scenario and effect, each test opens with a `# On <scenario>: <effect>`
comment as in `tools/tests/test_zigbee_page.py`. Every vendored-file change is recorded in
`p4/components/PATCHES.md` entry 33.

## Global Constraints

- No device API change: only `GET /device/status`, `/device/diag`, `/device/channels`, `/device/zigbee`,
  `/device/network`, `/device/names`, `/get_properties`, `/ipaddr`, `POST /device/reboot` as they are today.
- Section order: Device and network, Thread, Zigbee. Section headings exactly "Device and network", "Thread", "Zigbee".
- Placeholders, exact copy: "Thread is not active: no radio runs the Thread border router." + link "Set a radio to
  Thread on the Radios page" (`/radios.html`); "Zigbee is not active: no radio runs the Zigbee coordinator." + link
  "Set a radio to Zigbee on the Radios page" (`/radios.html`).
- Radios card: always exactly two rows, labels "C6 (on board)" and "USB radio". Mode words: `otbr` → "Thread",
  `ziggurat` → "Zigbee", `rcp-proxy` → "RCP proxy", `off` → "Off". USB row "Not connected" when no USB channel is
  present (none listed, or every USB channel's mode is `unavailable`). "-" when `/device/channels` could not be read.
- Board-wide faults (Device line, shown whatever runs Thread): board unreachable (today's offline line), and
  `mode == "RADIO_FAULT"` with `fault_reason` one of `storage unavailable`, `esp-hosted firmware`,
  `no rcp firmware`. Every other mode and reason is Thread's.
- Polling stays: status 3 s, diag 10 s, Thread 10 s (only in `BR_RUNNING`), Zigbee 5 s / 2 s while `starting`
  (only while `zigbee_channel` is set), network 10 s; `/device/channels` 10 s (new, for the Radios card).
- Nothing today's two pages show is dropped (Reboot, PSRAM, chip temperature, Thread addresses, every Zigbee field).
- The radio-mux nav slot `<li id="nav-slot-zigbee">` (patch 29) stays in `api.js`'s menu, empty and hidden.

## BDD skeleton

```
StatusLogic
    boardHealthFor(status)
        On any mode other than RADIO_FAULT
            - returns {tone: 'ok', title: 'Board OK', text: '', link: null}
        On RADIO_FAULT with fault_reason 'storage unavailable' | 'esp-hosted firmware' | 'no rcp firmware'
            - returns tone 'fault', title 'Board fault', text = today's faultText(reason)
            - links /firmware.html ('Open firmware') for the two firmware reasons, no link for storage
        On RADIO_FAULT with any other reason
            - returns the 'Board OK' line (the fault is Thread's)
    threadHeroFor(status)
        On BR_RUNNING / SNIFFER / STARTING / NO_RADIO / other RADIO_FAULT reasons
            - returns exactly today's heroFor result for that status
        On RADIO_FAULT with a board-wide reason
            - returns tone 'fault', title 'Thread cannot start', text 'See the board status above.', link null
    isThreadActive(status)
        On otbr_channel null
            - returns false
        On otbr_channel set, or the field missing (older firmware)
            - returns true
    radiosView(channelsBody)
        On a body listing c6 and one present USB channel
            - returns [{slot: 'C6 (on board)', mode: <word>}, {slot: 'USB radio', mode: <word>}]
        On a body with no USB channel, or only USB channels in mode 'unavailable'
            - USB row mode is 'Not connected'
        On null (request failed)
            - both rows read '-'
        On two present USB channels
            - the USB row shows the first present one (the mux supports one at a time)
StatusPage (index.html)
    refreshStatus
        On BR_RUNNING with a Thread channel
            - Device line shows 'Board OK'; Thread section content shown, placeholder hidden
        On otbr_channel null
            - Thread content hidden, placeholder shown with its link; Thread data cleared (no stale values)
        On a board-wide fault while Thread is off
            - Device line shows the fault; Thread placeholder still shown
        On zigbee_channel set
            - Zigbee content shown, placeholder hidden; the Zigbee poll is woken
        On zigbee_channel null
            - Zigbee placeholder shown; /device/zigbee is not requested
        On zigbee_channel going from set to null
            - Zigbee section values are cleared and the poll stops asking
        On the status request failing
            - offline line shown as today; sections keep their state (no flip to placeholders)
    refreshChannels
        On a channels body
            - Radios card shows the two rows from radiosView
        On a failed request
            - both rows read '-'
    reboot
        On click
            - unchanged behaviour (confirm, POST, wait), button now in the Device line
ZigbeeSection (zigbee.js, mounted by StatusPage)
    render(body)
        On running / waiting / starting / no_radio / incompatible / idle
            - same hero, client, network, traffic, memory and firmware texts as today's Zigbee page, in zg-* ids
              inside the Dashboard
    refresh
        On 409 (no Zigbee radio)
            - the section shows its placeholder
        On a failed request
            - the section's note shows the failure text, last values stay
    copy button
        On click
            - copies ws://<host>/zigbee, button reads 'Copied' then 'Copy' after COPY_RESET_MS
api.js nav
    On any status
        - no Zigbee link is added; the nav slot stays empty and hidden; the Thread menu rule is unchanged
zigbee.html (stub)
    On opening /zigbee.html
        - a short page links to /index.html#zigbee and redirects there; it loads no Zigbee script
```

## Review Focus

1. A stack stopping while the page is open: Thread or Zigbee values must not linger under a placeholder
   (tests in Task 2 and Task 3: "set → null clears").
2. `/device/channels` failing or slow: the Radios card must read "-", never "Not connected", which would tell the
   user to plug something in (Task 1 `radiosView(null)`, Task 2 page test).
3. Older firmware without `otbr_channel` / `zigbee_channel` in `/device/status`: Thread treated as active (today's
   behaviour), Zigbee as absent (Task 1 `isThreadActive`, Task 3 test with the field missing).
4. A board-wide fault with Thread off (`storage unavailable` while the C6 runs Zigbee): the Device line must still
   say it (Task 1, Task 2).
5. Bookmarks to `/zigbee.html` and the old Dashboard "Open" link: no dead end (Task 4 stub test; the Dashboard's old
   Zigbee card and its link are removed in Task 3).

---

### Task 1: StatusLogic helpers for the board line, the Thread line and the Radios card

**Files:**
- Modify: `p4/components/esp_ot_br_server/frontend/static/status.js` (StatusLogic only)
- Test: `tools/tests/test_status_page.py` (new class `TestDashboardLogic`; existing `heroFor` tests renamed to
  `threadHeroFor`)

**Interfaces:**
- Produces: `StatusLogic.boardHealthFor(s) -> {tone, title, text, link}`, `StatusLogic.threadHeroFor(s) -> {tone,
  title, text, link}` (today's `heroFor`, renamed and narrowed), `StatusLogic.isThreadActive(s) -> bool`,
  `StatusLogic.radiosView(body|null) -> [{slot, mode}, {slot, mode}]`, `StatusLogic.BOARD_FAULTS` (the three reasons).
  `heroFor` is removed (no other caller; grep first).

- [ ] **Step 1: Write the failing tests** — one per effect under `StatusLogic` in the BDD skeleton, through
  `logic("status", ...)`. Values: the three board-wide reasons and one Thread reason (`rcp incompatible`); the
  Radios bodies use the shapes of `/device/channels` as in `tools/tests/test_device_pages.py` fixtures (c6 +
  `usb-1a86-7523`).
- [ ] **Step 2: Run** `make test-tools` (or `pytest tools/tests/test_status_page.py -q`): new tests FAIL with
  "is not a function".
- [ ] **Step 3: Implement** the four exports in `StatusLogic`. `threadHeroFor` returns today's `heroFor` result
  except for the board-wide reasons. `radiosView` takes the first channel with `kind == "usb"` and mode not
  `unavailable`.
- [ ] **Step 4: Run** the tests: PASS, and every existing `test_status_page.py` test still passes after the rename.
- [ ] **Step 5: Commit** `feat(web): Dashboard logic for the board line, the Thread line and the Radios card`.

### Task 2: Dashboard markup and the Device and network / Thread sections

**Files:**
- Modify: `frontend/index.html` (three `<section>` blocks with `id="sec-device"`, `id="sec-thread"`,
  `id="sec-zigbee"`; Device line with `#rebootBtn`, uptime, reset; Radios card `#t-radio-c6`, `#t-radio-usb`; Thread
  status line (today's hero ids `hero`, `h-title`, `h-text`, `h-link`, `h-role` move here); Thread cards incl. the
  addresses table; placeholder `#thread-off` with the exact copy; remove `#h-thread-on` and `#h-mode`)
- Modify: `frontend/static/status.js` (`StatusPage`: render the Device line from `boardHealthFor`, the Thread line from
  `threadHeroFor`, toggle `#sec-thread` content vs `#thread-off` from `isThreadActive`; new `refreshChannels` poller
  (10 s) filling the Radios rows; drop `renderThreadOn`; stop calling `kit.renderThreadNotice` on this page, so the
  shared "no Thread radio" banner `#threadNotice` never duplicates the placeholder — a test asserts it stays hidden
  while Thread is off)
- Modify: `frontend/static/style.css` only for section headings and the placeholder, reusing existing card classes
- Test: `tools/tests/test_status_page.py` (class `TestDashboardSections`)

**Interfaces:**
- Consumes: Task 1's four exports.
- Produces: `StatusPage(env).refreshChannels() -> Promise<bool>`; ids above for later tasks and tests.

- [ ] **Step 1: Write the failing tests** — one per effect under `StatusPage refreshStatus` (Thread and board
  rows), `refreshChannels` and `reboot` in the skeleton, via `scenario(page("index", routes))`.
- [ ] **Step 2: Run**: FAIL (missing ids / functions).
- [ ] **Step 3: Implement** the markup and `StatusPage` changes.
- [ ] **Step 4: Run** `make test-tools`: PASS, existing Dashboard tests updated only where an id moved.
- [ ] **Step 5: Commit** `feat(web): Dashboard sections for the board and Thread, two-row Radios card`.

### Task 3: Zigbee section inside the Dashboard

**Files:**
- Modify: `frontend/static/zigbee.js` (`ZigbeePage` → `ZigbeeSection(env, kit)`: same render/refresh/copy, no own
  `init`/`start`/poller and no auto-start at the bottom; exports `module.exports.Section`)
- Modify: `frontend/index.html` (Zigbee section with today's `zigbee.html` cards and `zg-*` ids, plus placeholder
  `#zigbee-off` with the exact copy; load `static/zigbee.js` before `static/status.js`; remove the old `#t-zg-card`)
- Modify: `frontend/static/status.js` (`StatusPage` mounts `ZigbeeSection` with its kit; toggles section vs placeholder
  from `zigbee_channel`; the Zigbee poller uses `ZigbeeLogic.pollMs`, asks only while `zigbee_channel` is set, clears
  the section when it goes null; remove `renderZigbee*`, `zigbeeStateText`)
- Test: `tools/tests/test_zigbee_page.py` (`TestZigbeePage` → `TestZigbeeSection` against `page("index", …)`;
  `TestDashboardZigbeeCard` replaced by section tests), `tools/tests/device_pages_harness.js` (`SCRIPTS.index`
  needs `zigbee.js` too, or `status.js` requires it under node like `device_common.js`)

**Interfaces:**
- Consumes: Task 2's sections and `StatusPage` kit.
- Produces: `ZigbeeSection(env, kit) -> {render(body), renderNone(), clear(), refresh() -> Promise<bool>,
  bindCopy()}`.

- [ ] **Step 1: Move today's `TestZigbeePage` tests** to run on `page("index", …)` with `zigbee_channel` set, and
  add one test per new effect (placeholder, set → null clears, not polled without a channel, field missing on older
  firmware).
- [ ] **Step 2: Run**: FAIL.
- [ ] **Step 3: Implement** `ZigbeeSection` and the `StatusPage` wiring.
- [ ] **Step 4: Run** `make test-tools`: PASS.
- [ ] **Step 5: Commit** `feat(web): the Zigbee section moves into the Dashboard`.

### Task 4: Retire the Zigbee page and its nav link

**Files:**
- Modify: `frontend/zigbee.html` → a stub: title "Zigbee moved", one sentence and a link to `/index.html#zigbee`,
  `<meta http-equiv="refresh" content="0; url=/index.html#zigbee">`, no scripts but `api.js` for the menu
- Modify: `frontend/static/api.js` (remove `showZigbeeSlot` and its call; keep the empty hidden slot)
- Test: `tools/tests/test_zigbee_page.py` (`TestZigbeeNav` → the slot stays empty and hidden for any status; a stub
  test asserts the refresh target and that no `zigbee.js` is loaded)

- [ ] **Step 1: Write the failing tests** (nav and stub, per the skeleton).
- [ ] **Step 2: Run**: FAIL.
- [ ] **Step 3: Implement** the stub and the `api.js` change.
- [ ] **Step 4: Run** `make test-tools`: PASS.
- [ ] **Step 5: Commit** `feat(web): the Zigbee page becomes a pointer to the Dashboard`.

### Task 5: Docs and the patch log

**Files:**
- Modify: `docs/zigbee.md` (the "Zigbee page" section → "The Dashboard's Zigbee section"; Home Assistant setup
  points to the Dashboard), `docs/README.md` and `docs/api.md` only where they name the Zigbee page,
  `p4/components/PATCHES.md` entry 33 (frontend part rewritten: Dashboard sections, `zigbee.js` as a section,
  `zigbee.html` stub, `api.js` no longer fills patch 29's slot), `CHANGELOG.md` (Changed: one Dashboard for the board,
  Thread and Zigbee; the Zigbee page moved into it).

- [ ] **Step 1: Edit** the files above.
- [ ] **Step 2: Run** `make docs` and `uvx pre-commit run --files <changed>`: clean.
- [ ] **Step 3: Commit** `docs: one Dashboard for Thread and Zigbee`.

### Final verification

- [ ] `make test-tools`, `make test-host`, `make build` all pass; `git grep -n "zigbee.html"` shows only the stub,
  docs history and tests.
- [ ] Not on a board in this plan: the P4 is with the radio-mux session. The on-board look is checked in the next
  batched hardware pass (memory: no flashing inside tasks).
