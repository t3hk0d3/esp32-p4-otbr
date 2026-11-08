# Built-in RCP maintenance out of br_core: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The built-in RCP's firmware push and ROM recovery live in `builtin_rcp`, not in Thread's br_core, and the
push's answer waits for whatever mode runs on the C6 (Thread, Zigbee or the proxy), not only for Thread.

**Architecture:** `builtin_rcp` gets the two entry points device_api calls. They run the maintenance through
`radio_mux_maintain`, as today. The wait for the mode to run again moves from br_core into the orchestrator
(`radio_mux_maintain`), as a decision on the mode state the instance reports. Thread keeps only what is its own: its
answer to REINIT, now with today's refusal texts. br_core loses `run_maintenance` and its public wrappers.

**Tech Stack:** ESP-IDF v6.1, C (FreeRTOS), Unity host tests on the linux target, pytest-embedded on the board.

**Spec:** `docs/superpowers/specs/2026-11-08-channel-mode-orchestrator-design.md` (§4.1 channels own their
maintenance, §4.2 instance states, §4.3 the orchestrator, §6 the API). This plan adds the design below; the user asked
for the move on 2026-11-08 ("having firmware updates part of thread stack feels meh").

**Styleguide:** `STYLEGUIDE.md` (component prefixes `builtin_rcp_`, `radio_mux_`, `mode_fsm_`, `device_api_`; tests
`TEST_CASE("On <scenario>: <effect>", "[<component>]")`; errors as `esp_err_t` with a detail buffer; astyle and ruff
through pre-commit). Nothing new is needed in it.

## Design

What moves and where:

| Today (br_core) | After |
|---|---|
| `br_core_push_radio_firmware`, `br_core_rom_recover` | `builtin_rcp_push_firmware`, `builtin_rcp_rom_recover` |
| `run_maintenance`: wait for Thread to run on the C6 again (`core_radio_input_pending`, mode STARTING, 180 s) | `radio_mux_maintain` waits for the bound instance's reported state, for any mode |
| `mode_fsm_radio_job_result` | `builtin_rcp_job_result` (same rules, the mode's outcome instead of Thread's) |
| "the image is installed, but OpenThread did not start on it: <fault>" | `builtin_rcp_restart_detail`: the same text for Thread, "Zigbee" or "rcp-proxy" for the others |
| Refusal during a move, before asking | device_api's pre-check (immediate 409) and Thread's REINIT refusal (the race) |
| `br_core_c6_job_touches_openthread` | a static helper in device_api (Thread stored on the built-in channel) |
| Refusals of a STARTING or SNIFFER Thread with no text (ruling 31) | Thread's REINIT refusal carries today's texts |

My calls, each a visible change or a dropped behavior. Each is listed again in the final report.

1. **The push answer covers every mode.** With Zigbee or the proxy on the C6, a push that installed but whose mode did
   not run again answers 502 `restart_failed`, as Thread's does today. Before, it answered 200.
2. **The wait reads reported states.** At the end of a maintenance, before the orchestrator binds the instance back,
   it shows the instance's state as `starting`. The push then waits until the instance reports a state that is not
   `""`, `starting` or `releasing`, or until 180 s pass. Up states: Thread `running` and `sniffer`, Zigbee `running`,
   the proxy's `listening`, `connecting`, `backoff` and `connected`. Any other state counts as down.
3. **The C6 push no longer forces a fault state.** If Thread is still starting after 180 s, the push answers
   `restart_failed` "still starting after 180 s". br_core no longer writes "radio restart failed" into Thread's state;
   Thread keeps starting.
4. **No reboot after a push that follows an abandoned start.** Today a push that worked after an abandoned OpenThread
   start reboots the board. After the move it answers `restart_failed` with Thread's reason "an earlier OpenThread
   start was abandoned: reboot", and the board does not reboot by itself, as moves already behave (docs/radios.md
   "No reboot"). Today's reboot path is not reachable from a test hook (test_rcp_push.py:94).
5. **A C6 push no longer resets the compatibility of a USB radio.** Today a C6 push while Thread runs on a dongle
   marks the dongle's RCP compatibility as unknown. Thread's own REINIT acceptance already forgets the verdict when
   Thread is on the C6. That path stays.
6. **The "not from the OpenThread task" guard goes.** device_api tasks are the only callers. A call from the
   OpenThread task would end in Thread's 45 s drain timeout (409), not in a hang.

What stays: every HTTP route, status code and error code; device_api's pre-check before the self-update bundle; the
proxy's `c6_busy` detail; the NVS layout; Thread's REINIT drain, inbox and verdicts.

## BDD skeleton

