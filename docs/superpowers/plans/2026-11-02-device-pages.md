# Device pages redesign: implementation plan

## Context

`device.html` crams status, packet capture, the P4 update, the radio firmware and recovery, the network form and diagnostics
into one page, and the network form is a raw list of fields. You approved the sketches
(https://claude.ai/artifact/YM6s8R1bUGZmnGWcxwQiu2). The spec, `docs/superpowers/specs/2026-11-02-device-pages-design.md`,
is committed as `bffee11` on branch `feat/device-pages`.

It splits the page into five pages under a "Device" dropdown:

| Page | File |
| --- | --- |
| Status | `device.html` |
| Ethernet | `ethernet.html` |
| Firmware | `firmware.html` |
| Packet capture | `capture.html` |
| Diagnostics | `diagnostics.html` |

The Ethernet page gets the setup-cards editor with a review step. This is a frontend-only change: no firmware or API change.

**Execution:** native, in this session, with one whole-branch review at the end on the most capable model. Nothing is flashed.
The on-board test runs in the next batched hardware pass.

**Deviations from the sketches** (already in the spec):
- Ethernet: no live gateway or DNS for DHCP interfaces (the API doesn't report them).
- Firmware: no upload progress bar (fetch has no upload progress).
- Packet capture: no "your network uses channel N"; the channel field is empty, which means the network's channel.
- Diagnostics: no memory bars (diag reports no total).

## Styleguide

`STYLEGUIDE.md`.
- **Frontend code is vendored** (`p4/components/esp_ot_br_server/frontend/`):
  - It keeps the upstream format of `static/device.js`: ES5 `var`/`function`, an IIFE for the pure logic, and the `{key : value}` spacing.
  - It is excluded from the formatters.
  - Every changed or new file is listed in `p4/components/PATCHES.md`.
- **Page structure (as today):**
  - Pure logic goes in a `XxxLogic` IIFE, exported under node through `module.exports`.
  - The page is a factory `XxxPage(env)` that gets the document, fetch, timers, clock and dialogs through `env`, and runs only when `typeof document !== 'undefined'`.
  - Text from the device goes in through `textContent` only.
- **Python tests:** ruff, under `tools/tests/test_<page>_page.py`; each test comment says `On <scenario>: <effect>`.
- **Routine commands:** `make test-tools`, `make lint` and `make build`.
- **Commits:** end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` only.

## Design

- **`static/device_common.js` (new):** moved out of `device.js` unchanged in behavior.
  - Pure part `DeviceCommon`: `wholeNumber`, `errorMessage`, `nextDelay`, `lastSeenText`, `bootObserved`, `shouldPoll`, `formatRemaining`, `checkUploadSize`, `RCP_MAX_BYTES`, `BUNDLE_MAX_BYTES`, `loadText`, `bytesText`, `coreLabel`.
  - Factory `DeviceKit(env, opts)`: `$`, `el`, `setText`, `show`, `note`, `request`, `failure`, `poller`, `runOp`/`setBusy` over `opts.actionIds`, `waitForBoard`, and the trial banner (it polls `/device/network` every 10 s, renders the countdown, and calls `POST /device/network/confirm` from **Keep new settings**).
  - Page scripts take `DeviceCommon` with `require('./device_common.js')` under node, or from the global in the browser.
- **One script per page:**
  - `static/status.js`
  - `static/ethernet.js`
  - `static/firmware.js`
  - `static/capture.js`
  - `static/diagnostics.js`

  `static/device.js` is deleted at the end. Each page's HTML holds:
  - the shared trial banner markup (ids `trialBanner`, `trialRemaining`, `confirmBtn`, `confirmMsg`);
  - `<nav class="nav" id="nav">`;
  - `style.css`, `api.js`, `device_common.js` and the page script.
- **Ethernet markup:** three static interface cards (`data-iface="single|mgmt|thread_bb"`) with fixed ids such as `if-mgmt-vlan`. The script shows and hides them, so the fake DOM in the harness stays id-based.
- **`static/api.js`:** "Device" becomes a `children` dropdown (Status, Ethernet, Firmware, Packet capture, Diagnostics). The existing child-active logic highlights it.
- **`static/style.css`:** new classes only, built on the existing tokens, so no existing rule changes: `.seg`, `.chip`, `.mode-card`, `.tile-grid`/`.tile`, `.hero`, `.bar`, `.dropzone`, `.action-bar`, `.diff-table`.
- **`tools/tests/device_page_harness.js`:**
  - `makeEnv(page, routes)` loads `<page>.html`, `device_common.js` and the page script.
  - `querySelectorAll('[data-iface]')` gets generalized.
  - The STATUS fixture stays; NETWORK and DIAG fixtures are added.
- **Reused, not rewritten** (from `device.js`):
  - `captureUrl` (extended: an empty channel leaves out `channel=`);
  - `radioFirmwareUrl`, `checkUpdateUrl`, `faultHint`, `jumperPresent`, `rcpInstalledText`;
  - `ipv4Valid`, `maskValid`, `ifaceConfig`/`networkConfig`, which become the body builder behind the Ethernet draft;
  - `sendFile`/`sendUrl`/`finishInstall`, `recover`, `showLog`, `reboot`, `stopCapture`, `renderDiag`.

## BDD skeleton

```
DeviceCommon (device_common.js)                 [moved; existing tests move to test_device_common.py unchanged]
    errorMessage / nextDelay / lastSeenText / bootObserved / shouldPoll / formatRemaining / checkUploadSize
        - same results as the current device.js tests
DeviceKit (device_common.js)
    poller                  - no overlap; pauses while hidden; backs off; stops while a long operation runs   [moved]
    runOp / setBusy         - disables every action id of the page; failure enables them again                  [moved]
    waitForBoard            - reloads after outage + success; gives up after 3 min with the hint                [moved]
    trial banner
        On network answer with trial        - banner shown, countdown re-synced from remaining_s
        On no trial                         - banner hidden
        On Keep new settings                - POST /device/network/confirm; result shown in confirmMsg
        On page without banner markup       - no /device/network polling

api.js nav
    On any device page      - "Device" is a dropdown with 5 entries in order; active on each of the 5 paths

StatusLogic (status.js)
    heroFor(status)
        BR_RUNNING          - "Border router running", green, no link
        SNIFFER             - "Capturing packets", text names channel and frames, link capture.html
        RADIO_FAULT         - "Radio fault", faultHint text, link firmware.html
        STARTING            - "Starting", grey
    snifferText(sniffer)    - idle / active with channel, frames, drops / ended by a fault
    pdText(pd)              - state, plus prefix or "none received"
StatusPage
    On status 200           - hero, tiles and raw mode filled; offline line hidden
    On status failure       - offline line "last answer N s ago"
    On diag 200 / failure   - uptime, reset and load tile filled / dashes
    On Reboot               - confirm, POST /device/reboot, waitForBoard              [moved]

EthernetLogic (ethernet.js)
    prefixToMask / maskToPrefix     - 24 <-> 255.255.255.0; 0 and 33 and non-contiguous masks refused
    draftFromActive(network)        - mode and per-interface {tagged, vlan, dhcp, cidr, gw, dns} from active
    validate(draft)
        Tagged with VLAN outside 1..4094        - error on that interface
        Static with a bad or missing CIDR       - error naming the 192.168.1.10/24 form
        Bad gateway or DNS                      - error on that interface
        Split with two untagged                 - global error
        Split with equal VLAN IDs               - global error
        Valid draft                             - no errors
    changes(active, draft)
        Same as active          - empty list
        Mode changed            - "Setup" row, then VLAN and IPv4 rows per interface; missing interface "Not in use"
    requestBody(draft)          - the PUT /device/network shape of today (mask, not prefix)
    reachText(draft)            - static: http://<addr>; DHCP single: DHCP text; DHCP split: names the mgmt VLAN
    taggedVlans(draft)          - the VLAN IDs that need the switch checkbox
EthernetPage
    On first network answer         - draft filled once; later answers refresh live state only
    On picking a mode card          - aria-pressed moves; cards for that mode shown; new interfaces open in edit
    On any edit                     - errors shown inline; action bar text and Review enabled state updated
    On Discard changes              - draft back to active; bar hidden
    On Review changes               - diff table rows = changes(); steps show reachText
    With a tagged interface         - Apply disabled until the checkbox is ticked
    On Apply                        - confirm-free PUT /device/network with requestBody; "Rebooting" message; waitForBoard
    On Apply answered 400 / 409     - message in the review section; buttons enabled again
    On network poll failure         - stale line; action message untouched
    On fallback                     - fallback banner with the error

FirmwareLogic (firmware.js)
    source per card file|url        - the action uses the chosen source only
FirmwarePage
    On status                       - versions, slot and match filled; fault hint highlights the radio card
    On install from file / URL      - same requests, size check, https refusal and messages as today      [moved]
    On a file dropped               - the card's file input takes it
    Jumper fitted / not fitted      - recovery buttons shown / explanation and doc link shown
    On Recover / Show C6 console    - POST recover / GET log as today                                     [moved]

CaptureLogic (capture.js)
    captureUrl(channel, duration)
        Empty channel                   - no channel parameter
        Out-of-range channel/duration   - null                                                            [moved]
    liveCommand(host, url)              - curl -N 'http://<host><url>' | wireshark -k -i -
CapturePage
    On status idle                  - form shown; length segment changes the download href
    On status active                - running card with channel, time left, frames, drops; Stop sends DELETE
    On ended by fault               - the fault line

DiagnosticsLogic (diagnostics.js)
    sortedTasks(tasks)              - descending by 10 s CPU
    stackLow(bytes)                 - true below 1024
DiagnosticsPage
    On diag 200                     - CPU rows and bars, memory rows, lock table, task table with low-stack mark
    On diag failure                 - note shown, old figures kept

Pages (test_device_pages.py)
    every device page               - links style, api.js, device_common.js, its script; nav placeholder; no remote resource
    every page script               - no innerHTML etc.; every $('id') exists in its page; message areas are live regions
    endpoints                       - each endpoint is named by the script of its page and registered in device_api.c
    frontend                        - total under the 256 KB budget

On-board (p4/pytest/test_device_page.py, next hardware pass)
    GET each of the 5 pages and 6 scripts   - 200, right content type, file content
```

## Steps

Each step is TDD: change or move the tests first and watch them fail, then implement, `logrun make test-tools` green, and
commit. The repo copy of this plan goes to `docs/superpowers/plans/2026-11-02-device-pages.md` in step 0.

0. **Plan file:** add `docs/superpowers/plans/2026-11-02-device-pages.md` and commit.
1. **Shared module:**
   - Extract `device_common.js` from `device.js`; `device.js` uses it and `device.html` loads it.
   - Generalize the harness to `makeEnv(page, routes)`.
   - Move the common tests to `test_device_common.py`.
   - No user-visible change.
2. **Nav and styles:**
   - The Device dropdown in `api.js` and the new classes in `style.css`.
   - The nav test is updated.
   - Add a `PATCHES.md` entry.
3. **Diagnostics:** `diagnostics.html` and `diagnostics.js`; the diag section and its code leave `device.html`/`device.js`; `test_diagnostics_page.py`.
4. **Packet capture:**
   - Add `capture.html` and `capture.js`; move the sniffer code out.
   - Extend `captureUrl` for an empty channel; add `liveCommand`.
   - Tests in `test_capture_page.py`.
5. **Firmware:** `firmware.html` and `firmware.js` (OTA, radio, recovery); the code moves out; the source picker and drop zone; `test_firmware_page.py`.
6. **Ethernet:**
   - Add `ethernet.html` and `ethernet.js`, built from the `EthernetLogic` skeleton above.
   - The network code and `networkConfig` move out of `device.js`; `TestNetworkValidation` moves to `test_ethernet_page.py` and is extended.
7. **Status:**
   - Rewrite `device.html` as the status page.
   - Add `status.js` with hero, tiles and reboot.
   - Delete `device.js`.
   - Rename `test_device_page.py` to `test_device_pages.py` and add `test_status_page.py`.
8. **Docs and the on-board test:**
   - Update every page reference in docs (`api.md` Web UI paragraph, `updates.md`, `first-install.md`, `networking.md` "Keep new settings", `sniffer.md`, `diagnostics.md`, `maintenance-jumper.md`, `troubleshooting.md`, `development.md`, README line 74).
   - Add a CHANGELOG Unreleased "Changed" entry.
   - Extend `p4/pytest/test_device_page.py` to the five pages.
   - Run `make lint`.
9. **Review:**
   - Run `logrun make test-tools` and `logrun make build`, the latter to check that the web image packs.
   - Then a whole-branch review on the most capable model, and fix the Critical and Important findings test-first.

## Verification

- `logrun make test-tools`: every page test passes with node; the budget, nav, routing and id checks pass.
- `logrun make build`: `web_0.bin` packs the new files.
- `logrun make lint`.
- A manual look without the board: serve the `frontend/` directory with `python -m http.server` and open each page. API calls fail there, so this only checks layout, the offline and stale states, and Ethernet editing driven from the fake status. Read-only, local.
- Hardware (deferred, batched):
  - `make test-target TEST=p4/pytest/test_device_page.py`;
  - click through the five pages on the board, including one split-mode apply and Keep new settings on the rig;
  - new screenshots for `docs/images/`.
