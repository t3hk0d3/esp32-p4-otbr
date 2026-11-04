# Thread Console Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
> Chosen method: **Native** (superpowers:executing-plans).

**Goal:** The board's OpenThread CLI on the network: a WebSocket at `/device/console/ws`, a settings route, and a
Thread Console page, with the board staying a border router throughout.

**Architecture:** A new component `ot_console` owns the setting, the CLI output callback, the input arbiter (WebSocket
or USB `ot` command, one at a time, Busy while a command is pending) and a bounded output buffer; its decisions live
in a host-tested pure core. `device_api` adds the settings routes and the WebSocket transport (pings, take-over, close
codes). The page is `console.html` with `static/console.js` split into pure logic and DOM.

**Tech Stack:** ESP-IDF 6.1 (`esp_http_server` with `CONFIG_HTTPD_WS_SUPPORT`, OpenThread CLI API `otCliInit`,
`otCliInputLine`), Unity host tests on the `linux` target, vanilla JS under node for page tests, pytest-embedded with
`aiohttp` for the on-board test.

**Spec:** `docs/superpowers/specs/2026-11-04-thread-console-design.md`

## Styleguide

`STYLEGUIDE.md` (enforced by `make lint`): astyle for C, ruff for Python, `snake_case`, public symbols prefixed with
the component name (`ot_console_`), one `TAG` per file, `esp_err_t` returns with `ESP_RETURN_ON_ERROR`, host tests in
`<component>/test_host/` named `TEST_CASE("<scenario>: <effect>", "[ot_console]")`, target tests in
`p4/pytest/test_console.py`, test-only code under `#ifdef OTBR_TEST_BUILD`. Vendored `esp_ot_br_server` changes go into
`p4/components/PATCHES.md`. Every task follows it.

## Global Constraints

- Off by default; the setting survives reboots (NVS namespace `console`, key `enabled`, u8).
- One session at a time, newest wins. Close codes: 4001 taken over, 4003 console turned off, 4009 too many bytes
  queued; 1001 with notice "no answer" after 30 s without a pong.
- Board pings every 10 s; a pong refreshes the socket's LRU stamp (`httpd_sess_update_lru_counter`).
- Board notices are separate text frames starting with `# `. Exact texts:
  - `# Connected to <hostname> (OpenThread <version>, <role> <0xRLOC16>)`
  - `# Thread is not running (mode <MODE>)`
  - `# Busy: "<pending line>" is still running`
  - `# Closed: a new console session opened from <address>`
  - `# Line too long (max <n> bytes)`
  - `# Output dropped (<n> bytes)`
  - `# Closed: the console was turned off`
- A command is pending from posting the line until the CLI prints its `"> "` prompt; the prompt is never forwarded.
- While pending, only `factoryreset` is passed to the CLI; everything else gets the Busy notice.
- Max line: `OPENTHREAD_CONFIG_CLI_MAX_LINE_LENGTH - 1` bytes after stripping a trailing `\r`/`\n` and a leading
  `ot `.
- No OpenThread session stays open between commands; each line opens and closes one around its post.
- Lines are refused with the Thread not running notice unless the mode is `BR_MODE_BR_RUNNING` (SNIFFER, radio fault,
  stopped, a push all refuse).
