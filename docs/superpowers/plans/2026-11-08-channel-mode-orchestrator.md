# Channel/mode orchestrator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One orchestrator decides which mode runs on which radio channel. Channels have their own lifecycle,
including the `reinit` maintenance state. Modes are instances with their own state machines. The board behaves as
today except for the listed changes.

**Architecture:**
- The orchestrator lives in `radio_mux`: a pure core (`orch_core.c`) and a runtime (`orch.c`) on the mux owner task.
- It owns the binding table and delivers never-lost inputs to the instances: Thread in `br_core`, Zigbee in `zigbee`,
  and one RCP proxy per channel in `rcp_proxy`.
- The board's built-in RCP becomes a seam: a descriptor plus maintenance ops, implemented by a new `builtin_rcp`
  component that takes the C6 maintenance out of `br_core`.

**Tech stack:** ESP-IDF v6.1 (C, FreeRTOS) on the ESP32-P4. Host tests use Unity on the ESP-IDF `linux` target.
Target tests use pytest-embedded. Page tests are pytest + node (`tools/tests`).

**Spec:** `docs/superpowers/specs/2026-11-08-channel-mode-orchestrator-design.md` (approved by the user and reviewed by
the Zigbee session, d36eac5). Section numbers below (§) refer to it.

## Global Constraints

- **Branch** `feat/channel-orchestrator` (from master dd5da30), one PR. Never push or open the PR unless the user asks.
- **Style:** `STYLEGUIDE.md`:
  - snake_case; public symbols carry the component prefix (`radio_mux_`, `builtin_rcp_`, `br_core_`, `rcp_proxy_`,
    `zigbee_`, `device_api_`);
  - `esp_err_t` returns with `ESP_RETURN_ON_ERROR`;
  - one `TAG` per file;
  - host tests are named `TEST_CASE("<scenario>: <effect>", "[<component>]")` in `<component>/test_host/`;
  - target tests live in `p4/pytest/test_<feature>.py`;
  - test-only code sits under `#ifdef OTBR_TEST_BUILD`;
  - ops structs only where a host test needs them.
- **Formatting:** pre-commit on every changed file: `git diff --name-only HEAD | xargs uvx pre-commit run --files`.
  Vendored components are not touched.
- **Commits:** conventional messages ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. One commit
  per step marked "Commit".
- **Command output:** every command runs as `set -o pipefail; <command> 2>&1 | tee "$SCRATCH/<what-ran>.log"`, with
  `$SCRATCH` = the session's scratchpad directory. Re-read the log instead of rerunning the command.
- **One component's host tests:**
  `cd p4/components/<component>/test_host && bash -c '. "$HOME/esp/esp-idf-v6.1/export.sh" >/dev/null && idf.py build && ./build/*.elf'`.
  It passes when the last line reads `<N> Tests 0 Failures 0 Ignored`.
- **Whole tree:**
  - `make test-host` passes with "all <N> host test projects passed";
  - `make test-tools` passes for any web change;
  - `make build` (both images, `-Werror`) passes;
  - `make build-test` (the test image) passes.
- **No hardware inside tasks:** no flashing and no pytest-embedded runs. Target tests are written or updated in their
  task and run only in Task 12 (the standing hardware rule).
- **Memory:** no new permanent FreeRTOS task. The orchestrator runs on the mux owner task. Internal heap free at idle,
  with no dongle, may drop by at most 4 KB against master dd5da30 (§3).
- **API:** the write shape of `PUT/DELETE /device/channels/{id}` is kept: bodies, codes and `detail` texts. The only
  exceptions are §9: the move change, and the built-in channel's id and kind.
- **Storage:** no new NVS record. The binding table is the per-channel stored modes, and the built-in channel keeps the
  NVS key `c6` (§7).
