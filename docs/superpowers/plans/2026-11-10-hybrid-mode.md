# Hybrid Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Thread (OTBR) and Zigbee (Ziggurat) run at the same time, each on its own radio.

**Architecture:** The orchestrator's binding rules lose the Thread/Zigbee exclusion; a Thread move onto the radio that
runs Zigbee becomes a reboot that stores both radios in one commit. The Radios page warns when a save reboots Thread,
and the Dashboard warns when both networks share an 802.15.4 channel. No new task, field or stored record.

**Tech Stack:** ESP-IDF v6.1 C (radio_mux, device_api), the vendored web UI (plain JS, node page tests), pytest on the
board.

**Spec:** `docs/superpowers/specs/2026-11-10-hybrid-mode-design.md` (approved 2026-11-10, decisions in its section 9).

**Branch:** `feat/hybrid-mode` from `master` (`73bc984` or later), one PR.

## Styleguide

The project's `STYLEGUIDE.md` applies, with the pre-commit hooks (`git diff --name-only | xargs uvx pre-commit run
--files`). Points that bite here:

- C: ESP-IDF astyle rules; `snake_case`; public symbols carry the component prefix (`radio_mux_orch_...`); pure
  decisions live in `orch_core.c` and get host tests in `p4/components/radio_mux/test_host/main/test_orch.c`
  (Unity, `TEST_CASE("<scenario>: <effect>", "[radio_mux]")`).
- Web UI: `esp_ot_br_server/frontend` is vendored: every change is recorded in `p4/components/PATCHES.md` (next free
  entry: **35**) and keeps the file's formatting. Page logic goes in the page's `*Logic` module (`RadiosLogic`,
  `ZigbeeLogic`) so `tools/tests` can call it; page tests use `tools/tests/device_js.py` (`logic`, `scenario`, `page`).
- Python tests: `ruff`; on-board tests in `p4/pytest`, one file per feature.
- Commits: Conventional Commits with a scope, each ending in `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Global Constraints

- RF rule: no test forms or joins a network on 802.15.4 channels 11, 15 or 25. Zigbee tests form on **20**; Thread test
  networks use **26**; hybrid on-board tests keep the board's existing Thread dataset and form none.
- No new FreeRTOS task, no new stored record, no new read field. Internal heap: hybrid measured 195.3 KB free (lowest
  153.7 KB); this work spends no further internal RAM.
- Entering or leaving Zigbee still reboots the board (`"reboot": true` in the PUT reply).
- `/device/status` `mode` stays Thread's status in every scenario; `zigbee_channel` as today.
- Tasks 1-5 run host tests, page tests and builds only; nothing is flashed. The board is used only in Task 7.

## Review Focus

1. A Thread move onto the radio that runs Zigbee: must reboot and come back with Thread on the new radio and the old one
   off; a live move would probe a radio Zigbee still holds, and storing only one radio lets the boot-time singleton rule
   (first `otbr` in slot order wins) put Thread back on the C6. Pinned in Task 1 and Task 2.
2. Zigbee entering on the C6 while Thread runs on the C6: Thread leaves the C6 (board `NO_RADIO`), not a refusal and not
   a hybrid. Pinned in Task 1.
3. A second Zigbee while one runs (`ziggurat` on the other radio): still 409 `mode_taken`. Pinned in Task 1.
4. Same-channel notice when Thread's channel or Zigbee's channel is unknown (`-`, no `radio` yet): shows nothing, never
   "channel undefined". Pinned in Task 4.
5. A save on the Radios page that changes only Zigbee's radio while Thread runs elsewhere: says Thread restarts; a save
   with Thread off says nothing about Thread. Pinned in Task 3.

## BDD skeleton