- Output buffer cap 8 KB; a client that drains nothing for 15 s while output waits is closed with 4009 (my values for
  the spec's "builder's call").
- Every command is logged at INFO: `console <peer>: <line>` (`usb` as peer for the USB `ot` command).
- Routes are in the `NETCFG_SVC_DEVICE_API` service class (through `device_api`'s existing `gated_handler`).
- `GET`/`PUT /device/console` work in every mode; body `{"enabled": bool, "session": null | {"peer": str,
  "since_s": int}}`.
- Page: `/console.html`, nav label `Console`, under Management after Tools; `data-guard-exempt`; the web budget in
  `test_device_pages.py` (384 KB) is not raised without asking.
- Risky commands (page only): `factoryreset`, `reset`, `thread stop`, `ifconfig down`, and `dataset <sub>` for every
  sub-command except none, `active`, `pending` (each with or without `-x`).

## Review Focus

1. A command that never prints a prompt (a crashed or dropped async command) would leave the console on Busy forever:
   stack stop/restart clears it (Task 1 test), and the USB `ot` wait times out (Task 2 test).
2. Output arriving in fragments that split `"> "` or a line across callbacks: prompt detection must work on the
   callback's formatted text, not on whole lines (Task 1 test "prompt split across two writes").
3. The page reconnecting after a reboot must not repeat the command that was waiting (Task 4 test).
4. A client vanishing without a close (laptop sleep) frees the console within 30 s (Task 5 on-board check).
5. Turning the console off while a command is pending: the session closes with 4003, and the pending flag stays until
   the prompt so the CLI's state and the arbiter's agree (Task 1 test).

## File map

- Create `p4/components/ot_console/` with `CMakeLists.txt`, `include/ot_console.h` (board API),
  `include/ot_console_core.h` (pure, host-safe), `core.c` (pure core), `ot_console.c` (board glue: NVS, CLI callback,
  input, logging, USB `ot` command), `test_host/` (Unity project like `netnames/test_host`).
- Modify `p4/components/br_core/br_core.c`, `br_core.h`, `br_core_test.c`: a start/stop hook replaces the test-only
  `esp_openthread_cli_init` call and the `ot` command plumbing.
- Modify `p4/main/app_main.c`, `p4/main/test_cmds.c`: init `ot_console`, register its `ot` command.
- Create `p4/components/device_api/console_ws.c` + `console_json.c`/`.h`; modify `device_api.c`, `CMakeLists.txt`,
  `test_host/`.
- Modify `p4/sdkconfig.defaults` (`CONFIG_HTTPD_WS_SUPPORT=y`),
  `p4/components/esp_ot_br_server/src/esp_br_web.c` (`EXTRA_HANDLER_SLOTS`), `p4/components/PATCHES.md`.
- Create `frontend/console.html`, `frontend/static/console.js`; modify `static/api.js` (nav), `static/style.css`.
- Create `tools/tests/test_console_page.py`; modify `tools/tests/test_device_pages.py`, `test_mgmt_pages.py` (nav).
- Create `p4/pytest/test_console.py`.
- Modify `docs/api.md`, `CHANGELOG.md`, `README.md`; add `docs/images/console.png`.

## BDD skeleton

```
ot_console_core (pure)
    ot_console_line_normalise
        On a plain command
            - returns OK and leaves the line unchanged
        On a line with a trailing "\r\n" or "\n"
            - strips it
        On a line starting with "ot " (any case of spacing after it)
            - strips the prefix
        On an empty or blank line
            - returns EMPTY
        On a line longer than the maximum after stripping
            - returns TOO_LONG
    ot_console_arb_submit
        On a line with nothing pending and the stack running
            - returns SEND and records the line and its source as pending
        On a second line while one is pending
            - returns BUSY and keeps the first line as pending
        On "factoryreset" while a line is pending
            - returns SEND
        On a line while the stack is not running
            - returns NOT_RUNNING and records nothing
        On a USB line while a WebSocket session is attached
            - returns IN_USE
    ot_console_arb_output
        On output while a WebSocket line is pending
            - routes it to the session
        On output while a USB line is pending
            - routes it to stdout
        On output with nothing pending and a session attached
            - routes it to the session
        On output with nothing pending and no session
            - routes it to stdout
        On the "> " prompt
            - is not forwarded and clears the pending line
        On a prompt split across two writes ("> " as ">" then " ")
            - is still recognised and not forwarded
    ot_console_arb_stack
        On the stack stopping or restarting
            - clears the pending line
    ot_console_arb_detach
        On detaching while a line is pending
            - keeps the line pending until the prompt
    ot_console_buf (bounded output)
        On pushing within the cap
            - take returns the bytes in order
        On pushing past the cap
            - drops the oldest bytes and reports the dropped count once
    ot_console_notice_* (text)
        On each notice
            - produces the exact text from Global Constraints
ot_console (board)
    ot_console_set_enabled
        On turning on
            - stores enabled=1 and GET reports enabled
        On turning off with a session attached
            - stores enabled=0 and closes the session with 4003
    ot_console_submit
        On a line in BR_RUNNING
            - logs it at INFO, opens and closes an OpenThread session, posts it to the CLI
        On a line in SNIFFER
            - refuses with "# Thread is not running (mode SNIFFER)"
    USB ot command (test builds)
        On a command while no session is attached
            - prints the output and returns after the prompt
        On no prompt within 10 s
            - returns with a timeout message instead of blocking
        On a command while a session is attached
            - prints "console in use from <address>"
device_api console routes
    GET /device/console
        On no session
            - 200 {"enabled": <bool>, "session": null}
        On a session
            - 200 with "session": {"peer", "since_s"}
    PUT /device/console
        On {"enabled": true|false}
            - stores it and answers like GET
        On a missing or non-boolean "enabled"
            - 400 with the API's usual error body
    GET /device/console/ws
        On upgrade while off
            - closes with 4003 and the off notice
        On upgrade while on
            - sends the Connected notice; an earlier session gets the take-over notice and 4001
        On a text frame
            - submits the line and streams output frames back
        On 30 s without a pong
            - closes with 1001 "no answer"
        On output not drained for 15 s
            - closes with 4009
ConsoleLogic (page, pure)
    normalise
        On "ot state" / "state\n"
            - returns "state"
        On ""
            - returns null
    riskyReason
        On each risky command
            - returns its explanation
        On "dataset", "dataset active -x", "dataset pending"
            - returns null
    noticeState
        On each board notice
            - maps it to the pill state (connected, not-running, taken-over, off, busy)
    backoff
        On attempts 1, 2, 3, 4, 10
            - returns 1000, 2000, 5000, 5000, 5000 ms
    history
        On up/down past either end
            - stops at the oldest entry / returns to an empty input
    reconnect
        On reconnecting with a line still waiting
            - marks it lost and never resends it
ConsolePage (DOM)
    On open with the console on and no session
        - connects by itself
    On open with another session
        - shows "In use by <address>" and connects only on Connect
    On a risky command
        - opens the confirm dialog; Cancel sends nothing
    On Disconnect
        - closes the socket and disables the input
```

---

### Task 1: `ot_console` pure core

**Files:**
- Create: `p4/components/ot_console/include/ot_console_core.h`, `p4/components/ot_console/core.c`,
  `p4/components/ot_console/CMakeLists.txt` (linux target builds `core.c` only, like `device_api`),
  `p4/components/ot_console/test_host/{CMakeLists.txt,main/CMakeLists.txt,main/test_core.c}`

**Interfaces:**
- Produces (all in `ot_console_core.h`, no ESP-IDF or OpenThread headers):
  - `typedef enum { OT_CONSOLE_LINE_OK, OT_CONSOLE_LINE_EMPTY, OT_CONSOLE_LINE_TOO_LONG } ot_console_line_t;`
  - `ot_console_line_t ot_console_line_normalise(char *line, size_t max_len);` (in place)
  - `typedef enum { OT_CONSOLE_SRC_WS, OT_CONSOLE_SRC_USB } ot_console_src_t;`
  - `typedef enum { OT_CONSOLE_SEND, OT_CONSOLE_BUSY, OT_CONSOLE_NOT_RUNNING, OT_CONSOLE_IN_USE } ot_console_verdict_t;`
  - `typedef enum { OT_CONSOLE_TO_SESSION, OT_CONSOLE_TO_STDOUT, OT_CONSOLE_DROP_PROMPT } ot_console_dest_t;`
  - `typedef struct ot_console_arb ot_console_arb_t;` with a public struct definition sized for a static instance
    (pending line copy, source, attached flag, running flag, prompt-split carry).
  - `void ot_console_arb_init(ot_console_arb_t *a);`
  - `ot_console_verdict_t ot_console_arb_submit(ot_console_arb_t *a, ot_console_src_t src, const char *line);`
  - `ot_console_dest_t ot_console_arb_output(ot_console_arb_t *a, const char *text, size_t len);`
  - `void ot_console_arb_attach(ot_console_arb_t *a, bool attached);`
  - `void ot_console_arb_stack(ot_console_arb_t *a, bool running);`
  - `const char *ot_console_arb_pending(const ot_console_arb_t *a);` (NULL when none)
  - `typedef struct { uint8_t *data; size_t cap, head, len; size_t dropped; } ot_console_buf_t;` with
    `void ot_console_buf_init(ot_console_buf_t *b, uint8_t *storage, size_t cap);`,
    `void ot_console_buf_push(ot_console_buf_t *b, const char *s, size_t n);`,
    `size_t ot_console_buf_take(ot_console_buf_t *b, char *out, size_t cap);`,
    `size_t ot_console_buf_take_dropped(ot_console_buf_t *b);` (returns and resets the count)
  - Notice formatters, each `int ot_console_notice_<name>(char *out, size_t cap, ...)` returning the length:
    `connected(hostname, ot_version, role, rloc16)`, `not_running(mode_name)`, `busy(pending)`,
    `taken_over(address)`, `too_long(max)`, `dropped(n)`, `turned_off()`.

- [ ] **Step 1: Write the failing host tests** in `test_host/main/test_core.c`, one `TEST_CASE` per effect of the
  skeleton's `ot_console_core` branch, e.g. `TEST_CASE("factoryreset while pending: is sent", "[ot_console]")`
  asserting `ot_console_arb_submit(&a, OT_CONSOLE_SRC_WS, "factoryreset") == OT_CONSOLE_SEND` after a pending `ping`;
  the split-prompt case feeds `">"` then `" "` and asserts the second call returns `OT_CONSOLE_DROP_PROMPT` and
  `ot_console_arb_pending(&a) == NULL`; notice tests compare against the Global Constraints texts byte for byte.
- [ ] **Step 2: Run, expect build failure** — `logrun make test-host` (fails: `ot_console_core.h` missing).
- [ ] **Step 3: Implement** `core.c`. The prompt carry is the only subtle part: keep at most the last byte of a write
  when it is `>` with nothing after it, and treat `"> "` (whole or completed by the next write's leading space) as the
  prompt; any other text flushes the carried `>` to the normal destination.
- [ ] **Step 4: Run, expect pass** — `logrun make test-host`.
- [ ] **Step 5: Commit** — `feat(ot_console): pure core: line rules, input arbiter, prompt detection, output buffer`.

### Task 2: `ot_console` board glue, `br_core` hook, USB `ot` command

**Files:**
- Create: `p4/components/ot_console/include/ot_console.h`, `p4/components/ot_console/ot_console.c`
- Modify: `p4/components/br_core/include/br_core.h`, `br_core.c` (call sites at lines ~470 and ~1083),
  `br_core_test.c` (remove the `ot` command plumbing), `p4/main/app_main.c`, `p4/main/test_cmds.c`

**Interfaces:**
- Consumes: Task 1's core.
- Produces (`ot_console.h`):
  - `typedef struct { void (*wake)(void *ctx); void (*close)(void *ctx, uint16_t code, const char *notice); void *ctx; } ot_console_sink_t;`
    (`wake` is called from the OpenThread task when output is buffered and must not block)
  - `esp_err_t ot_console_init(void);` (loads the setting, registers the `br_core` hook)
  - `bool ot_console_enabled(void);` / `esp_err_t ot_console_set_enabled(bool on);`
  - `esp_err_t ot_console_attach(const char *peer, ot_console_sink_t sink, uint32_t *session_id);` (takes over:
    the previous sink gets `close(4001, taken-over notice)`)
  - `void ot_console_detach(uint32_t session_id);`
  - `esp_err_t ot_console_submit(uint32_t session_id, const char *line);` (notices for refusals go into the session's
    output, so the transport only drains)
  - `size_t ot_console_take(uint32_t session_id, char *out, size_t cap);` (drains output; a pending drop count comes
    first as the dropped notice)
  - `bool ot_console_session(char *peer, size_t peer_cap, uint32_t *since_s);`
  - `esp_err_t ot_console_usb_command_register(void);` (`#ifdef OTBR_TEST_BUILD`)
- Produces (`br_core.h`): `void br_core_set_ot_cli_hook(void (*hook)(bool running));` called under the OpenThread
  lock right after the netif is attached (`true`, in every build) and before deinit (`false`); replaces
  `br_core_test_ot_command_register` / `br_core_test_ot_command_set_running`.

- [ ] **Step 1: Write the failing on-board check** in `p4/pytest/test_console.py`: `test_usb_ot_command_still_works`
  runs `ot state` on the USB console and expects a role line and `Done` (keeps the existing USB tests honest), and
  `test_usb_ot_command_times_out` runs `ot ping <this board's mesh-local EID> 8 5 3` (five pings 3 s apart, about 12 s
  before the prompt) and expects the timeout message after 10 s, then the late output and a working `ot state`.
- [ ] **Step 2: Implement** `ot_console.c`: the hook calls `otCliInit(instance, output_cb, NULL)` with `true` and
  `ot_console_arb_stack`; `output_cb` formats with `vsnprintf` into a stack buffer of
  `OPENTHREAD_CONFIG_CLI_MAX_LINE_LENGTH` bytes (longer writes go out in pieces), asks the arbiter for the
  destination, then `vprintf`s or pushes to the buffer and calls `wake`. `ot_console_submit` checks the mode
  (`br_core_get_mode`), normalises, asks the arbiter, logs, then `br_core_ot_session_begin`,
  `esp_openthread_task_queue_post` of `otCliInputLine` on a copy, `br_core_ot_session_end`. The USB command waits on
  a semaphore given at the prompt, 10 s timeout. NVS as in `netnames.c`.
- [ ] **Step 3: Build and lint** — `logrun make build-test` and `logrun make lint`, both pass; `logrun make
  test-host` still passes.
- [ ] **Step 4: Commit** — `feat(ot_console): CLI output routing, input path and USB ot command`.

The on-board run of Step 1 is batched into Task 5 (no flashing inside tasks).

### Task 3: WebSocket transport and settings routes

**Files:**
- Create: `p4/components/device_api/console_json.c`, `console_json.h`, `console_ws.c`
- Modify: `p4/components/device_api/device_api.c` (routes, `route_t` gains `bool websocket`), `CMakeLists.txt`,
  `test_host/main/` (new `test_console_json.c`), `p4/sdkconfig.defaults`,
  `p4/components/esp_ot_br_server/src/esp_br_web.c` (`EXTRA_HANDLER_SLOTS` 24 → 27), `p4/components/PATCHES.md`

**Interfaces:**
- Consumes: Task 2's `ot_console.h`.
- Produces: `int device_api_console_json(char *out, size_t cap, bool enabled, const char *peer, uint32_t since_s);`
  (peer NULL → `"session": null`) and `esp_err_t device_api_console_parse(const char *body, bool *enabled);`
  (`ESP_ERR_INVALID_ARG` when missing or not a boolean); routes `GET /device/console`, `PUT /device/console`,
  `GET /device/console/ws` (`is_websocket = true`, `handle_ws_control_frames = true`).

- [ ] **Step 1: Write the failing host tests** in `test_console_json.c` for the JSON and parse effects of the
  skeleton (`{"enabled":true}` → true; `{"enabled":"yes"}`, `{}`, invalid JSON → `ESP_ERR_INVALID_ARG`; with a peer
  the body is exactly `{"enabled":true,"session":{"peer":"192.168.10.23","since_s":812}}`).
- [ ] **Step 2: Run, expect failure** — `logrun make test-host`.
- [ ] **Step 3: Implement** the JSON helpers, then `console_ws.c`: on upgrade, refuse with 4003 when off, else
  `ot_console_attach` with a sink whose `wake` does `httpd_queue_work` to drain `ot_console_take` into
  `httpd_ws_send_frame_async` text frames; text frames go to `ot_console_submit`; an `esp_timer` per session sends a
  ping every 10 s and closes with 1001 after 30 s without a pong; a pong calls `httpd_sess_update_lru_counter`; the
  drain tracks the last time it sent anything while bytes were waiting and closes with 4009 after 15 s; socket close
  calls `ot_console_detach`. Settings handlers use the JSON helpers and the API's existing error helpers.
- [ ] **Step 4: Run** — `logrun make test-host`, `logrun make build-test`, `logrun make lint`: all pass.
- [ ] **Step 5: Commit** — `feat(device_api): /device/console settings and WebSocket transport`.

### Task 4: Thread Console page

**Files:**
- Create: `p4/components/esp_ot_br_server/frontend/console.html`, `frontend/static/console.js`,
  `tools/tests/test_console_page.py`
- Modify: `frontend/static/api.js` (nav: `{href : '/console.html', label : 'Console'}` after Tools),
  `frontend/static/style.css` (`.con-*` classes, phone layout as in the mockup), `tools/tests/test_mgmt_pages.py`
  (nav), `tools/tests/test_device_pages.py` (page list only)

**Interfaces:**
- Consumes: Task 3's routes and the Global Constraints notice texts.
- Produces: `ConsoleLogic` (exported for node like `LeaderLogic`): `normalise(line) → string|null`,
  `riskyReason(cmd) → string|null`, `noticeState(text) → 'connected'|'not-running'|'taken-over'|'off'|'busy'|null`,
  `backoff(attempt) → ms`, `historyStep(history, pos, dir) → {pos, value}`; `ConsolePage` (DOM, WebSocket).

- [ ] **Step 1: Write the failing node tests** in `test_console_page.py` (with `device_js.logic` / `page` /
  `scenario` like `test_leader_page.py`), one per effect of the skeleton's `ConsoleLogic` and `ConsolePage` branches,
  with the exact risky explanations from the mockup (e.g. `thread stop` → "Stops Thread on this board. It stops being
  a border router for <network> until Thread is started again.") and a fake WebSocket in the page harness.
- [ ] **Step 2: Run, expect failure** — `logrun make test-tools`.
- [ ] **Step 3: Implement** `console.html`, `console.js`, the nav entry and the styles per the mockup (desktop and
  phone).
- [ ] **Step 4: Run** — `logrun make test-tools` passes, including the unchanged 384 KB budget; if the budget fails,
  stop and ask.
- [ ] **Step 5: Commit** — `feat(web): Thread Console page`.

### Task 5: On-board tests, docs, hardware pass

**Files:**
- Modify: `p4/pytest/test_console.py` (from Task 2), `docs/api.md`, `CHANGELOG.md`, `README.md`
- Create: `docs/images/console.png`

- [ ] **Step 1: Write the on-board tests** with `aiohttp`'s WebSocket client, one test per Testing bullet of the
  spec: off by default (4003), `PUT` on, `state` → role + `Done`, `dns browse _meshcop._udp.default.service.arpa`
  returns this board's meshcop record, Busy during `ping`, take-over (4001 + notice), SNIFFER refusal and recovery,
  every web page loaded in turn leaves the console open, a client that stops answering pings is closed within 40 s,
  turning off closes with 4003. Teardown turns the console off.
- [ ] **Step 2: Hardware pass** (one flash, after Tasks 1–4 are reviewed): check `/device/status` `app.version`
  against the last build put on the board in its own step, then `logrun make build-test`, `logrun make flash`, and
  `logrun make test-target TEST=p4/pytest/test_console.py`. Peer tests need the aux C6's `ot_cli`, which currently
  runs ESPHome; this file needs no peer.
- [ ] **Step 3: Docs** — `docs/api.md` Thread Console section (routes, frames, notices, close codes, websocat
  example), screenshot of the page from the board, `CHANGELOG.md` entry, README feature line.
- [ ] **Step 4: Full checks** — `logrun make test`, `logrun make build`, `logrun make lint`.
- [ ] **Step 5: Commit** — `test, docs(console): on-board console tests, API docs and screenshot`.