```
radio_mux_orch_core (radio_mux_orch_settle)
    radio_mux_orch_settle(chan_state, instance, mode_state)
        On no instance stored on the channel
            - returns RADIO_MUX_SETTLE_NONE
        On the channel in fault or absent
            - returns RADIO_MUX_SETTLE_DOWN
        On the channel still in reinit
            - returns RADIO_MUX_SETTLE_WAIT
        On a ready channel whose state is "", "starting" or "releasing"
            - returns RADIO_MUX_SETTLE_WAIT
        On a ready channel whose state is running, sniffer, listening, connecting, backoff or connected
            - returns RADIO_MUX_SETTLE_UP
        On a ready channel whose state is fault, no_radio, incompatible, idle or off
            - returns RADIO_MUX_SETTLE_DOWN

radio_mux (runtime, on the board)
    radio_mux_maintain
        On a maintenance whose channel comes back ready with an instance stored on it
            - the channel's mode_state is "starting" before BIND/BACK is delivered
            - returns after the instance reported an up or down state, or after RADIO_MUX_SETTLE_MS (180000)
            - out->mode, out->settle, out->mode_state, out->mode_reason hold the outcome
        On a maintenance whose channel ends in fault
            - returns at once with out->settle DOWN and the state the orchestrator set (radio_mux_orch_lost_state)
        On a maintenance of a channel with no instance stored (mode off)
            - returns at once with out->settle NONE
        On a refusal
            - out->ran false, out->settle NONE, nothing waited for

builtin_rcp_core
    builtin_rcp_job_result(base_only, job_err, settle)
        On a push or a recovery that failed
            - returns job_err, whatever the mode did
        On a base-only push or a ROM recovery that worked
            - returns ESP_OK whatever the mode did (the status shows "no rcp firmware")
        On a push that worked and a mode that is up, or no mode
            - returns ESP_OK
        On a push that worked and a mode that is down or still waiting
            - returns RCP_SYNC_ERR_RESTART
    builtin_rcp_restart_detail(out, cap, maint)
        On Thread down with a reason
            - writes "the image is installed, but OpenThread did not start on it: <reason>"
        On Zigbee down with a reason
            - writes "the image is installed, but Zigbee did not start on it: <reason>"
        On the proxy down with no reason
            - writes "the image is installed, but rcp-proxy did not start on it: <state>"
        On a mode still waiting after the timeout
            - writes "the image is installed, but <who> did not start on it: still starting after 180 s"
        On a buffer too small
            - writes a truncated, terminated string

builtin_rcp (runtime, on the board)
    builtin_rcp_push_firmware(source, detail, cap)
        On a NULL source or read
            - returns ESP_ERR_INVALID_ARG; nothing asked
        On a refusal by the orchestrator or the instance
            - returns its error and detail; for RADIO_MUX_ERR_PEER_CONNECTED the detail is
              "c6_busy: rcp-proxy on the C6 has a peer"
        On a push that ran
            - returns builtin_rcp_job_result(done.base_only, err, done.settle)
            - on RCP_SYNC_ERR_RESTART, detail is builtin_rcp_restart_detail
    builtin_rcp_rom_recover(detail, cap)
        On no maintenance jumper
            - returns ESP_ERR_NOT_SUPPORTED; nothing asked
        On a recovery that ran
            - returns its own result (builtin_rcp_job_result with base_only true)

mode_fsm (Thread)
    mode_fsm_reinit_refusal(mode, moving)
        On a move running
            - returns "OpenThread is being moved to another radio"
        On SNIFFER
            - returns "a capture is running"
        On STARTING
            - returns "the border router is starting"
        On BR_RUNNING, RADIO_FAULT or NO_RADIO, no move
            - returns NULL

br_core (Thread, on the board)
    reinit (br_radio)
        On a REINIT while a move runs
            - refuses with ESP_ERR_INVALID_STATE and "OpenThread is being moved to another radio"; OpenThread untouched
        On a REINIT in STARTING or SNIFFER
            - refuses with today's text (mode_fsm_reinit_refusal)

device_api
    device_api_radio_job_busy(mode, thread_on_c6, moving)
        On a move running (whatever the channel)
            - returns "OpenThread is being moved to another radio"
        On Thread on the C6 in SNIFFER or STARTING
            - returns today's texts
        On Thread elsewhere, or in BR_RUNNING, RADIO_FAULT or NO_RADIO, no move
            - returns NULL
    POST /device/radio/firmware, POST /device/radio/recover, the update's RCP step (on the board)
        On every path
            - calls builtin_rcp_push_firmware / builtin_rcp_rom_recover; replies as today
```

## Global Constraints

