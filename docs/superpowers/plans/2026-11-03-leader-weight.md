# Leader weight and "Become leader": implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** a stored leader weight adjustment and a "Become leader" action, exposed as `GET`/`PUT /device/thread` and as
a Leader card on the Network page, so this board can take the leader role from another border router and keep it
through later partition merges.

**Architecture:** `br_core` owns the setting: NVS storage, apply at bring-up and live through
`otThreadSetDeviceProperties`, and the take-over through `otThreadBecomeLeader`. The take-over decision is a pure
helper so it is host-tested. `device_api` adds one endpoint with a pure JSON layer. The page reads `GET /device/thread`
for this board's weight and the existing `GET /node` for the role and the leader's weight; its logic lives in a new
`static/leader.js` (pure `LeaderLogic` plus `LeaderPage`), tested under node.

**Tech stack:** ESP-IDF v6.1 (OpenThread, Thread 1.4), cJSON, NVS, Unity host tests, node page tests, pytest-embedded.

**Spec:** `docs/superpowers/specs/2026-11-03-leader-weight-design.md` (approved, `4b65dc6`). Read it with this plan.
Mockup: https://claude.ai/artifact/DHVssN3ZEPZXetCP8KTECj (`Main.dc.html`).

**Styleguide:** `STYLEGUIDE.md`. In short:
- astyle through `make lint`, `snake_case`, the prefixes `br_core_` and `device_api_`;
- `esp_err_t` with `ESP_RETURN_ON_ERROR`, one `TAG` per file;
- host tests in `<component>/test_host/`, named `"<scenario>: <effect>"`;
- target tests in `p4/pytest/test_<feature>.py`;
- the page script is ES5 like the other `static/*.js`, page tests in `tools/tests/test_<page>_page.py`;
- every change to vendored code gets a `PATCHES.md` entry.

## Global constraints

- Adjustment: an integer −16..+16, default 0. Stored in NVS namespace `thread`, key `leader_adj` (i8), written only
  when the value changes. `net reset` does not touch it.
- Base weight on this build: 64 + 1 (border router) + 0 (external power) = 65. The UI shows "base" as
  `leader_weight − leader_weight_adjustment` from the API, never a hard-coded 65.
- Take-over only through `"become_leader": true` in a PUT. Never automatic, never at boot. The role is not stored.
- OpenThread only takes over from a strictly lower weight: equal weights refuse.
- Field names and codes exactly as in spec section 3: `leader_weight_adjustment`, `leader_weight`,
  `leader_weight_base`, `partial`, `applied`, `detail`, `become_leader` (`"started"`); 400 `invalid_config` with
  `field`; 409 `cannot_take_over` with `detail` `weight not higher` | `already leader` | `not attached` |
  `not router eligible`; 409 `busy` `mode <MODE>`; 503 `busy` `OpenThread is busy, retry`; 500 `storage_failed`.
- PUT order: validate the whole body, then store and apply the adjustment, then try the take-over. A refused take-over
  keeps an adjustment sent in the same body.