- **Board mode:** the table in §6 is the contract. Its rows are host-tested in Task 5.
- **Error codes:** `RADIO_MUX_ERR_PEER_CONNECTED` is `0x7c01` (unused; `0x7a01`–`0x7a04` and `0x7b01` are taken).
- **Timing:**
  - `RADIO_MUX_REINIT_REPLY_MS` is `50000` (Thread's 45 s drain plus margin);
  - `SESSION_DRAIN_MS` stays `45000`;
  - `BOOT_WAIT_MS` stays `180000`.

## Rulings made while planning (the spec left these to the builder)

- **Where the orchestrator code lives:** in `radio_mux`, as `orch_core.c` and `orch.c`. A new component would need the
  mux's private registry. Symbols use `radio_mux_orch_` (pure) and `radio_mux_` (public).
- **The boot steps' task:** `radio_mux_start` creates a short-lived task, `mux_boot` (MGMT_CORE, 6144 bytes of stack).
  It runs the built-in RCP's `boot_steps` and deletes itself.
  - Why: the steps run even when the network init fails, and they cost no RAM at idle.
  - Today they run on `net_init_task`, inside `br_core_start`.
  - The spec's "no new task" is about the orchestrator itself.
- **Reinit protocol:**
  - The orchestrator sends `RADIO_MUX_IN_REINIT` (seq) and waits for `radio_mux_reinit_reply` with that seq.
  - "Accept" and "delay" are one path: an instance replies when it has let go, at once or after its drain.
  - No reply within `RADIO_MUX_REINIT_REPLY_MS` is a refusal: "<mode> did not let go of the radio in time".
  - The channel enters `reinit` (tx and reset refused) only on an accept.
- **Refusal codes:**
  - The proxy refuses with `RADIO_MUX_ERR_PEER_CONNECTED`; device_api maps it to 409 `c6_busy` with today's text.
  - Thread refuses with `ESP_ERR_INVALID_STATE` and its reason; device_api maps it to 409 `busy` with today's texts.
  - `RCP_SYNC_ERR_C6_BUSY` is removed with the C6 job API.
- **Push and recovery replies:** `radio_mux_maintain` returns when the channel is `ready` or `fault` again. The push
  handler then waits, as today, for Thread to settle when Thread is bound to that channel (`BR_RUNNING` or a fault, up
  to `BOOT_WAIT_MS`). The reply codes stay those of today's `mode_fsm_radio_job_result` and
  `device_api_radio_recover_reply`.
- **The stuck-start test hook:** a test-build console command, `ot_start_abandon <channel id|off>`. It makes the next
  OpenThread start on that channel end as `MODE_FSM_ERR_START_ABANDONED`, without waiting `BOOT_WAIT_MS`.

## Review Focus

These are the failure modes the spec implies but no single scenario test covers. Each line names the task whose tests
pin it.

1. **An input overtaken by a newer one** (a BIND that a task handles after the UNBIND that followed it): the instance
   acts only on the latest seq and ends unbound. Tested in Tasks 3, 4 and 5 ("On an input older than the latest").
2. **The radio lost while it is in `reinit`** (a dongle unplugged during detection, a C6 fault during a push): the
   channel ends `absent`/`fault` and the instance gets LOST, not BACK. `radio_mux_maintain` returns the work's error,
   and the binding is kept. Tested in Task 1 (reinit run) and Task 6b (runtime).
3. **A PUT of a mode on a channel while its maintenance runs**: 409 `busy`, nothing stored, as today for a radio job.
   Tested in Task 2 (`radio_mux_orch_check_bind` gets `in_reinit`).
4. **An instance that never replies to a reinit request** (a stuck drain): the maintenance is refused after
   `RADIO_MUX_REINIT_REPLY_MS` with the reason, the channel stays `ready`, and nothing ran. Tested in Task 1.
5. **A reboot-on-change instance and maintenance** (Zigbee on the C6, then a push): no reboot, Zigbee pauses and comes
   back. Unbinding Zigbee is still `APPLY_REBOOT`. Tested in Task 2 (bind rules) and Task 4.

## BDD skeleton (the contract; tests come from it, one test per effect)

```
radio_mux orchestrator core (orch_core.c, pure)
    radio_mux_orch_check_bind
        On every refusal of today's radio_mux_check_set_mode (moved unchanged)
            - returns the same verdict as before (bad value, unavailable, not available, incompatible, hybrid,
              mode taken, MOVE, APPLY_REBOOT)
        On a change on a channel in reinit
            - returns RADIO_MUX_REFUSE_BUSY
        On leaving or entering a reboot_on_change mode
            - returns RADIO_MUX_APPLY_REBOOT
    radio_mux_orch_should_bind
        On a ready channel whose firmware fits and no instance bound
            - returns true
        On a ready channel whose firmware no longer fits
            - returns false
        On a channel absent, in reinit or in fault
            - returns false
    radio_mux_singleton_dedupe
        On two channels stored otbr (or two ziggurat)
            - keeps the first in slot order and sets the others off
            - marks the changed slots
        On one or none
            - changes nothing
    radio_mux_reinit_ask / _answer / _tick / _done
        On a channel with no instance bound
            - runs at once (RUN)
        On a bound instance that accepts with the current seq
            - runs (RUN)
        On a bound instance that refuses
            - refuses with its verdict, phase back to idle
        On an answer with an older seq
            - is ignored
        On no answer within RADIO_MUX_REINIT_REPLY_MS
            - refuses (timeout), phase back to idle
        On done
            - phase back to idle
    radio_mux_work_reason / radio_mux_busy_reason / radio_mux_chan_state_name
        On each value
            - returns the text of §4.1 and today's busy texts

radio_mux runtime (orch.c, mux.c, mux_api.c, mux_deliver.c)
    radio_mux_register_instance
        On a duplicate mode, after the start, or more than RADIO_MUX_RULES_MAX
            - returns today's register_consumer errors
    the binding loop (owner task)
        On boot
            - every channel starts in reinit; an instance is bound once its channel is ready, never before
        On a ready channel with a fitting stored mode
            - delivers BIND with a new handle
        On an unplug or a link/firmware fault while bound
            - delivers LOST with the reason and kind; the binding is kept
        On the channel's return (detection done, fits)
            - delivers BACK with a new handle
        On the return with firmware that no longer fits
            - delivers nothing; the instance shows no_radio with the fit reason; the binding is kept
        On a PUT of another mode
            - delivers UNBIND to the old instance, stores the mode, binds the new one
    channel in reinit
        On radio_mux_tx from the bound instance
            - returns ESP_ERR_INVALID_STATE; nothing is written
        On radio_mux_reset from the bound instance
            - is ignored
        On the maintenance's own resets
            - posts no RESET to the paused instance
        On radio_mux_release of a slot taken before the reinit
            - returns the slot to the pool
    radio_mux_maintain
        On a bound instance that accepts
            - runs the work in reinit; delivers BACK after; returns the work's result
        On a bound instance that refuses
            - returns its verdict and reason; nothing ran; the channel stays ready
        On another maintenance running, or a move running
            - returns ESP_ERR_INVALID_STATE
    radio_mux_get_snapshot
        On any state
            - carries channel_state, channel_reason, mode_state, mode_reason and the instances list
            - keeps today's mode, state and reason values (§6)

builtin_rcp (maintenance ops)
    boot_steps
        On an installed RCP
            - returns ESP_OK; updates the installed version
        On no RCP firmware, the ESP-Hosted image or a failed ROM recovery
            - returns a firmware fault with today's reason
        On c6_hold (test build)
            - returns ESP_OK and keeps the C6 in reset (Thread's probe then fails with "rcp incompatible")
    push / rom_recover
        On success, failure, or no jumper
            - returns today's errors (ESP_ERR_NOT_SUPPORTED for no jumper)
    builtin_rcp_get_status
        On any state
            - returns installed version, known flag, recovery flags as br_core did

Thread instance (br_core: mode_fsm.c, br_core_radio.c)
    mode_fsm_mode_for
        On each row of §6's table
            - returns that board mode and reason
    mode_fsm_reinit_verdict
        On BR_RUNNING with sessions that drain
            - accepts (after the drain)
        On SNIFFER or STARTING
            - refuses with ESP_ERR_INVALID_STATE and today's text
        On a drain timeout
            - refuses with "OpenThread is busy, try again"
    inputs
        On BIND
            - starts OpenThread on the channel (probe, init, BR start)
        On UNBIND
            - drains, stops, board NO_RADIO "no channel in otbr" (a stop) or hands over to the move
        On REINIT
            - drains, stops, board STARTING, replies accept; refuses as above
        On LOST (link) / LOST (firmware)
            - NO_RADIO / RADIO_FAULT with the reason
        On BACK
            - clears rcp_compatible after a push or recovery and starts again
        On an input older than the latest
            - is ignored
    mode_fsm_move_otbr
        On a probe failure
            - returns MODE_FSM_ERR_PROBE_FAILED; nothing changed
        On a drain timeout
            - returns MODE_FSM_ERR_DRAIN_TIMEOUT; Thread runs on the source
        On a start failure on the target
            - returns MODE_FSM_ERR_START_FAILED; binding on the target; RADIO_FAULT; no start back on the source
        On an abandoned start on the target
            - returns MODE_FSM_ERR_START_ABANDONED; RADIO_FAULT "an earlier OpenThread start was abandoned: reboot";
              no reboot

rcp_proxy instance
    rcp_proxy_reinit_verdict
        On a connected peer
            - refuses with RADIO_MUX_ERR_PEER_CONNECTED and "rcp-proxy on the C6 has a connected peer"
        On no peer
            - accepts after closing the session and the listener
    inputs
        On BIND/BACK, UNBIND/LOST, an older seq
            - opens with the channel's settings, closes, is ignored

zigbee instance
    on_input
        On any input
            - records it with its seq and kicks the task; never blocks
    the task
        On REINIT
            - handles it like GONE (in-flight tx/scans fail, radio cleared, no_radio "radio reinit: <work>"),
              client stays, replies accept
        On BACK
            - runs the attach sequence (CMD_RESET, caps and version check, configuration replay)
        On an input older than the latest
            - is ignored

device_api
    GET /device/channels
        On any state
            - adds channel_state, channel_reason, mode_state, mode_reason per channel and the top-level modes list
            - keeps every existing field and value
    POST /device/radio/firmware, POST /device/radio/recover, the update's RCP step
        On a proxy peer / Thread busy / no jumper / success
            - 409 c6_busy / 409 busy / 412 no_jumper / today's success replies
    the built-in channel
        On any read or write
            - id builtin-c6, kind builtin; "c6" answers 404 unknown_channel
```

---

### Task 1: The orchestrator's pure core

**Files:**
- Create: `p4/components/radio_mux/include/radio_mux_orch_core.h`, `p4/components/radio_mux/orch_core.c`,
  `p4/components/radio_mux/test_host/main/test_orch.c`
- Modify:
  - `p4/components/radio_mux/core.c`, `include/radio_mux_core.h`: move `radio_mux_check_set_mode` and
    `radio_mux_otbr_dedupe` out;
  - `p4/components/radio_mux/CMakeLists.txt`: add `orch_core.c` to both SRCS lists;
  - `p4/components/radio_mux/test_host/main/CMakeLists.txt`;
  - `p4/components/radio_mux/mux.c`, `mux_nvs.c`: callers of the moved functions;
  - `test_core.c`, `test_registry.c`: move their check_set_mode and dedupe cases into `test_orch.c`.

**Interfaces (produces):**
- `typedef enum { RADIO_MUX_CS_ABSENT, RADIO_MUX_CS_REINIT, RADIO_MUX_CS_READY, RADIO_MUX_CS_FAULT } radio_mux_chan_state_t;`
- `const char *radio_mux_chan_state_name(radio_mux_chan_state_t s)`: returns "absent", "reinit", "ready" or "fault".
- `typedef enum { RADIO_MUX_FAULT_NONE, RADIO_MUX_FAULT_FIRMWARE, RADIO_MUX_FAULT_LINK } radio_mux_fault_kind_t;`
- `typedef enum { RADIO_MUX_WORK_NONE, RADIO_MUX_WORK_BOOT_STEPS, RADIO_MUX_WORK_DETECT, RADIO_MUX_WORK_PUSH, RADIO_MUX_WORK_ROM_RECOVERY } radio_mux_work_t;`
- `const char *radio_mux_work_reason(radio_mux_work_t w)`: returns "boot steps", "detecting firmware", "firmware push",
  "ROM recovery", or "" for NONE.
- `const char *radio_mux_busy_reason(radio_mux_work_t w)`: returns "the C6's boot steps run" for BOOT_STEPS, "a C6 job
  holds the radio" for PUSH and ROM_RECOVERY, and "" otherwise. These are today's texts.
- `radio_mux_verdict_t radio_mux_orch_check_bind(const radio_mux_rules_t *rules, const radio_mux_chan_view_t *chans, size_t n, size_t target, radio_mux_mode_t want, bool in_reinit, size_t *move_from)`:
  today's `radio_mux_check_set_mode`, plus `in_reinit` → `RADIO_MUX_REFUSE_BUSY` (checked after the bad-value check).
- `bool radio_mux_orch_should_bind(radio_mux_chan_state_t cs, bool fits, bool bound)`.
- `size_t radio_mux_singleton_dedupe(radio_mux_mode_t *modes, bool *changed, size_t n)`: `radio_mux_otbr_dedupe`,
  extended to `ziggurat`.
- `#define RADIO_MUX_REINIT_REPLY_MS 50000`
- `typedef enum { RADIO_MUX_RP_IDLE, RADIO_MUX_RP_ASKED, RADIO_MUX_RP_RUNNING } radio_mux_reinit_phase_t;`
- `typedef enum { RADIO_MUX_RA_WAIT, RADIO_MUX_RA_RUN, RADIO_MUX_RA_REFUSE, RADIO_MUX_RA_IGNORE } radio_mux_reinit_act_t;`
- `typedef struct { radio_mux_reinit_phase_t phase; uint32_t seq; int64_t deadline_ms; } radio_mux_reinit_run_t;`
- `radio_mux_reinit_act_t radio_mux_reinit_ask(radio_mux_reinit_run_t *r, uint32_t seq, bool bound, int64_t now_ms)`
- `radio_mux_reinit_act_t radio_mux_reinit_answer(radio_mux_reinit_run_t *r, uint32_t seq, bool accept)`
- `radio_mux_reinit_act_t radio_mux_reinit_tick(radio_mux_reinit_run_t *r, int64_t now_ms)`
- `void radio_mux_reinit_done(radio_mux_reinit_run_t *r)`

- [ ] **Step 1:** Write the failing host tests in `test_orch.c`: one `TEST_CASE` per effect of the BDD subtree
  "radio_mux orchestrator core". Move the existing check_set_mode and dedupe cases from `test_core.c` and
  `test_registry.c` unchanged in their assertions, calling the new names.
- [ ] **Step 2:** Run radio_mux's host tests. They are expected to fail to build: the `radio_mux_orch_*` and
  `radio_mux_reinit_*` symbols are undefined.
- [ ] **Step 3:** Implement the interfaces above in `orch_core.c`, and update the callers in `mux.c` (`check_mode`
  passes `in_reinit` = the C6 hold or a job holds the channel) and `mux_nvs.c` (load dedupe).
- [ ] **Step 4:** Run radio_mux's host tests. Expected: all pass, and the number of moved cases equals the number
  removed.
- [ ] **Step 5:** `make build` passes. Commit:
  "refactor(radio_mux): the orchestrator's pure core (binding rules, reinit run, dedupe)".

### Task 2: The instance contract and the orchestrator runtime, consumers behind a shim

**Files:**
- Create: `p4/components/radio_mux/orch.c`
- Modify:
  - `include/radio_mux.h`: the instance section; the consumer section is kept and marked transitional;
  - `radio_mux_priv.h`: the binding table and per-instance seq;
  - `mux.c`: `want_attach`, `do_attach`, `do_detach` and `c6_apply` decide nothing any more; they call `orch.c`;
  - `mux_api.c`: `radio_mux_register_instance`, `radio_mux_report`, and the snapshot fields;
  - `CMakeLists.txt`.

**Interfaces (produces):**
- `typedef enum { RADIO_MUX_IN_NONE, RADIO_MUX_IN_BIND, RADIO_MUX_IN_UNBIND, RADIO_MUX_IN_REINIT, RADIO_MUX_IN_LOST, RADIO_MUX_IN_BACK } radio_mux_input_type_t;`
  (`NONE` is never delivered; it is the "nothing to deliver" answer of `radio_mux_orch_input_for`).
- `typedef struct { radio_mux_input_type_t type; uint32_t seq; radio_mux_chan_t *ch; radio_mux_fault_kind_t kind; char reason[RADIO_MUX_REASON_MAX]; } radio_mux_input_t;`
  - `ch` is the new handle for BIND and BACK, and the held handle otherwise.
  - `seq` grows by 1 per input per instance (per channel for `per_channel` instances).
- `typedef struct { radio_mux_mode_t mode; radio_mux_fw_t needs; uint8_t limit; bool reboot_on_change; bool frames; bool per_channel; void (*input)(const radio_mux_input_t *in, void *ctx); void *ctx; } radio_mux_instance_t;`
  - `input` runs on the owner task. It copies `*in` as the latest input and wakes the instance's task. It never
    blocks and never calls the mux.
- `esp_err_t radio_mux_register_instance(const radio_mux_instance_t *inst)`: the same errors as
  `radio_mux_register_consumer`.
- `void radio_mux_report(radio_mux_chan_t *ch, const char *state, const char *reason)`: the instance's `mode_state`
  and `mode_reason`. It is ignored unless `ch` is the current handle, and it never blocks (it copies under
  `mux_lock`).
- Snapshot (`radio_mux_chan_info_t`) gains:
  - `radio_mux_chan_state_t chan_state`;
  - `char chan_reason[RADIO_MUX_REASON_MAX]`;
  - `char mode_state[16]`;
  - `char mode_reason[RADIO_MUX_REASON_MAX]`.
- `radio_mux_snapshot_t` gains `struct { radio_mux_mode_t mode; char channel[RADIO_MUX_ID_MAX]; char state[16]; char reason[RADIO_MUX_REASON_MAX]; bool reboot_on_change; } modes[RADIO_MUX_RULES_MAX + RADIO_MUX_CHANNELS_MAX]; size_t n_modes;`
- `#define RADIO_MUX_ERR_PEER_CONNECTED 0x7c01`
- `esp_err_t radio_mux_reinit_reply(radio_mux_chan_t *ch, uint32_t seq, esp_err_t verdict, const char *reason)`:
  - ESP_OK accepts; anything else refuses with that code;
  - ignored for a stale handle or seq; never blocks;
  - declared and recorded here so that Tasks 3–5 can call it; nothing asks for a reinit until Task 6b.
- **The shim:** a consumer registered the old way is wrapped as an instance. BIND and BACK call its `attach` with the
  handle. UNBIND and LOST post GONE and call its `detach`, exactly as today's attach machine does.

- [ ] **Step 1:** Write failing host tests for the binding loop's pure decisions, in `test_orch.c`. They cover "On a
  ready channel ...", "On the return with firmware that no longer fits ..." and "On a PUT of another mode ...", via
  `radio_mux_orch_should_bind` and a pure `radio_mux_input_t radio_mux_orch_input_for(radio_mux_chan_state_t before, radio_mux_chan_state_t after, bool fits, bool bound)`.
  That function returns the input to deliver, with `type` `RADIO_MUX_IN_NONE` when there is none.
- [ ] **Step 2:** Run radio_mux's host tests: they fail on `radio_mux_orch_input_for`.
- [ ] **Step 3:** Implement `radio_mux_orch_input_for`, then `orch.c`:
  - the binding table (loaded from the per-channel stored modes at start, after `radio_mux_singleton_dedupe`);
  - the per-instance seq;
  - delivery of BIND, UNBIND, LOST and BACK at the points where the attach machine attached and detached today;
  - the shim;
  - `radio_mux_register_instance` and `radio_mux_report`;
  - the snapshot fields. `chan_state` follows the attach state: ABSENT → absent; PROBING and REOPENING → reinit
    (DETECT); C6 hold or job → reinit; ATTACHED and IDLE → ready unless `p->fault` → fault.
  - The fault kind is firmware for the C6's listing faults (today's `radio_mux_c6_present` reasons) and link for every
    USB fault (`USB close failed`, the fruitless-reset give-up, no driver).
  - `mode_state` comes from `radio_mux_report`. When an instance is not bound because its channel's firmware no longer
    fits, the orchestrator itself reports `no_radio` with today's fit reason for it.
