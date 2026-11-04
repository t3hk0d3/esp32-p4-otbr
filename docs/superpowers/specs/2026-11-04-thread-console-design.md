# Thread Console: the OpenThread CLI over a WebSocket: design

Status: draft for review, 2026-11-04. Mockup (desktop, states, phone):
<https://claude.ai/artifact/GdcV1ek3Si2BaRKyBPJ46N>.

## 1. Purpose and scope

Debugging the mesh today needs a second Thread node: the spare C6 devkit on the host, running `ot_cli`, joined to
the network by hand. That C6 now runs ESPHome over Thread, so it is a device under test, no longer a debugging tool.
The board itself is already a full OpenThread node on the mesh, and the OpenThread CLI is already in the firmware,
but only on the USB console, and opening that port power-on-resets the P4.

This feature puts the board's own OpenThread CLI on the network: a WebSocket endpoint that runs CLI command lines on
the live stack and streams their output back, used from a new **Thread Console** page in the web UI and from a shell
with `websocat`. The board stays a border router the whole time.

Who uses it:

- Developers and agents probing the mesh from a shell or a script (`dns browse`, `udp send`, `netdata show`,
  `leaderdata`, `ping`, `router table`, `srp server host`), and on-board pytest checks that today need the C6.
- The user, from the web UI, to look under the Topology or Network page without a USB cable.

Success: everything that was done from the C6 on 2026-11-03/04 (DNS-SD browses against other border routers, UDP and
mDNS probes, network data and leader checks) works from the console, and using the console never disturbs the border
router unless the typed command obviously does.

Goals:

- The full OpenThread CLI, not a curated subset.
- Output streams as it arrives, including late output of asynchronous commands (`dns browse`, `ping`).
- One endpoint for both the web page and `websocat`.
- Off by default; turned on by a stored setting.

Non-goals:

- A second, separate node on the mesh (the Spinel pass-through idea, or a spare devkit, covers that).
- Authentication or encryption. The console is as protected as the rest of the API (LAN, no login). The off switch
  guards against accidents and drive-by use, not against someone on the LAN.
- The ESP-IDF console (`esp_console` commands such as `c6_hold`), and the IDF log stream.
- Changing the USB console.

Anti-goals (would make it a failure even if it works):

- The console holding OpenThread open: a radio push or ROM recovery must never wait on an idle console.
- An idle console tab getting silently killed by normal page browsing, or the console starving the web UI of sockets.
- A slow or stalled client blocking the OpenThread task.

## 2. Facts this design rests on

Checked in this repository and in IDF 6.1's OpenThread:

- `br_core.c` calls `esp_openthread_cli_init()` at start in every build. IDF's output callback
  (`esp_openthread_cli.c`) prints every CLI output to stdout with `vprintf`, except the `"> "` prompt, which it turns
  into a notification to a task handle private to IDF (`s_cli_task`).
- Test builds register IDF's `ot` command on the USB console (`br_core_test.c`,
  `esp_openthread_cli_console_command_register`). It posts the line to the OpenThread task queue, then blocks with
  `portMAX_DELAY` until IDF's own output callback sees the prompt. Replacing IDF's output callback therefore hangs
  IDF's `ot` command forever: it has to be replaced too.
- `esp_http_server` never sends WebSocket pings; it only answers a client's ping, and browsers send none. It does
  have `httpd_sess_update_lru_counter`.
- `br_core_ot_session_begin` succeeds in SNIFFER mode (OpenThread runs there); only the mode tells a capture apart.
- The CLI answers every command with `Done` or `Error <n>: <text>`, then prints the `"> "` prompt. An asynchronous
  command (one that returns `OT_ERROR_PENDING`, such as `dns browse` or `ping`) prints its results later from
  callbacks, then `Done`.
- While a command is pending, OpenThread's CLI **silently drops** every new line except `factoryreset`
  (`Interpreter::ProcessLine`, `cli.cpp`): no output, no prompt.
- The maximum line is `OPENTHREAD_CONFIG_CLI_MAX_LINE_LENGTH` (384 bytes including the terminator, 640 in reference
  device builds); a longer line is a parse error.
- WebSocket support is off in the build (`# CONFIG_HTTPD_WS_SUPPORT is not set`).
- The vendored web server runs with `max_open_sockets = 7` (`ESP_BR_WEB_MAX_OPEN_SOCKETS`, patch 21) and
  `lru_purge_enable = true`: when a new connection arrives and all sockets are taken, the least recently used one is
  closed. An idle WebSocket is a likely victim.
- The OpenThread session rule (`br_core.h`): every task other than the OpenThread task opens
  `br_core_ot_session_begin/end` around its OpenThread access; a push waits up to 45 s for open sessions and is refused
  if they stay open.
- Thread operations are guarded by mode (`esp_br_guard.c`): SNIFFER answers 409, a stopped stack 503.

## 3. Decisions

### What the user sees