- Lock waits: 1 s for the PUT; the GET uses the bounded wait the other Thread reads use.
- No flashing inside tasks. Target tests run in one batched hardware pass after review and fixes.
- Commits end with only `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review focus

Inputs the spec implies but no task's tests would otherwise exercise. Each one has a test added in its owning task.

1. **A script sends `8.0`, `8.5`, `"8"` or `null` as the adjustment.** `8.0` is accepted as 8 (cJSON keeps no
   integer/float distinction); `8.5`, strings and `null` answer 400 with `field` `leader_weight_adjustment`.
   Owner: Task 3.
2. **The page is stale when the button is pressed**: the leader changed meanwhile to a heavier one. The PUT answers 409
   `weight not higher`; the page shows that detail next to the button and refreshes both reads at once. Owner: Task 4.
3. **`GET /node` fails** (503 in `RADIO_FAULT` or while sniffing) while `GET /device/thread` answers. The card shows the
   stored adjustment, Save still works, the comparison is hidden and the button is greyed out with "Thread is not
   running". Owner: Task 4.
4. **The leader's weight is above what this board can reach** (82 against a maximum of 65 + 16 = 81). The hint says
   the leader's weight is above this board's maximum instead of suggesting an adjustment that doesn't exist.
   Owner: Task 4.
5. **NVS is unusable** (`storage_ok` false). PUT answers 500 `storage_failed` and changes nothing; GET answers
   adjustment 0; Thread still starts. Owner: Task 2 (runtime), Task 3 (error mapping).

## Design notes

### br_core interface (`p4/components/br_core/include/br_core.h`)

- `typedef enum { BR_CORE_LEADER_OK, BR_CORE_LEADER_NOT_RUNNING, BR_CORE_LEADER_NOT_ATTACHED,
  BR_CORE_LEADER_ALREADY, BR_CORE_LEADER_NOT_ELIGIBLE, BR_CORE_LEADER_WEIGHT_NOT_HIGHER } br_core_leader_refusal_t;`
- `#define BR_CORE_LEADER_ADJ_MIN (-16)` and `BR_CORE_LEADER_ADJ_MAX 16`.
- Pure, in `leader.c` (host-built):
  - `bool br_core_leader_adjustment_valid(int value);`
  - `br_core_leader_refusal_t br_core_leader_check(bool running, bool attached, bool is_leader, bool router_eligible,
    uint8_t own_weight, uint8_t leader_weight);` Checks in this order and returns the first that applies: not
    running, not attached, already leader, not eligible, `own_weight <= leader_weight`.
- Runtime, in `br_core.c`:
  - `esp_err_t br_core_set_leader_adjustment(int8_t adj, bool *applied);` `ESP_ERR_INVALID_ARG` out of range (nothing
    stored); `ESP_ERR_INVALID_STATE` when storage is degraded; an NVS error when the write fails. On `ESP_OK`,
    `*applied` is false when not `BR_RUNNING` or the lock was not free within 1 s.
  - `int8_t br_core_leader_adjustment(void);` the stored value, 0 when nothing is stored or storage is degraded;
    never takes a lock.
  - `esp_err_t br_core_leader_weight(uint8_t *out);` `otThreadGetLocalLeaderWeight`. `ESP_ERR_INVALID_STATE` when
    OpenThread is not running, `ESP_ERR_TIMEOUT` when the lock wait runs out.
  - `esp_err_t br_core_become_leader(br_core_leader_refusal_t *refusal);` Under the lock (1 s): reads role,
    `otThreadIsRouterEligible`, both weights, runs `br_core_leader_check`, then `otThreadBecomeLeader`. `ESP_OK`
    started; `ESP_ERR_INVALID_STATE` with `*refusal` set (also when `otThreadBecomeLeader` itself refuses:
    `OT_ERROR_NOT_CAPABLE` → weight not higher, `OT_ERROR_INVALID_STATE` → not attached); `ESP_ERR_TIMEOUT` on the
    lock.

### device_api pure layer (`p4/components/device_api/thread_json.c`, `thread_json.h`)

- `typedef struct { bool has_adjustment; int8_t adjustment; bool become_leader; } device_api_thread_put_t;`
- `esp_err_t device_api_thread_parse(const char *body, device_api_thread_put_t *out, const char **field);`
- `typedef struct { int8_t adjustment; bool weight_known; uint8_t weight; bool partial; bool has_applied;
  bool applied; const char *detail; bool become_started; } device_api_thread_t;`
- `int device_api_thread_json(char *buf, size_t cap, const device_api_thread_t *t);` length, or negative when it
  doesn't fit. `leader_weight_base` is `weight − adjustment`.
- `void device_api_thread_refusal(br_core_leader_refusal_t refusal, int mode, int *status, const char **code,
  char *detail, size_t cap);` `mode` is formatted with `device_api_mode_name` for `BR_CORE_LEADER_NOT_RUNNING`.

### Page (`static/leader.js`)