- [ ] **Step 4:** Run radio_mux's host tests (they pass), `make test-host` and `make build`. Commit:
  "feat(radio_mux): instances bound by the orchestrator; consumers behind a shim".

### Task 3: The RCP proxy as an instance per channel

**Files:**
- Modify: `p4/components/rcp_proxy/proxy.c` (registration, `on_attach`/`on_detach` become `on_input`, `reconcile` reads
  the latest input), `rcp_proxy/core.c` and `include/rcp_proxy_core.h` (pure verdict),
  `rcp_proxy/test_host/main/test_core.c`.

**Interfaces:**
- Consumes: `radio_mux_instance_t`, `radio_mux_input_t`, `radio_mux_reinit_reply` and `RADIO_MUX_ERR_PEER_CONNECTED`
  (all from Task 2). Until Task 6b nothing sends REINIT, so that branch is compiled but not reached.
- Produces:
  - `typedef enum { RCP_PROXY_REINIT_ACCEPT, RCP_PROXY_REINIT_REFUSE_PEER } rcp_proxy_reinit_t;`
  - `rcp_proxy_reinit_t rcp_proxy_reinit_verdict(bool peer_connected)`;
  - `bool rcp_proxy_input_current(uint32_t seen_seq, uint32_t input_seq)`: true when the input is newer than the last
    one handled.