- No flashing inside tasks. Tasks run host tests and builds only. One batched on-board pass follows the review
  (Task 12 of the orchestrator plan, extended by Task 5 below).
- Never use the aux C6 board (spinel2quic benchmark). Re-read `/device/status` `app.version` before every on-board
  action. Run `fuser` before opening the serial port.
- Commands run as `set -o pipefail; <cmd> 2>&1 | tee <scratchpad>/<name>.log`.
- The channel id stays `builtin-c6`, the NVS id `c6`.
- RADIO_MUX_SETTLE_MS is 180000 (today's BOOT_WAIT_MS wait). Up states and wait states are exactly those in Design 2.
- Today's texts stay byte for byte: "a capture is running", "the border router is starting", "OpenThread is being
  moved to another radio", "c6_busy: rcp-proxy on the C6 has a peer", "the image is installed, but OpenThread did not
  start on it: <reason>".
- Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. No push, no PR unless asked.

## Review Focus

1. An instance that reports nothing after the maintenance's BACK, for example a proxy that only reports on change,
   or Zigbee before its ATTACH event. The push would wait 180 s and answer `restart_failed` although the mode runs.
   Expected: the reply comes within seconds of the mode running. Pinned on the board in Task 5: the Zigbee push reply
   arrives in under 30 s; a proxy push answers 200.
2. A stale state from before the maintenance read as the outcome, for example Zigbee's `no_radio` "radio reinit:
   firmware push". That would answer `restart_failed` at once. Pinned by the settle tests in Task 1 (`starting` waits)
   and by the reset to `starting`, which Task 5 checks on the board.
3. The channel faulting after a push that worked (the boot steps fail). Expected: `restart_failed` with the
   channel's reason at once, not after 180 s. Pinned by Task 1's fault test and Task 2's down test.
4. A push during a move. Expected: 409 with today's text, nothing touched. Pinned by Task 3's busy test and Task 4's
   refusal test.
5. A base-only push (ESP-Hosted installer) and a ROM recovery. Thread then ends in RADIO_FAULT "no rcp firmware",
   yet the reply is 200 (`installed: null` for the installer) or the recovery's own result. Pinned by Task 2's
   job-result tests.

## Files

- `p4/components/radio_mux/include/radio_mux_orch_core.h`, `orch_core.c`: `radio_mux_settle_t`,
  `radio_mux_orch_settle`.
- `p4/components/radio_mux/include/radio_mux.h`: `radio_mux_maint_t` gains the outcome; `RADIO_MUX_SETTLE_MS`.
- `p4/components/radio_mux/mux_api.c` (`radio_mux_maintain`: the wait), `orch.c` (`starting` before the
  maintenance's bind).
- `p4/components/builtin_rcp/include/builtin_rcp.h`, `builtin_rcp.c`: the two entry points.
- `p4/components/builtin_rcp/include/builtin_rcp_core.h`, `builtin_rcp_core.c`: `builtin_rcp_job_result`,
  `builtin_rcp_restart_detail`.
- `p4/components/device_api/device_api.c`, `json_err.c`, `device_api_priv.h`: the calls and the pre-check.
- `p4/components/br_core/br_core.c`, `br_core_radio.c`, `br_core_priv.h`, `include/br_core.h`, `mode_fsm.c`,
  `mode_fsm.h`: removals and `mode_fsm_reinit_refusal`.
- Tests: `radio_mux/test_host/main/test_orch.c`, `builtin_rcp/test_host/main/test_builtin_rcp.c`,
  `device_api/test_host/main/test_errors.c`, `br_core/test_host/main/test_mode_fsm.c`.
- Docs: `docs/api.md` (push and recover rows), `docs/radios.md` (Maintenance), `docs/zigbee.md` (Radio resets and
  loss), `p4/pytest/test_rcp_push.py` (the class docstring names the new function).

---

### Task 1: the orchestrator waits for the bound mode after a maintenance

**Files:** `radio_mux_orch_core.h`, `orch_core.c`, `radio_mux.h`, `mux_api.c`, `orch.c`, `test_orch.c`.

**Interfaces:**
- Produces: `typedef enum { RADIO_MUX_SETTLE_NONE, RADIO_MUX_SETTLE_WAIT, RADIO_MUX_SETTLE_UP, RADIO_MUX_SETTLE_DOWN }
  radio_mux_settle_t;`
- Produces: `radio_mux_settle_t radio_mux_orch_settle(radio_mux_chan_state_t cs, bool instance, const char *mode_state);`
- Produces: `#define RADIO_MUX_SETTLE_MS 180000` (radio_mux.h).
- Produces: `radio_mux_maint_t` gains `radio_mux_mode_t mode; radio_mux_settle_t settle; char mode_state[16];
  char mode_reason[RADIO_MUX_REASON_MAX];`.

- [ ] **Step 1: Write the failing tests** in `test_orch.c`, one `TEST_CASE` per `radio_mux_orch_settle` scenario of
  the skeleton. Each asserts the returned value for every state the scenario names: ready with each up state → UP,
  each down state → DOWN, each wait state → WAIT; fault and absent → DOWN; reinit → WAIT; `instance` false → NONE.
- [ ] **Step 2: Run them.** `bash <scratchpad>/hosttest.sh radio_mux`. Expected: the build fails on
  `radio_mux_settle_t` undeclared.
- [ ] **Step 3: Implement** `radio_mux_orch_settle` in `orch_core.c`, with the two state lists from Design 2.
- [ ] **Step 4: Run them.** Expected: `Failures 0`.
- [ ] **Step 5: Implement the runtime.** These parts run on the owner task, so the board pass in Task 5 checks them.
  - In `orch.c`, the bind at a maintenance's end (`handle_maint_done` through `c6_apply`) first sets the channel's
    `mode_state` to `starting` with an empty reason. Only that bind does this; other binds do not.
  - In `radio_mux_maintain`, after `mux_maint_finish`, poll every 100 ms under `mux_lock`: the channel's `chan_state`,
    whether its stored mode has a registered instance, and `mode_state`/`mode_reason`. Stop at a settle other than
    WAIT, or after RADIO_MUX_SETTLE_MS. Fill `out`.
  - A refusal leaves `settle` at NONE.
  - Update the `radio_mux_maintain` doc comment.
- [ ] **Step 6: Run** `make test-host` and `make build`. Expected: all host projects pass and the build completes.
- [ ] **Step 7: Commit** `feat(radio_mux): a maintenance waits for its channel's mode to run again`.

### Task 2: builtin_rcp runs the push and the ROM recovery

**Files:** `builtin_rcp.h`, `builtin_rcp.c`, `builtin_rcp_core.h`, `builtin_rcp_core.c`, `test_builtin_rcp.c`,
`builtin_rcp/CMakeLists.txt` if a dependency is missing.

**Interfaces:**
- Consumes: Task 1's `radio_mux_maint_t` fields and `radio_mux_settle_t`.
- Produces: `esp_err_t builtin_rcp_push_firmware(const rcp_fw_source_t *source, char *detail, size_t cap);`
- Produces: `esp_err_t builtin_rcp_rom_recover(char *detail, size_t cap);`
- Produces (pure): `esp_err_t builtin_rcp_job_result(bool base_only, esp_err_t job_err, radio_mux_settle_t settle);`
- Produces (pure): `void builtin_rcp_restart_detail(char *out, size_t cap, const radio_mux_maint_t *m);`
- The entry points' doc comments take over br_core.h's (refusals, blocking, base-only). The changes are Design 1, 3,
  4 and 6.

- [ ] **Step 1: Write the failing tests** in `test_builtin_rcp.c`, one per skeleton scenario of
  `builtin_rcp_job_result` and `builtin_rcp_restart_detail`. The four mode_fsm job-result cases move here with
  `settle` in place of the restart error (UP for ESP_OK; DOWN and WAIT for a failure). Detail tests compare whole
  strings with the texts in the skeleton. Use reason "no rcp firmware" for Thread and "firmware too old" for Zigbee.
  The truncation test uses cap 16 and asserts the length is 15 and the string is terminated.
- [ ] **Step 2: Run them.** `bash <scratchpad>/hosttest.sh builtin_rcp`. Expected: the build fails on
  `builtin_rcp_job_result` undeclared.
- [ ] **Step 3: Implement both pure functions** in `builtin_rcp_core.c`. Who: OTBR is "OpenThread", ZIGGURAT is
  "Zigbee", RCP_PROXY is "rcp-proxy". The tail is the reason, else the state, else "still starting after 180 s" for
  WAIT.
- [ ] **Step 4: Run them.** Expected: `Failures 0`.
- [ ] **Step 5: Implement the entry points** in `builtin_rcp.c`, following the skeleton. Log the refusal or the
  failure as `run_maintenance` did. br_core is not touched yet: both paths exist until Task 4.
- [ ] **Step 6: Run** `make test-host` and `make build`. Expected: pass.
- [ ] **Step 7: Commit** `feat(builtin_rcp): the built-in RCP runs its own push and ROM recovery`.

### Task 3: device_api calls builtin_rcp

**Files:** `device_api.c`, `json_err.c`, `device_api_priv.h`, `test_errors.c`.

**Interfaces:**
- Consumes: Task 2's entry points; `br_core_move_running()`; `radio_mux_otbr_channel()`; `RADIO_MUX_BUILTIN_ID`.
- Produces: `const char *device_api_radio_job_busy(int mode, bool thread_on_c6, bool moving);`

- [ ] **Step 1: Write the failing test.** In `test_errors.c`, add `TEST_CASE("On a move running: the radio job is
  busy with today's move text", "[device_api]")`. It asserts "OpenThread is being moved to another radio" for
  `moving` true with `thread_on_c6` false, and with BR_RUNNING. The existing cases get `moving` false.
- [ ] **Step 2: Run it.** `bash <scratchpad>/hosttest.sh device_api`. Expected: the build fails (too many
  arguments).
- [ ] **Step 3: Implement** the `moving` parameter (checked first). Add a static `thread_on_builtin()` in
  device_api.c for the three pre-checks. Switch `push_task`, `recover_task` and `update_rcp` to the builtin_rcp entry
  points. The `restart_failed` default detail becomes "the image is installed, but the radio's mode did not start on
  it"; Thread's detail still comes from builtin_rcp.
- [ ] **Step 4: Run** `make test-host` and `make build`. Expected: pass.
- [ ] **Step 5: Commit** `refactor(device_api): radio maintenance through builtin_rcp`.

### Task 4: br_core keeps only Thread's own part

**Files:** `br_core.c`, `br_core_radio.c`, `br_core_priv.h`, `include/br_core.h`, `mode_fsm.c`, `mode_fsm.h`,
`test_mode_fsm.c`, docs.

**Interfaces:**
- Produces: `const char *mode_fsm_reinit_refusal(br_core_mode_t mode, bool moving);`
- Removes: `br_core_push_radio_firmware`, `br_core_rom_recover`, `br_core_c6_job_touches_openthread`,
  `run_maintenance`, `MAINT_SETTLE_POLL_MS`, `mode_fsm_radio_job_result` and its four tests (moved in Task 2),
  `core_radio_input_pending`, and `mode_fsm_inbox_pending` with its test (no caller left).

- [ ] **Step 1: Write the failing tests** in `test_mode_fsm.c`, one per `mode_fsm_reinit_refusal` scenario of the
  skeleton, with the exact texts.
- [ ] **Step 2: Run them.** `bash <scratchpad>/hosttest.sh br_core`. Expected: the build fails on
  `mode_fsm_reinit_refusal` undeclared.
- [ ] **Step 3: Implement it.** `reinit()` in br_core_radio.c refuses on a running move before the drain. Both
  refusals reply with the text instead of an empty reason.
- [ ] **Step 4: Remove** the code listed under Removes. Remove the job-result tests only once Task 2's copies pass.
- [ ] **Step 5: Docs.**
  - `docs/api.md`: the push row says `restart_failed` is "the image is installed, but the mode on the C6 did not
    start on it", for any mode.
  - `docs/radios.md`, Maintenance: the reply waits up to 180 s for the C6's mode. No reboot after an abandoned start.
    A C6 push leaves a USB radio's compatibility alone.
  - `docs/zigbee.md`, Radio resets and loss: one line saying a push answers once Zigbee runs again.
  - `test_rcp_push.py`: the class docstring names `builtin_rcp_push_firmware`.
- [ ] **Step 6: Run the full suite.** `bash -c 'make test-host && make test-tools && make build && make build-test'`.
  Expected: 24/24 host projects, every page test passes, both builds complete without warnings.
- [ ] **Step 7: Commit** `refactor(br_core)!: C6 maintenance leaves Thread's code`.

### Task 5: review, then the on-board pass

- [ ] **Step 1: Review.** A fresh reviewer on the most capable model reviews the range from `add883f` to HEAD
  against this plan and its Review Focus. Re-grade the findings, then fix the Critical and Important ones test-first
  in one pass.
- [ ] **Step 2: The batched hardware pass** (the orchestrator plan's Task 12) runs on the result. It adds:
  - test_rcp_push (Thread on the C6);
  - test_zigbee's push with a client, with its reply in under 30 s and 200;
  - a push with the proxy stored on the C6 and no peer, 200;
  - a push within 2 s of the boot steps ending (fix 1);
  - a push during a slow start (fix 2);
  - `GET /device/channels` with the C6 empty at boot (`rcp_probe_override empty`): Thread's `mode_state` is `fault`
    (fix 3);
  - `/device/radio/recover` only if the jumper is fitted on the main board.
- [ ] **Step 3: Ledger and memory.** Record the results, then finish the branch with
  superpowers:finishing-a-development-branch.