- `LeaderLogic.view(thread, node, pendingAdj)` → `{ownWeight, base, adjustment, leaderWeight, leaderName, role,
  verdict, verdictKind, scaleOwn, scaleLeader, buttonEnabled, hint, dirty, saveEnabled, comparisonShown}`. `thread`
  and `node` are the parsed replies or `null` when the read failed. Scale 48..96, positions in percent, clamped.
- `LeaderLogic.leaderName(node)`: "this board" when `state` is `leader`, else `router 0x` + 4 hex digits of
  `leaderRouterId × 1024`.
- `LeaderPage`: loads both replies, renders the card, owns the slider, Save, the confirm dialog and the polling.
- Copy: the card text, verdicts, hints, the "What Become leader does" box and the dialog are the mockup's
  (`Main.dc.html`), with the spec's corrections: the leader name rule above, and the hint for Review focus 4:
  "The leader's weight N is above the most this board can reach (M)."

## BDD skeleton

```
br_core_leader (leader.c, host)
    br_core_leader_adjustment_valid
        -16, 0, 16
            - true
        -17, 17
            - false
    br_core_leader_check
        Not running (other inputs anything)
            - BR_CORE_LEADER_NOT_RUNNING
        Running, not attached
            - BR_CORE_LEADER_NOT_ATTACHED
        Attached, this board is leader
            - BR_CORE_LEADER_ALREADY
        Attached, not router eligible
            - BR_CORE_LEADER_NOT_ELIGIBLE
        Own 65, leader 65
            - BR_CORE_LEADER_WEIGHT_NOT_HIGHER
        Own 64, leader 65
            - BR_CORE_LEADER_WEIGHT_NOT_HIGHER
        Own 66, leader 65, eligible, attached, not leader
            - BR_CORE_LEADER_OK
        Not attached and not eligible together
            - BR_CORE_LEADER_NOT_ATTACHED (order)

br_core leader runtime (br_core.c, on board)
    bring-up
        Stored adjustment +6
            - local leader weight 71 before Thread is enabled
        Nothing stored
            - adjustment 0, weight 65
    br_core_set_leader_adjustment
        17
            - ESP_ERR_INVALID_ARG, stored value unchanged
        +6 in BR_RUNNING
            - ESP_OK, applied true, local weight 71 at once
        +6 when not BR_RUNNING
            - ESP_OK, applied false, stored; applied at the next bring-up
        Same value as stored
            - ESP_OK, no NVS write
        Storage degraded
            - ESP_ERR_INVALID_STATE, nothing changes
    br_core_become_leader
        Leader weight equal or higher
            - ESP_ERR_INVALID_STATE, refusal WEIGHT_NOT_HIGHER, role unchanged
        Own weight higher
            - ESP_OK; role leader within 30 s; the old leader stays attached as router or child
        Lock not free within 1 s
            - ESP_ERR_TIMEOUT

device_api thread JSON (thread_json.c, host)
    device_api_thread_parse
        {"leader_weight_adjustment": 8}
            - ESP_OK, has_adjustment, 8, become_leader false
        {"leader_weight_adjustment": 8.0}
            - ESP_OK, 8
        {"become_leader": true}
            - ESP_OK, become_leader true, has_adjustment false
        Both keys
            - ESP_OK, both set
        {"become_leader": false}
            - ESP_OK, nothing to do
        Unknown key next to a known one
            - ESP_OK, ignored
        {}, {"x": 1}, [], "8", not JSON
            - ESP_ERR_INVALID_ARG, field "body"
        Adjustment 8.5, "8", null, true, 17, -17
            - ESP_ERR_INVALID_ARG, field "leader_weight_adjustment"
        become_leader 1, "true", null
            - ESP_ERR_INVALID_ARG, field "become_leader"
    device_api_thread_json
        Weight known (adjustment 8, weight 73)
            - {"leader_weight_adjustment":8,"leader_weight":73,"leader_weight_base":65}
        Weight unknown, not partial
            - only leader_weight_adjustment
        Partial
            - leader_weight_adjustment and "partial":true
        has_applied, applied false, detail "Thread is not running"
            - "applied":false,"detail":"Thread is not running"
        become_started
            - "become_leader":"started"
        Buffer too small
            - negative
    device_api_thread_refusal
        WEIGHT_NOT_HIGHER / ALREADY / NOT_ATTACHED / NOT_ELIGIBLE
            - 409 "cannot_take_over", detail "weight not higher" / "already leader" / "not attached" / "not router eligible"
        NOT_RUNNING, mode RADIO_FAULT
            - 409 "busy", detail "mode RADIO_FAULT"

device_api thread handlers (device_api.c, on board)
    GET /device/thread
        BR_RUNNING
            - 200, all three fields
        OpenThread not running
            - 200, adjustment only
    PUT /device/thread
        Invalid body
            - 400 invalid_config with field; nothing stored
        Adjustment only
            - 200, GET body plus applied
        Adjustment and become_leader, take-over refused
            - 409 cannot_take_over; a following GET shows the new adjustment
        become_leader, take-over started
            - 200 with "become_leader":"started"
        Storage failure
            - 500 storage_failed
        Lock busy during the take-over
            - 503 busy "OpenThread is busy, retry"

LeaderLogic (leader.js, node)
    view
        Router, own 61, leader 65
            - verdict lower, button disabled, hint "Needs a weight above 65 (the leader’s). Raise the adjustment to +1 or more."
        Router, own 65, leader 65
            - verdict equal, button disabled, same hint
        Router, own 73, leader 65
            - verdict higher, button enabled, hint "Takes over now. The mesh reattaches for a few seconds."
        This board is leader
            - verdict "This board is the leader.", button disabled, hint "Already the leader."
        Pending slider value differs from the saved one
            - dirty, Save enabled, button disabled, hint "Save the adjustment first."
        Leader 82, base 65
            - button disabled, hint "The leader’s weight 82 is above the most this board can reach (81)."
        node state detached or disabled
            - comparison hidden, button disabled, hint "This board is not attached to a Thread network."
        node read failed (null)
            - comparison hidden, button disabled, hint "Thread is not running.", Save still enabled when dirty
        thread reply partial (no weight)
            - comparison hidden, button disabled
        Scale
            - 48 → 0 %, 96 → 100 %, 40 → 0 %, 100 → 100 %
    leaderName
        state leader
            - "this board"
        leaderRouterId 11
            - "router 0x2c00"

LeaderPage (leader.js, node with the fake DOM)
    load
        Both reads answer
            - card rendered with both weights and the breakdown "65 base · +8 adjustment"
    Save
        Slider moved to +8, Save
            - PUT /device/thread {"leader_weight_adjustment":8}; card shows the reply
        400 reply
            - detail shown under the slider; slider keeps the value
    Become leader
        Click
            - confirm dialog opens; no request yet
        Cancel
            - dialog closes; no request
        Confirm, 200 started
            - PUT {"become_leader":true}; polls both reads every 1 s until node state is leader, at most 30 s
        Confirm, 409 weight not higher
            - detail shown next to the button; both reads refreshed at once
    Polling
        Page visible
            - both reads every 5 s
        Page hidden
            - no reads until visible again

On-board (p4/pytest/test_leader.py)
    Peer is leader with weight 70
        - GET /node leaderData.weighting 70; PUT become_leader → 409 weight not higher
    Adjustment +6
        - 200 applied; GET leader_weight 71
    Take-over
        - 200 started; board state leader within 30 s; peer router or child
    Peer leader again at weight 82, board +16
        - PUT become_leader → 409 weight not higher
    Reboot
        - GET leader_weight_adjustment 16 and leader_weight 81
```