- [ ] **Step 1:** Write failing host tests for the "rcp_proxy instance" subtree: `rcp_proxy_reinit_verdict` (both
  scenarios) and `rcp_proxy_input_current` ("On an input older than the latest: is ignored").
- [ ] **Step 2:** Run rcp_proxy's host tests: they fail.
- [ ] **Step 3:** Implement the two functions, then the proxy's instance:
  - register with `per_channel = true`;
  - `on_input` writes the latest input and its seq into today's `s_want[slot]` (it already holds the latest wish,
    under `s_lock`) and pokes;
  - `reconcile` acts on the newest seq;
  - REINIT: the verdict; on accept, close the session and the listener, then reply;
  - `radio_mux_report` on each state change (`listening`, `connecting`, `backoff`, `connected`, `no_radio`, `off`).
  - Keep the `radio_mux_set_busy_peer` calls until Task 6b.
- [ ] **Step 4:** rcp_proxy's host tests pass and `make build` passes. Commit: "refactor(rcp_proxy): an orchestrator
  instance per channel".

### Task 4: Zigbee as an instance; inputs never lost, never blocking

**Files:**
- Modify: `p4/components/zigbee/zigbee.c` (`on_attach`/`on_detach` → `on_input`; `ATTACH_POST_MS` and its blocking
  `xQueueSend` removed; `gone_backstop` generalized), `zigbee_priv.h`, `zigbee/status.c` (while idle or no_radio it
  copies the snapshot's `chan_reason`, so the C6's boot steps and a reinit show why), `zigbee/test_host` (a new test file
  `test_input.c` if the pure helper lives in a new `input.c`).