- A new page **Thread Console** (`/console.html`) under Management in the nav, after Tools. Layout as in the mockup:
  - header row: status pill (Connected / Connecting… / Disconnected / Off / Thread not running), network name, role
    and RLOC16, a **Connect / Disconnect** button, then Clear, Copy all, Save log (a "⋯" menu on a phone);
  - terminal: dark, monospace, newest output at the bottom, long lines scroll sideways (tables keep their columns);
    commands, normal output, late output, errors, `Done` and board notices in distinct colours;
  - input: `>` prompt, Enter runs, ↑/↓ history (a history button on a phone), a leading `ot ` is accepted and
    stripped so commands pasted from C6 sessions work; Run button;
  - quick-command chips: `state`, `netdata show`, `router table`, `child table`, `leaderdata`, `srp server host`,
    `dns browse _matter._tcp.default.service.arpa`;
  - footer: the `websocat` command line with this board's address.
- **Risky commands ask first, in the web page only**: `factoryreset`, `reset`, `thread stop`, `ifconfig down` and any
  `dataset …` that changes something (everything except `dataset`, `dataset active`, `dataset pending` and their
  `-x` forms) open a confirm dialog (a bottom sheet on a phone) saying what the command does to the network. `websocat`
  gets no confirmation.
- **Off by default.** While off, the page shows the Off state with a **Turn on the console** button; turning it on
  stores the setting. Turning it off closes any open session.
- **Connecting on open (my call):** the page connects by itself when it opens if the console is on and no session is
  open. If another session is open, it shows Disconnected with "In use by <address>" and connects only when the user
  presses Connect (which takes over).
- **One session at a time, newest wins.** The CLI has one output, so a second connection takes over: the old one gets
  a notice saying who took over, then is closed. Disconnect frees the console for `websocat` without closing the tab.
- **Thread not running** (SNIFFER, stopped, radio fault, a push in progress): the connection stays open, commands are
  refused with a notice giving the mode, and the console works again by itself once OpenThread runs.
- **Board restarting or unreachable:** the page reconnects with a back-off (1 s, 2 s, 5 s, then every 5 s), keeps
  the scrollback, and marks a command that was still waiting for output as lost, never repeating it.

### Protocol

- Endpoint: `GET /device/console/ws`, upgraded to a WebSocket. Text frames only.
- Client → board: one frame is one command line (UTF-8, no newline; a trailing `\r`/`\n` is stripped). Longer than
  the CLI's maximum line: refused with a notice, not sent to the CLI.
- Board → client: CLI output as plain text, exactly as the CLI prints it, line breaks included; the prompt is not
  sent. Board notices are separate frames whose text starts with `# ` (the CLI never starts a line that way), for
  example `# Connected to esp-otbr-d2cafc (OpenThread 1.4, router 0x6800)`,
  `# Thread is not running (mode SNIFFER)`, `# Busy: "ping …" is still running`,
  `# Closed: a new console session opened from 192.168.10.23`. Plain text keeps `websocat` readable without a
  client.
- Close codes (4000–4999 range, reason text = the notice): 4001 taken over, 4003 console turned off, 4009 too many
  bytes queued (see Back-pressure). A connection to a console that is off gets 4003 right after the upgrade.
- **Busy handling (board side):** the board tracks whether a command is pending: from posting a line until the CLI
  prints its `"> "` prompt (the prompt, not `Done`/`Error` parsing, ends a command). A new line while one is pending
  is answered with the Busy notice instead of being passed to the CLI, where it would vanish. Exception:
  `factoryreset`, which the CLI itself always accepts. The pending flag is cleared whenever OpenThread stops or
  restarts (a push, ROM recovery), so a command lost with the stack never leaves the console stuck on Busy.
- **Liveness (my call):** the board sends a WebSocket ping every 10 s and closes the session (code 1001, notice
  "no answer") when no pong arrives for 30 s. Each pong refreshes the socket's LRU stamp
  (`httpd_sess_update_lru_counter`), which is also how the console survives LRU purge. A sleeping laptop therefore
  frees the console within about 30 s instead of leaving a stale "In use".

### Settings API

- `GET /device/console` → `{"enabled": false, "session": null}` or
  `{"enabled": true, "session": {"peer": "192.168.10.23", "since_s": 812}}` (`since_s`: seconds since it connected).