## Tasks

### Task 1: The take-over decision (pure)

**Files:**
- Create: `p4/components/br_core/leader.c`.
- Modify: `p4/components/br_core/include/br_core.h` (the enum, limits and the two pure functions only),
  `p4/components/br_core/CMakeLists.txt` (`leader.c` in both branches).
- Create: `p4/components/br_core/test_host/main/test_leader.c`; modify `test_host/main/CMakeLists.txt`.

**Interfaces:** produces `br_core_leader_refusal_t`, `BR_CORE_LEADER_ADJ_MIN/MAX`,
`br_core_leader_adjustment_valid`, `br_core_leader_check` (see Design notes). `leader.c` uses no OpenThread type, so
the host build needs no new stub.

- [ ] Write `test_leader.c` from the `br_core_leader` branch of the skeleton, one `TEST_CASE` per effect, tagged
  `[br_core]`.
- [ ] Run `logrun make test-host`. Expected: the br_core suite fails to build.
- [ ] Implement `leader.c`.
- [ ] Run `logrun make test-host`. Expected: all suites green.
- [ ] Commit `feat(br_core): leader take-over decision`.

### Task 2: Stored adjustment, bring-up and take-over in br_core

**Files:** modify `p4/components/br_core/br_core.c` and `include/br_core.h`.