**Interfaces:**
- Produces: `bool zigbee_input_newer(uint32_t seen_seq, uint32_t input_seq)` (pure, host-tested). The instance
  registers with `reboot_on_change = true` and `needs = RADIO_MUX_FW_SPINEL`, as today.

- [ ] **Step 1:** Write failing host tests: `zigbee_input_newer` ("On an input older than the latest: is ignored", and
  the newer and equal cases).
- [ ] **Step 2:** Run zigbee's host tests: they fail.
- [ ] **Step 3:** Implement:
  - `on_input` stores the latest input and seq atomically, then calls `zigbee_kick()` (today's backstop path). It
    never posts to the event queue.
  - The task's kick handler compares seqs:
    - BIND and BACK → today's ZIGBEE_EV_ATTACH path;
    - UNBIND and LOST → `handle_gone()`;
    - REINIT → `handle_gone()` with the reason "radio reinit: <work>", then `radio_mux_reinit_reply(ch, seq, ESP_OK, NULL)`.
  - `zigbee_status_set_state` also calls `radio_mux_report`.
- [ ] **Step 4:** zigbee's host tests pass, and `make build` and `make test-host` pass. Commit:
  "refactor(zigbee): an orchestrator instance; inputs never lost or blocking".

### Task 5: Thread as an instance; the board-mode table

**Files:**
- Modify:
  - `p4/components/br_core/mode_fsm.h`, `mode_fsm.c`: pure table and verdict;
  - `br_core_radio.c`: `reconcile`, `s_start_failed`, `s_lost`, `s_retry`, `s_tried_gen`, `s_attach_gen` and
    `otbr_attach`/`otbr_detach` are replaced by input handling on br_radio. `s_booting` and `s_c6_boot_steps` stay
    until Task 6b.
  - `br_core.c`: callers;
  - `br_core/test_host/main/test_mode_fsm.c`, and `br_core/test_host/main/CMakeLists.txt` (REQUIRES `radio_mux`,
    whose pure core builds on linux, for the `radio_mux_chan_state_t`, `radio_mux_fault_kind_t` and
    `radio_mux_work_t` types);
  - `radio_mux`: the shim is removed once Thread, the proxy and Zigbee are instances. That means
    `radio_mux_consumer_t` and `radio_mux_register_consumer` are deleted and the "transitional" section goes.

**Interfaces (produces):**
- `typedef struct { bool bound; radio_mux_chan_state_t chan_state; radio_mux_fault_kind_t kind; radio_mux_work_t work; bool is_usb; const char *chan_id; const char *chan_reason; br_core_mode_t thread; const char *thread_reason; bool unplugged; } mode_fsm_view_t;`
  - `thread` is Thread's own state: STARTING, BR_RUNNING, SNIFFER or RADIO_FAULT while bound and started, and
    NO_RADIO otherwise.
- `br_core_mode_t mode_fsm_mode_for(const mode_fsm_view_t *v, char *reason, size_t cap)`: one row of §6 per case.
- `esp_err_t mode_fsm_reinit_verdict(br_core_mode_t mode)`: ESP_OK, or ESP_ERR_INVALID_STATE in SNIFFER and STARTING.
  The drain-timeout refusal comes from the drain itself.
- `bool mode_fsm_input_newer(uint32_t seen_seq, uint32_t input_seq)`.

- [ ] **Step 1:** Write failing host tests in `test_mode_fsm.c`:
  - one `TEST_CASE` per row of §6's table (`mode_fsm_mode_for`), with the exact reasons: "waiting for <id>", "radio
    unplugged (<id>)", "no channel in otbr", the channel's reason, "rcp incompatible", and "an earlier OpenThread start
    was abandoned: reboot";
  - `mode_fsm_reinit_verdict` (three scenarios);
  - `mode_fsm_input_newer`.
  Today's mode_fsm cases stay and must pass unchanged.
- [ ] **Step 2:** Run br_core's host tests: the new cases fail.
- [ ] **Step 3:** Implement the three functions, then Thread's instance in `br_core_radio.c`:
  - `on_input` stores the latest input and seq and notifies br_radio.
  - br_radio handles them:
    - BIND/BACK: today's start branch of `reconcile`;
    - UNBIND: today's stop (drain, `core_radio_ot_stop`);
    - LOST: today's `mode_fsm_on_channel_gone` path, with `mode_fsm_mode_for` setting the mode and reason;
    - REINIT: drain, stop, mode STARTING, reply. The reply is ESP_OK, or ESP_ERR_INVALID_STATE with "OpenThread is
      busy, try again" on a drain timeout. A verdict refusal is replied at once with today's text.
    - an older seq is ignored.
  - "A PUT of mode otbr retries a fault" stays: `br_core_retry_otbr` re-runs the last BIND.
  - `radio_mux_report` on each mode change: the state names of §4.2.
  Then remove the shim from radio_mux.
- [ ] **Step 4:** br_core's and radio_mux's host tests pass, and `make test-host` and `make build` pass. Commit:
  "refactor(br_core): Thread as an orchestrator instance; the board-mode table".

### Task 6a: The built-in RCP seam and the `builtin_rcp` component (code moved, no behavior change)

**Files:**
- Create:
  - `p4/components/builtin_rcp/CMakeLists.txt`;
  - `builtin_rcp/include/builtin_rcp.h`;
  - `builtin_rcp/builtin_rcp.c`: the chip-independent part. That is the registration with `radio_mux`, the status
    and the descriptor, built from the board's chip;
  - `builtin_rcp/c6.c`: this board's implementation, the C6 on the ESP32-P4 board. It covers the probe, the push, the
    ROM recovery, the ESP-Hosted and jumper checks, and the test overrides. A board with another built-in RCP (an
    ESP32-H2) would add its own file next to it and nothing else;
  - `builtin_rcp/builtin_rcp_core.c` and `builtin_rcp/include/builtin_rcp_core.h` (pure decisions);
  - `builtin_rcp/test_host/` (CMakeLists, main/CMakeLists, `main/test_builtin_rcp.c`).
- Modify:
  - `include/radio_mux.h` (the built-in section);
  - `mux.c` (the C6 listing reads the built-in descriptor);
  - `br_core.c`: `run_probe`, `core_c6_boot_steps`, `run_c6_job`, the `g_core.rcp_installed*` and recovery fields,
    and the probe override move out;
  - `br_core_test.c`;
  - `device_api/status_json.c` (reads `builtin_rcp_get_status`);
  - `p4/main/test_cmds.c` (`c6_hold`, `rcp_probe_override`, `c6_jumper_ignore` call builtin_rcp);
  - `p4/main/app_main.c` (`builtin_rcp_register()` before `radio_mux_start`);
  - `STYLEGUIDE.md` (add `builtin_rcp_` to the prefix list).
- Move:
  - `mode_fsm_rom_failure_is_fault`, `mode_fsm_next_recovery_failed` and `mode_fsm_installed_after_push` →
    `builtin_rcp_core.c` as `builtin_rcp_rom_failure_is_fault`, `builtin_rcp_next_recovery_failed` and
    `builtin_rcp_installed_after_push`;
  - `radio_mux_c6_present` → `builtin_rcp_present`, which also returns the fault kind (always firmware).
  Their host tests move with them, assertions unchanged.

**Interfaces (produces):**
- In `radio_mux.h`:
  - `typedef struct { const char *id; const char *label; const char *chip; esp_err_t (*boot_steps)(radio_mux_fault_kind_t *kind, char *reason, size_t cap); esp_err_t (*push)(const void *source, char *detail, size_t cap); esp_err_t (*rom_recover)(char *detail, size_t cap); } radio_mux_builtin_t;`
  - `esp_err_t radio_mux_set_builtin(const radio_mux_builtin_t *b)`: before `radio_mux_start`; ESP_ERR_INVALID_STATE
    after it.
  - The ops run on the caller's task while the channel is in `reinit`.
- In `builtin_rcp.h`:
  - `esp_err_t builtin_rcp_register(void)`;
  - `typedef struct { bool installed_known; char installed[32]; bool rom_recovery_failed; bool jumper; } builtin_rcp_status_t;`
    (the fields `/device/status` reads today from `g_core`);
  - `void builtin_rcp_get_status(builtin_rcp_status_t *out)`.
- The id stays `"c6"` in this task; Task 9 renames it to `builtin-c6` (the descriptor builds it as `builtin-<chip>`).

- [ ] **Step 1:** Create the `builtin_rcp` test_host with the moved host tests and their assertions unchanged. Add a
  failing case for `builtin_rcp_present`'s kind: "On no RCP firmware, the ESP-Hosted image or a failed ROM recovery:
  returns a firmware fault with today's reason".
- [ ] **Step 2:** Run builtin_rcp's host tests: they fail to build.
- [ ] **Step 3:** Move the code and register the ops. br_core still drives the C6 job (`c6_job_window`), but calls the
  ops through `radio_mux`'s built-in descriptor instead of its own copies. The behavior is the same.
- [ ] **Step 4:** builtin_rcp's, br_core's and radio_mux's host tests pass, and `make test-host`, `make build` and
  `make build-test` pass. Commit: "refactor(builtin_rcp): the C6's maintenance behind the built-in RCP seam".

### Task 6b: Maintenance is `reinit`

**Files:**
- Modify:
  - `radio_mux`: `orch.c` (the maintenance runner and the boot task); `mux.c` (remove `radio_mux_c6_hold_t` and its
    use, `handle_c6_job_begin`/`_end`, `c6_hold_check`; tx/reset refusal in reinit; no RESET delivery in reinit; USB
    detection is reinit DETECT); `mux_api.c`; `mux_deliver.c`; `core.c` and `radio_mux_core.h` (remove
    `radio_mux_c6_hold_*` and `radio_mux_c6_job_refused`, and their tests in `test_spi.c`); `include/radio_mux.h`
    (remove the C6 job API and `radio_mux_set_busy_peer`; add the maintenance API).
  - `br_core`: remove `run_radio_job`, `op_radio_job`, `c6_job_window`, `run_c6_job_alone`, `push_busy`, `s_booting`,
    `s_c6_boot_steps`, `core_radio_boot`'s C6 job, `br_core_push_radio_firmware`, `br_core_rom_recover`,
    `br_core_radio_job_running`, and `mode_fsm_push_firmware`/`mode_fsm_rom_recover` with their ops. Their host tests
    go too, and the cases that still apply move to the Thread REINIT cases of Task 5.
  - `rcp_proxy/proxy.c`: remove `radio_mux_set_busy_peer`; the REINIT branch becomes live.
  - `rcp_sync/include/rcp_sync.h`: remove `RCP_SYNC_ERR_C6_BUSY` and its docs.
  - `device_api/device_api.c`: the push, recover and update-RCP-step handlers call `radio_mux_maintain`.
  - `device_api/json_err.c`: `RADIO_MUX_ERR_PEER_CONNECTED` → C6_BUSY.
  - `device_api/device_api_priv.h`.
  - `netnames/netnames.c`: `br_core_radio_job_running` → `radio_mux_maintenance_running`.

**Interfaces (produces):**
- `radio_mux_reinit_reply` (declared in Task 2) now feeds `radio_mux_reinit_answer` for the running maintenance.
- `esp_err_t radio_mux_maintain(const char *id, radio_mux_work_t work, const void *source, char *detail, size_t cap)`:
  - PUSH: `source` is a `const rcp_fw_source_t *`; ROM_RECOVERY: NULL.
  - Returns ESP_ERR_INVALID_STATE when another maintenance or a move runs, or with the instance's reason on a timeout.
  - Returns the instance's refusal code (`RADIO_MUX_ERR_PEER_CONNECTED`, ESP_ERR_INVALID_STATE) with its reason in
    `detail`.
  - Otherwise returns the op's result. It returns once the channel is ready or in fault again.
  - Not from the owner task.
- `bool radio_mux_maintenance_running(void)`

**Algorithm (the maintenance runner, on the caller's task with the owner task's help):**

```
maintain(id, work):
  post ASK(id, work) to owner; owner: refuse if a maintenance/move runs; seq = next input seq of the bound instance;
     act = reinit_ask(run, seq, bound, now); if bound: deliver REINIT(seq, reason = work_reason(work))
  wait on the owner's answer: RUN | REFUSE(code, reason)      (owner ticks reinit_tick each loop; answer() on reply)
  on REFUSE: return code, reason
  owner (on RUN): channel -> reinit(work); close the transport, release the pins (as handle_c6_job_begin did)
  caller: run builtin op (push | rom_recover), then boot_steps (as c6_job_window did)
  post DONE(result, steps verdict) to owner: channel -> ready | fault(kind, reason); reinit_done(run);
     if bound and ready and fits: deliver BACK (new handle) else LOST(kind, reason)
  return op result
```

The boot task does the same with `work = BOOT_STEPS` and no op (only `boot_steps`). Every channel is in reinit until
then, so nothing is bound first.

- [ ] **Step 1:** Write failing host tests:
  - in `test_orch.c`, the runner's pure steps not yet covered: "On a bound instance that refuses: returns its verdict
    and reason; nothing ran; the channel stays ready", and Review Focus 2 ("the radio lost while it is in reinit: LOST,
    not BACK"), via `radio_mux_orch_input_for`;
  - in `test_mode_fsm.c`, Thread's REINIT cases;
  - in rcp_proxy, the REINIT path is already covered by Task 3.
- [ ] **Step 2:** Run the affected host tests: they fail.
- [ ] **Step 3:** Implement the runner, the boot task (`mux_boot`, MGMT_CORE, stack 6144, deleted when done), and the
  removals and call sites listed above. Then:
  - the channel's tx/reset refusal and no-RESET rule while in reinit, in `mux_api.c` and `mux_deliver.c` (the B ruling
    of §4.1);
  - `release` keeps working for slots of older handles (today's rule; check it is not lost).
- [ ] **Step 4:** Update the target tests:
  - `p4/pytest/test_channels.py`: `test_put_validation_c6_busy` still expects 409 `c6_busy`;
  - `p4/pytest/test_zigbee.py`: a new `test_push_with_a_client_pauses_and_resumes`, built from
    `radio_held_comes_back`. The client stays connected; `/device/zigbee` shows `no_radio` "radio reinit: firmware
    push", then `running`; the device still toggles.
  - `p4/pytest/test_rcp_push.py`: replies unchanged.
  Do not run them.
- [ ] **Step 5:** `make test-host`, `make build` and `make build-test` pass. Commit:
  "feat(radio_mux): maintenance is the reinit channel state; the C6 jobs go".

### Task 7: Moves are rebinds (no rollback, no reboot on an abandoned start)

**Files:**
- Modify:
  - `p4/components/br_core/mode_fsm.c`, `mode_fsm.h`: `start_back` is removed, and the "start back" ops with it;
  - `br_core_radio.c`: `run_move` no longer calls `core_schedule_reboot`; the commit comes before the start;
  - `radio_mux` `orch.c`: the commit is the binding change; `radio_mux_commit_otbr`'s order is changed or it is
    folded into the orchestrator;
  - `br_core_test.c` and `p4/main/test_cmds.c`: the `ot_start_abandon <id|off>` test command;
  - `test_mode_fsm.c`;
  - `p4/pytest/test_dongle.py`: the move tests that expect a rollback expect the fault on the target; a new
    `test_move_abandoned_start_no_reboot` uses `ot_start_abandon`.

**Interfaces:**
- Produces: `void br_core_test_set_start_abandon(const char *id)` (OTBR_TEST_BUILD only; NULL clears it).
- `mode_fsm_move_otbr` keeps its signature and return codes. Its effects follow the BDD subtree "mode_fsm_move_otbr".

- [ ] **Step 1:** Write failing host tests in `test_mode_fsm.c` for the four move scenarios. The start-failure and
  abandoned cases check that no start on the source is attempted (the ops recorder sees no `start_on(from)`) and that
  no reboot is asked.
- [ ] **Step 2:** Run br_core's host tests: they fail.
- [ ] **Step 3:** Implement:
  - the move order: probe (lease) → 202 → unbind and drain the source → commit (`mux_store_chans`, one NVS commit) →
    bind the target;
  - the fault on the target;
  - an abandoned start → RADIO_FAULT with today's text and no reboot;
  - the test hook.
- [ ] **Step 4:** Update the target tests named above (do not run them). br_core's host tests pass, and
  `make test-host`, `make build` and `make build-test` pass. Commit:
  "feat(br_core): a move is a rebind: no rollback, no reboot on an abandoned start".

### Task 8: The honest read API

**Files:**
- Modify:
  - `p4/components/device_api/channels_json.c` and `channels_json.h`;
  - `device_api/test_host/main/test_channels_json.c`;
  - `p4/pytest/test_channels.py`: new field assertions in `test_c6_listed_in_otbr` and `test_c6_off_and_back`.

**Interfaces:**
- Consumes the snapshot fields of Task 2.
- Produces this JSON per channel:
  - `"channel_state"`: "absent", "reinit", "ready" or "fault";
  - `"channel_reason"`: string or null;
  - `"mode_state"`: the instance's state, or null for `off`;
  - `"mode_reason"`: string or null.
- At the top level, `"modes"` is a list of
  `{"mode", "channel" (id or null), "state", "reason" (or null), "reboot_on_change"}`. A proxy instance appears once per
  channel bound to rcp-proxy.

- [ ] **Step 1:** Write failing host tests in `test_channels_json.c`:
  - the four fields for a ready running channel, an absent dongle, the C6 in reinit (boot steps) and a channel in
    firmware fault;
  - `modes` with Thread unbound, `channel` null, and with two proxy instances;
  - every existing field unchanged in the same fixtures.
- [ ] **Step 2:** Run device_api's host tests: they fail.
- [ ] **Step 3:** Implement the rendering.
- [ ] **Step 4:** Update `docs/api.md` (the example and the field list, §6 wording). device_api's host tests pass, and
  `make test-host` and `make build` pass. Commit: "feat(device_api): channel_state, mode_state and the modes list".

### Task 9: The built-in channel is `builtin-c6`, kind `builtin`

**Files:**
- Modify:
  - `radio_mux_core.h`: `RADIO_MUX_C6_ID` becomes `RADIO_MUX_BUILTIN_ID`, value `"builtin-c6"`, and the kind enum
    becomes `RADIO_MUX_KIND_BUILTIN`;
  - `core.c` `radio_mux_nvs_key`: the built-in id maps to the key `"c6"`;
  - every user found by `git grep -nE "['\"]c6['\"]|RADIO_MUX_C6_ID|RADIO_MUX_KIND_C6"`: br_core, rcp_proxy
    (`slot_of`), device_api (the kind name `"builtin"`), the host tests, `firmware.js`, `status.js`, `zigbee.js`
    (`radioName` shows the channel's label), `test_channels.py`, `test_dongle.py`, `test_zigbee.py` (its C6 radio
    parameters and the "rcp_kill holds the C6 only" skip), the page tests (`test_zigbee_page.py`
    `test_radio_ids_read_as_names` among them), `docs/api.md`, `docs/radios.md` and `docs/zigbee.md`.
  - `builtin_rcp`'s descriptor gets `id "builtin-c6"`, `chip "esp32c6"` and the label "ESP32-C6 (built-in)".

**Interfaces:**
- `/device/channels` lists `{"id": "builtin-c6", "kind": "builtin", ...}`.
- `PUT /device/channels/c6` answers 404 `unknown_channel` (no alias).

- [ ] **Step 1:** Write failing host tests:
  - `radio_mux_nvs_key("builtin-c6")` is `"c6"`;
  - device_api renders kind "builtin";
  - `PUT` parsing of the id "c6" is unknown.
  Update the page tests to the new id and kind.
- [ ] **Step 2:** Run radio_mux's and device_api's host tests and `make test-tools`: they fail.
- [ ] **Step 3:** Rename everywhere the grep lists; nothing else changes.
- [ ] **Step 4:** `make test-host`, `make test-tools`, `make build` and `make build-test` pass. A
  `git grep -nE "['\"]c6['\"]"` shows only the NVS key and the docs' history notes. Commit:
  "feat!: the built-in RCP channel is builtin-c6, kind builtin".

### Task 10: Docs and Radios page texts

**Files:**
- Modify:
  - `docs/radios.md`: "Moving Thread" steps 4–6 (commit before start, no rollback, no reboot on an abandoned start; a
    PUT of otbr on another channel plus a reboot as the way out); a new "Maintenance (reinit)" section; channel kinds;
  - `docs/api.md`: §9 items 1–3;
  - `docs/zigbee.md`: no_radio "radio reinit: <work>";
  - the Radios page texts that promise the rollback (`radios.js`, found by `git grep -n "rollback\|back on the old\|old radio" -- p4/components/esp_ot_br_server/frontend`)
    and their page tests;
  - `docs/superpowers/specs/2026-11-06-radio-channel-mux-design.md`: one line at the top pointing to the orchestrator
    design for the replaced sections.

- [ ] **Step 1:** Update the page tests that check the rollback texts to the new texts, and run `make test-tools`:
  they fail.
- [ ] **Step 2:** Change the page texts and the docs.
- [ ] **Step 3:** `make test-tools` and `make build` pass. Commit: "docs: moves without rollback, maintenance as reinit,
  the built-in channel".

### Task 11: Whole-branch review and fixes

- [ ] **Step 1:** The executing skill's final review: one reviewer on the most capable model over
  `git diff dd5da30...HEAD`, against the spec and this plan's BDD skeleton.
- [ ] **Step 2:** Fix what it finds, with host tests where testable. `make test-host`, `make test-tools`, `make build`
  and `make build-test` pass. Commit per fix.

### Task 12: The batched hardware pass (after Task 11; the controller runs the board)

Flash budget:
- two P4 app flashes: master dd5da30 for the baseline, then the branch's test build;
- C6 flash writes only for `test_rcp_push` and `test_radio_firmware_e2e`.

Before every on-board action, re-read `/device/status` `app.version` and run `fuser` on the console port. Use the board
network in single mode, switching from split only with the user's OK and restoring it after (the rig notes in memory).
Never touch the aux C6.

- [ ] **Step 1: Baseline.**
  - Build master dd5da30 (`git worktree add` a scratch checkout, `make build-test`) and OTA it.
  - Record the idle internal heap with no dongle (`/device/status` diag or the `heap` console command).
  - Set the C6 to rcp-proxy and back, so master writes the NVS records the branch must read.
- [ ] **Step 2: The branch.**
  - OTA the branch's test build.
  - Check that the stored modes and proxy settings came back unchanged (NVS compatibility) and that `builtin-c6` is
    listed with kind `builtin`.
  - Heap within 4 KB of the baseline.
- [ ] **Step 3: Regression suites (§3).**
  - Run, in one session: `test_channels`, `test_dongle` (without the unplug cases unless the user plugs),
    `test_rcp_push`, `test_device_api`, `test_br_boot`, `test_jumper`, `test_rest_guard`, `test_sniffer`,
    `test_radio_firmware_e2e` and `test_zigbee`.
  - The otbr-agent check runs with `RADIO_MUX_AGENT_TEST=1 RADIO_MUX_AGENT_NET=otbr-p0 RADIO_MUX_AGENT_BOARD=<the board's ULA>`.
  - Each run is teed to `$SCRATCH`.
- [ ] **Step 4: Targeted checks (§10).**
  - Boot with each mode on `builtin-c6`: the log shows one bind after "reinit: boot steps", and no other resets.
  - A push with Zigbee on the C6 under a connected client (the new `test_zigbee` case).
  - A push refused with a proxy peer (409 `c6_busy`).
  - A failed move: fault on the target, no rollback.
  - `ot_start_abandon` during a move: RADIO_FAULT, no reboot.
  - The §6 table rows the rig reaches: `c6_hold`, `rcp_probe_override empty`, an unplug, and a link fault via
    `usb_stall`.
- [ ] **Step 5:** Record the results in the ledger and memory. Report to the user, and fix what failed
  (back to Task 11's loop) before asking about the PR.