```
radio_mux_orch_check_bind (orch_core.c)
    ziggurat on the dongle while Thread runs on the C6
        - returns RADIO_MUX_APPLY_REBOOT
    otbr on the C6 (off) while Zigbee runs on the dongle
        - returns RADIO_MUX_MOVE with move_from = n (a start)
    otbr on the dongle that runs Zigbee while Thread runs on the C6
        - returns RADIO_MUX_APPLY_REBOOT
        - sets move_from to the C6's index
    otbr on the dongle that runs Zigbee while no channel runs Thread
        - returns RADIO_MUX_APPLY_REBOOT with move_from = n
    ziggurat on the C6 while Thread runs on the C6 and nothing runs Zigbee
        - returns RADIO_MUX_APPLY_REBOOT
    ziggurat on the dongle while Zigbee runs on the C6
        - returns RADIO_MUX_REFUSE_MODE_TAKEN
    otbr on the dongle while Thread runs on the C6 and nothing runs Zigbee
        - returns RADIO_MUX_MOVE with move_from = the C6 (unchanged)
radio_mux_orch_modes_after (orch_core.c, new)
    a plain change
        - returns the stored modes with only the target's changed
    a reboot that moves Thread (move_from < n)
        - sets the target to otbr and move_from to off
        - leaves every other channel as it was
    a reboot with move_from = n
        - changes the target only
set-mode apply (mux.c, board)
    APPLY_REBOOT that moves Thread
        - stores both channels with mux_store_chans in one commit
        - after the reboot: Thread on the new radio, the old one off, Zigbee unbound
device_api channels_json
    any PUT
        - never answers 409 hybrid_not_supported
RadiosLogic.rebootNote(snap, ch, pick, status)
    pick enters ziggurat while Thread runs on another radio
        - returns the restart text
    pick leaves ziggurat while Thread runs on another radio
        - returns the restart text
    pick is otbr on the radio that runs Zigbee
        - returns the move-with-reboot text
    Thread runs nowhere, or the pick does not enter or leave ziggurat
        - returns ""
Radios page dialog
    a draft with a non-empty rebootNote
        - shows #zigbeeNote with that text
    otherwise
        - hides #zigbeeNote
    a 409 reply
        - has no hybrid_not_supported text left
ZigbeeLogic.sharedChannelText(threadChannel, z)
    Thread and Zigbee on the same channel
        - returns the warning with the channel number
    different channels
        - returns ""
    either channel unknown ("-", null, no radio)
        - returns ""
Dashboard Zigbee section
    Thread and Zigbee on one channel
        - shows #zg-shared with the warning and a link to /radios.html
    otherwise
        - hides #zg-shared
on board: test_hybrid.py
    Thread on one radio, Zigbee on the other (both assignments)
        - /device/status mode BR_RUNNING with otbr_channel and zigbee_channel set
        - /device/zigbee state running
    Zigbee load for 60 s
        - no failed round trip
        - pings to a Thread router: no loss
    reboot
        - both back within 60 s
    otbr PUT on Zigbee's radio
        - reboot; Thread on that radio, the old radio off, zigbee_channel null
    C6 firmware push with Thread on the C6 and Zigbee on the dongle
        - Zigbee stays running throughout
    dongle unplug (manual)
        - Thread stays BR_RUNNING; Zigbee no_radio, then running after the replug
```

---

### Task 1: Binding rules without the exclusion