**Interfaces:** consumes Task 1. Produces `br_core_set_leader_adjustment`, `br_core_leader_adjustment`,
`br_core_leader_weight`, `br_core_become_leader` (see Design notes).

- [ ] Load `leader_adj` from NVS namespace `thread` once, when storage is usable (after
  `br_core_note_storage_init`), into a static that `br_core_leader_adjustment` returns. Degraded storage: 0, and the
  setter answers `ESP_ERR_INVALID_STATE` (Review focus 5).
- [ ] In `bringup_task`, apply the adjustment under the lock already held, before `otIp6SetEnabled`/
  `otThreadSetEnabled`: `otThreadGetDeviceProperties`, set `mLeaderWeightAdjustment`, `otThreadSetDeviceProperties`.
  Check that every path that runs `esp_openthread_init` again (radio restart, recovery) reaches `bringup_task`; apply
  there too if one doesn't.
- [ ] Implement the setter: range check, NVS write only when the value differs, then the live apply in `BR_RUNNING`
  with a 1 s lock wait.
- [ ] Implement `br_core_leader_weight` with the bounded wait the status reads use, and `br_core_become_leader` as in
  Design notes.
- [ ] Log one line per change: `leader weight adjustment %+d (weight %u)` and `taking over as leader (weight %u > %u)`.
- [ ] Run `logrun make build` and `logrun make test-host`. Expected: both OK.
- [ ] Commit `feat(br_core): stored leader weight adjustment and become leader`.

### Task 3: `/device/thread` API

**Files:**
- Create: `p4/components/device_api/thread_json.c`, `thread_json.h` (pure).
- Create: `p4/components/device_api/test_host/main/test_thread.c`; modify `test_host/main/CMakeLists.txt`.
- Modify: `p4/components/device_api/CMakeLists.txt` (`thread_json.c` in both branches), `device_api.c` (two routes and
  handlers next to `/device/names`).