- `PUT /device/console` with `{"enabled": true|false}` → the same body as GET. Stored in NVS (namespace and key are
  the builder's call, following `netnames`/`netcfg`). Turning off closes the session with 4003.
- Both work in every mode (guard class NONE): the setting is not a Thread operation.
- **Interfaces (user's decision):** the settings routes and the WebSocket endpoint belong to the existing
  `NETCFG_SVC_DEVICE_API` service class, like every other `/device/*` route (patch 12): in split mode only the
  management interface reaches the console; in single mode, as now, everything does. No new service class.

### Board side

- A new component (name is the builder's call, e.g. `ot_console`) owns: the setting, the single session, the pending
  flag, the output buffer and the WebSocket handler, which the web server registers like the other device routes.
- **Output path.** The component installs its own CLI output callback in place of IDF's (it calls `otCliInit` with
  its callback where `br_core` calls `esp_openthread_cli_init` today). Output goes to whichever input issued the
  current command: the WebSocket session, or stdout for the USB `ot` command. Output with no command pending goes to
  the attached session if there is one, else stdout. The callback runs on the OpenThread task and never blocks: it formats into a bounded buffer
  and the WebSocket side sends from it on the web server's own task (`httpd_queue_work` / async send).
- **Input path.** Each line opens an OpenThread session (`br_core_ot_session_begin`), posts the line to the OpenThread
  task queue (as `esp_openthread_cli_input` does), and closes the session. No session stays open between commands, so
  an idle console never holds up a push. If `br_core_ot_session_begin` fails, or the mode is not `BR_RUNNING` /
  starting (SNIFFER, radio fault), the line is refused with the Thread not running notice giving the mode.
- **USB `ot` command (my call).** IDF's `ot` console command is replaced by our own, registered under the same name
  in test builds, going through the same input path and Busy tracking as WebSocket lines (one arbiter for both
  inputs). It waits for the prompt with a timeout (it must never block the console task forever) and prints the Busy
  notice like the WebSocket. While a WebSocket session is attached, it answers "console in use from <address>"
  instead of running. Existing on-board tests that drive `ot` over USB keep working unchanged.
- **Logging.** Each command is logged at INFO on the board log with the peer address (`console 192.168.10.23: thread
  stop`) (my call: the log is where a surprising network change gets traced back).
- **Back-pressure.** Output waiting for a slow client is capped (size is the builder's call, a few KB). When the cap
  is hit, the oldest unsent output is dropped and a `# Output dropped (<n> bytes)` notice is sent in its place; if
  the client stays stalled past a timeout, the session is closed with 4009. The OpenThread task never waits.
- **Sockets.** Enabling `CONFIG_HTTPD_WS_SUPPORT` and keeping the console alive under LRU purge is required: with the
  console open, browsing every page of the web UI in another tab must not close it, and pages must keep loading. How
  (a WebSocket ping that refreshes the socket's LRU stamp, a reserved socket, raising the socket count) is the
  builder's call; any change to the vendored server goes into `PATCHES.md`.

### Page

- `console.html` plus `static/console.js`, split like `leader.js`: a pure part (line handling, `ot ` stripping, the
  risky-command rule, notice parsing, history, reconnect back-off) testable in node, and the page part (DOM,
  WebSocket).
- The page's mode guard leaves the console alone (`data-guard-exempt`), as with the Leader card: the board reports
  Thread not running itself.
- The page budget in `test_device_pages.py` stays as it is; if the page does not fit, that is raised as a question,
  not quietly changed.

## 4. Testing

- Host tests (the existing `test_host` pattern) for the board's pure logic: line validation and stripping, the
  pending/busy state machine (including `factoryreset` passing while busy), Done/Error detection across output
  fragments, notice text, session arbitration (newest wins, turning off closes), and back-pressure dropping.
- Node tests for `console.js` pure logic: risky-command rule (each listed command, `dataset` read-only forms not
  risky), `ot ` prefix, history, back-off sequence, notice parsing into pill states.
- On-board pytest (`p4/pytest/test_console.py`), with a Python WebSocket client:
  - off by default: the upgrade is closed with 4003; `PUT` on, then it connects;
  - `state` returns the role and `Done`; `dns browse _meshcop._udp.default.service.arpa` returns this board's own
    meshcop record after the pending phase;
  - a second line while `ping` is pending gets the Busy notice;
  - a second client takes over: the first gets the notice and 4001;
  - SNIFFER: a line gets the Thread not running notice and the connection stays; after the capture, `state` works;
  - with the console open, loading every web page in turn leaves it open (the LRU requirement);
  - turning the setting off closes the session with 4003.
- Docs: a Thread Console section in `docs/api.md` (endpoint, frames, close codes, settings, websocat example) with a
  screenshot, a `CHANGELOG.md` entry, and the README feature list.

## 5. Left to the builder

- Component and file names, NVS namespace and key, buffer sizes and the stall timeout.
- How the WebSocket survives LRU purge (within the requirement above).
- Whether `ESP_BR_WEB_MAX_OPEN_SOCKETS` changes, with its RAM cost stated.
- Exact colours and copy within the mockup's look.

## 6. Risks

- **Silent drops.** If the board's pending tracking and the CLI's disagree (a command the CLI treats as pending that
  the board does not, or the reverse), lines vanish or are refused wrongly. Done/Error detection must cover every
  command's ending, including `* Error` lines from internal debug commands.
- **Socket pressure.** One more long-lived socket on a 7-socket server; the LRU requirement is the guard.
- **Risky commands from `websocat`.** No confirmation outside the page; the INFO log line is the trace.
- **Output volume.** `help` and big tables on a large network produce several KB at once; the cap and the drop
  notice keep the OpenThread task safe at the cost of truncated output on a stalled client.