**Files:**
- Modify: `p4/components/radio_mux/orch_core.c` (`radio_mux_orch_check_bind`)
- Modify: `p4/components/radio_mux/include/radio_mux_core.h` (remove `RADIO_MUX_REFUSE_HYBRID`)
- Modify: `p4/components/radio_mux/include/radio_mux_orch_core.h` (the check's doc comment)
- Modify: `p4/components/device_api/channels_json.c`, `channels_json.h`, `include/device_api.h` (drop the 409 text)
- Test: `p4/components/radio_mux/test_host/main/test_orch.c`

**Interfaces:**
- Produces: `radio_mux_orch_check_bind` keeps its signature; for `otbr` on a channel whose stored mode has
  `reboot_on_change` it returns `RADIO_MUX_APPLY_REBOOT` and sets `*move_from` to the channel that runs `otbr`, or `n`.

- [ ] **Step 1: Write the failing tests.** Replace the test case "On otbr while a ziggurat consumer is active, or
  ziggurat while otbr is active: returns RADIO_MUX_REFUSE_HYBRID" by one test case per `radio_mux_orch_check_bind`
  scenario of the BDD skeleton, with a rules helper whose ziggurat rule has `.reboot_on_change = true` (the real
  instance's value). Each asserts the verdict and, where the skeleton says so, `move_from`.
- [ ] **Step 2: Run them and watch them fail.** `make test-host`; expected: the hybrid cases fail with
  `RADIO_MUX_REFUSE_HYBRID`, and "otbr on the dongle that runs Zigbee while Thread runs on the C6" fails with
  `RADIO_MUX_MOVE`.
- [ ] **Step 3: Implement.** Remove the exclusion block. Move the "otbr on a channel leaving a mode that reboots" check
  before the singleton-limit check, so it wins over a move, and have it set `*move_from` (the first other `otbr`
  channel, or `n`). Remove `RADIO_MUX_REFUSE_HYBRID` and its mapping in `channels_json.c` and the doc lists.
- [ ] **Step 4: Run the host tests.** `make test-host`; expected: all projects pass.
- [ ] **Step 5: Commit** `feat(radio_mux): Thread and Zigbee may bind at once; Thread onto Zigbee's radio reboots`.

### Task 2: A reboot that moves Thread stores both radios

**Files:**
- Modify: `p4/components/radio_mux/orch_core.c`, `include/radio_mux_orch_core.h` (new pure function)
- Modify: `p4/components/radio_mux/mux.c` (set-mode apply path, around `check_mode` and `mux_store_chan`)
- Test: `p4/components/radio_mux/test_host/main/test_orch.c`

**Interfaces:**
- Consumes: Task 1's `*move_from` on `RADIO_MUX_APPLY_REBOOT`.
- Produces: `void radio_mux_orch_modes_after(const radio_mux_chan_view_t *chans, size_t n, size_t target,
  radio_mux_mode_t want, size_t move_from, radio_mux_mode_t *out)`: the stored mode of every channel after the change.

- [ ] **Step 1: Write the failing tests** for the three `radio_mux_orch_modes_after` scenarios of the skeleton,
  asserting every entry of `out`.
- [ ] **Step 2: Run them and watch them fail** (`make test-host`: undefined reference).
- [ ] **Step 3: Implement** `radio_mux_orch_modes_after`, and in `mux.c` use it on `RADIO_MUX_APPLY_REBOOT`: when the
  old Thread channel changes too, set both channels' modes and store them with `mux_store_chans` (one commit),
  restoring both on failure as the single-channel path does; then return for the reboot as today. `check_mode` passes
  `move_from` out (today it fills `from` only for a move).
- [ ] **Step 4: Run** `make test-host` and `make build`; expected: all pass, the build is clean (`-Werror`).
- [ ] **Step 5: Commit** `feat(radio_mux): a reboot that moves Thread stores both radios in one commit`.

### Task 3: Radios page: the reboot note and no hybrid refusal

**Files:**
- Modify: `p4/components/esp_ot_br_server/frontend/static/radios.js` (`RadiosLogic`, `renderDialog`, the 409 texts)
- Modify: `p4/components/esp_ot_br_server/frontend/radios.html` (a `<div class="rd-note rd-warn" id="zigbeeNote">`
  next to `#otbrNote`)
- Modify: `p4/components/PATCHES.md` (entry 35: the hybrid web changes of Tasks 3 and 4)
- Test: `tools/tests/test_radios_page.py`

**Interfaces:**
- Produces: `RadiosLogic.rebootNote(snap, ch, pick, status) -> string` ("" when nothing to say).

- [ ] **Step 1: Write the failing tests:** one per `RadiosLogic.rebootNote` scenario and per dialog scenario of the
  skeleton; replace the `(409, "hybrid_not_supported", "Thread and Zigbee")` case by an assertion that the texts table
  has no `hybrid_not_supported` key. Exact copy:
  - entering or leaving Zigbee: "Saving reboots the board. Thread restarts with it: its devices lose the border router
    for about 20 s."
  - otbr on Zigbee's radio: "Saving reboots the board and moves Thread from <label> to this radio; Zigbee stops. Thread
    devices lose the border router for about 20 s."
- [ ] **Step 2: Run them and watch them fail** (`make test-tools`).
- [ ] **Step 3: Implement** `rebootNote`, show it in `#zigbeeNote` from `renderDialog` (hidden when ""), and use it in
  place of `moveText` when the pick is `otbr` on the radio that runs Zigbee. Remove the `hybrid_not_supported` text.
- [ ] **Step 4: Run** `make test-tools`; expected: all pass.
- [ ] **Step 5: Commit** `feat(web): Radios page says when a save restarts Thread; no hybrid refusal`.

### Task 4: Dashboard: shared-channel notice

**Files:**
- Modify: `p4/components/esp_ot_br_server/frontend/static/zigbee.js` (`ZigbeeLogic`, `ZigbeeSection`)
- Modify: `p4/components/esp_ot_br_server/frontend/static/status.js` (pass Thread's channel to the Zigbee section)
- Modify: `p4/components/esp_ot_br_server/frontend/index.html` (a `<div class="hidden form-hint" id="zg-shared">` in
  `#zigbee-on`, above `#zg-note`)
- Modify: `p4/components/PATCHES.md` (entry 35)
- Test: `tools/tests/test_zigbee_page.py`, `tools/tests/test_status_page.py`

**Interfaces:**
- Produces: `ZigbeeLogic.sharedChannelText(threadChannel, z) -> string`; `ZigbeeSection(...).setThreadChannel(ch)`
  called by `status.js` with `threadView(...).channel` on every Thread refresh (`null` when Thread is not active).

- [ ] **Step 1: Write the failing tests** for each `sharedChannelText` and Dashboard scenario of the skeleton. Exact
  copy: "Thread and Zigbee both use channel <n>. They share the air, so each slows the other under load: move one of
  them to another channel." with "Radios" linking `/radios.html`.
- [ ] **Step 2: Run them and watch them fail** (`make test-tools`).
- [ ] **Step 3: Implement** `sharedChannelText` (channels compared as numbers; anything unknown gives ""),
  `setThreadChannel`, and the render of `#zg-shared` on each Zigbee render and each `setThreadChannel`.
- [ ] **Step 4: Run** `make test-tools` and `make build`; expected: all pass.
- [ ] **Step 5: Commit** `feat(web): Dashboard warns when Thread and Zigbee share a channel`.

### Task 5: Docs

**Files:**
- Modify: `docs/zigbee.md` ("No hybrid" limit → a short "Thread and Zigbee together" section: both assignments, the
  reboot on entering or leaving Zigbee, the shared-channel notice, "turn Zigbee off before going back to an older
  firmware"), `docs/radios.md`, `docs/api.md` (409 list), `CHANGELOG.md` (feature line and the downgrade note).

- [ ] **Step 1: Edit** the four files; no other doc mentions `hybrid_not_supported` (`git grep -n hybrid_not_supported`
  shows only `docs/superpowers`).
- [ ] **Step 2: Run** `make docs`; expected: strict build without warnings.
- [ ] **Step 3: Commit** `docs: Thread and Zigbee together`.

### Task 6: On-board tests (written, not run)

**Files:**
- Create: `p4/pytest/test_hybrid.py` (markers `usb_dongle`; the push case also needs `OTBR_RCP_IMAGE`; the unplug case
  is `manual`)
- Modify: `p4/pytest/pytest.ini` only if a new marker is needed

**Interfaces:**
- Consumes: `p4/pytest/test_zigbee.py`'s helpers (`zigpy_app` on `TEST_CHANNEL` 20, `set_mode`, `wait_state`,
  `zigbee_status`) and `zigbee_device.ZigbeeTestDevice` (env `ZIGBEE_DEVICE_PORT`); Thread pings through the board's
  `ot ping <rloc> 32 <count>` console command to a router from `GET /topology` other than the board (skip when there
  is none).

- [ ] **Step 1: Write** one test per on-board scenario of the skeleton. Each restores the stored modes it found
  (Thread on its radio, the dongle off) in teardown. No test forms a Thread network.
- [ ] **Step 2: Check** `uvx ruff check p4/pytest/test_hybrid.py` and `pytest --collect-only p4/pytest/test_hybrid.py`
  list the tests without errors.
- [ ] **Step 3: Commit** `test(hybrid): on-board tests for Thread and Zigbee together`.

### Final review, then Task 7: the batched on-board pass

After Tasks 1-6: `make test-host`, `make test-tools`, `make build`, `make docs` all pass; then the whole-branch review
and its fixes. Then, with the user's go-ahead and the P4 handed over by its current holder:

- [ ] **Step 1:** Re-read `/device/status` `app.version`; OTA the branch's test build.
- [ ] **Step 2:** Run `p4/pytest/test_hybrid.py -m "usb_dongle"` (both assignments), the manual unplug with the user,
  then `test_zigbee.py` and `test_channels.py` as regression, with `ZIGBEE_DEVICE_PORT` set to the rig's own light.
- [ ] **Step 3:** Record internal heap (free, lowest) in both assignments against the spec's section 2.
- [ ] **Step 4:** Put the board back as found (master test build, Thread on the C6, dongle off) and report.