- Modify: `p4/components/esp_ot_br_server/src/esp_br_web.c` (`EXTRA_HANDLER_SLOTS` 20 → 24: the table has 18 routes
  plus the catch-all, these add 2) and `p4/components/PATCHES.md` (patch 10's slot sentence).
- Modify: `docs/api.md` (two rows after `/device/names`).

**Interfaces:** consumes Tasks 1 and 2. Produces the HTTP contract in spec section 3.

- [ ] Write `test_thread.c` from the `device_api thread JSON` branch of the skeleton (Review focus 1 included).
- [ ] Run `logrun make test-host`. Expected: the device_api suite fails to build.
- [ ] Implement `thread_json.c` with cJSON for parsing and `json_out` for writing, like `names_json.c`.
- [ ] Run `logrun make test-host`. Expected: green.
- [ ] Add the handlers:
  - GET: `br_core_leader_adjustment`, then `br_core_leader_weight` (`ESP_ERR_TIMEOUT` → `partial`).
  - PUT: `read_small_body` with a 256-byte cap, `device_api_thread_parse`, then the setter when `has_adjustment`
    (`ESP_ERR_INVALID_STATE` or an NVS error → 500 `storage_failed`; `applied` false gets `detail` "Thread is not
    running" or "OpenThread is busy, applied at the next start"), then `br_core_become_leader` when `become_leader`
    (refusal → `device_api_thread_refusal`; `ESP_ERR_TIMEOUT` → 503 `busy` "OpenThread is busy, retry").
  - 400 replies carry `field` the way `/device/names` builds them.
- [ ] Add the `docs/api.md` rows: `GET /device/thread` and `PUT /device/thread` (both keys, the order, the errors).
- [ ] Run `logrun make build`. Expected: OK.
- [ ] Commit `feat(device_api): GET/PUT /device/thread`.

### Task 4: Leader card on the Network page

**Files:**
- Create: `p4/components/esp_ot_br_server/frontend/static/leader.js`, exporting `LeaderLogic` with
  `module.exports.Page = LeaderPage`, like `names.js`.
- Modify: `frontend/network.html` (the card first in the container, the confirm dialog next to the join modal, the
  script tag) and `frontend/static/style.css` only for what existing classes don't cover (the scale and the two weight
  tiles).
- Create: `tools/tests/test_leader_page.py`.
- Modify: `tools/tests/device_pages_harness.js` (mocks for `GET`/`PUT /device/thread` and `GET /node`, if missing).

**Interfaces:** consumes the spec section 3 JSON and `GET /node` (`state`, `leaderData.weighting`,
`leaderData.leaderRouterId`).

- [ ] Write `test_leader_page.py` from the `LeaderLogic` and `LeaderPage` branches of the skeleton (Review focus 2, 3
  and 4 included).
- [ ] Run `logrun make test-tools`. Expected: the new tests fail (no `leader.js`).
- [ ] Implement `leader.js` and the HTML to the mockup (`Main.dc.html`) and spec section 4. The slider is a real
  `<input type="range" min="-16" max="16" step="1">` with a `<label>`; the dialog uses the page's existing
  `modal-overlay` pattern.
- [ ] Run `logrun make test-tools`. Expected: all green, `test_network_page.py` included.
- [ ] Commit `feat(web): Leader card on the Network page`.

### Task 5: On-board test and docs

**Files:**
- Create: `p4/pytest/test_leader.py`. It uses the `peer`, `stored_dataset` and `board_ip` fixtures; the peer joins the
  board's network the way `thread_peer_address` in `test_latency.py` does, then is made leader with
  `peer.command("leaderweight 70")` and `peer.command("state leader")`.
- Modify: `docs/api.md` (subsection "Choosing the leader" under "Adding more border routers": why another border router
  stays leader, the adjustment, the button, the "Afterwards" caveat), `CHANGELOG.md` (Unreleased, Added).

- [ ] Write `test_leader.py` from the on-board branch of the skeleton. A teardown PUTs adjustment 0. Don't run it:
  hardware pass.
- [ ] Write the docs. No BDD: docs only.
- [ ] Run `logrun make lint` and `logrun make test`. Expected: both green.
- [ ] Commit `test(leader): on-board test; docs: choosing the leader`.

### Task 6: Review, then the hardware pass

- [ ] Whole-branch review on the most capable model. Fix the Critical and Important findings test-first, and commit
  each fix.
- [ ] Batched hardware pass (with the user):
  - `make test-target TEST=p4/pytest/test_leader.py`;
  - by hand: with the board as leader, reboot it and note who is leader afterwards and when; then keep it off for
    3 minutes and note again. Correct the "Afterwards" text in `leader.js`, `network.html` and `docs/api.md` if it
    differs from "about 2 minutes", and remove the "provisional" note from spec section 6;
  - a look at the Leader card in a browser against the real network (Linux OTBR as leader).

## Verification

- `logrun make test-host`: the br_core and device_api suites are green.
- `logrun make test-tools`: the leader page tests and the existing page tests are green.
- `logrun make build` and `logrun make lint`: OK.
- Hardware (deferred, batched): `make test-target TEST=p4/pytest/test_leader.py`, the reboot check, and the browser
  check.
