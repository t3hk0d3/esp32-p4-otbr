# Logging: channels, levels and a live log page

Date: 2026-11-10. Status: design, awaiting the user's approval.

## Why

The board has no logging story. ESP-IDF lines go to the UART (CH343) at Info and nothing else; Debug and Verbose
calls are compiled out (`CONFIG_LOG_MAXIMUM_LEVEL=3`); ziggurat's Rust tracing goes nowhere. Debugging the Zigbee
pairing problem (2026-11-09) needed a spike (`spike/ziggurat-log` 90c1e6d, submodule branch `spike/p4-tracing-log`
f727f27, both local only) just to see ziggurat's log. The next investigation should need no spike: every part of the
firmware (ESP-IDF components, OpenThread, ziggurat) ends up in one log stream, the part being chased can be turned up
on its own, and the log can be read live in the browser or later from a syslog server.

## Goals

1. **Four channels**, each with its own level:
   - **UART**: the CH343 console (header pins 8/10, GPIO37/38), as today.
   - **USB-JTAG**: the P4's USB-Serial-JTAG peripheral on header pins 27/28 (GPIO24/25, the full-speed USB PHY),
     reached with a USB breakout on the 40-pin header. The USB dongles use the high-speed OTG port
     (`radio_mux/usb_transport.c`, `peripheral_map 0`), so the two do not conflict.
   - **Syslog over UDP** to one target.
   - **Live log over WebSocket**: only for connected clients. No client, no work (no buffering and no formatting for
     it); with clients, every line fans out to each of them.
2. **Per-component levels** (component = ESP-IDF log tag, plus `OPENTHREAD` and `ziggurat`), with a default for every
   tag not listed.
3. **Combination rule (user):** a line goes out on a channel only if it passes both its component's level and that
   channel's level. Example: ziggurat Debug + syslog Warning → syslog gets ziggurat's warnings and errors; a Debug
   viewer gets its debug lines.
4. **Web UI**: one page, Device → Logs (details below).
5. Settings are stored and survive a reboot.

## Non-goals

- No log history on the board (user: "we don't have luxury of keeping logs; we have syslog for full history"). A
  small transit buffer between the producer and the channels is fine; nothing is kept for later reading, and the live
  view shows only lines from the moment the client connected.
- No TCP or TLS syslog, no more than one syslog target.
- No logs from the C6 (RCP) or the USB dongles.
- No on-board time sync here: that is the parallel RTC/NTP feature; logging only reads its "clock valid" flag.

## Anti-goals (would be a failure even if it works)

- Logging stalls or slows the OpenThread task, the zigbee task, lwIP or an ISR, because a socket, the USB-JTAG host or a
  viewer is slow or absent. Lines are dropped and counted instead.
- A feedback loop: sending a line to syslog or a viewer produces new log lines that are sent again.
- Lines from different tasks interleave into garbled lines on a channel.
- The live log costs anything while nobody watches.
- The UART loses lines it gets today: the default UART output at Info must stay as complete as it is now, crash and
  early-boot output included.

## Decisions taken with the user

- Channels: UART, USB-JTAG, syslog (UDP), live WebSocket. JTAG stays: the board brings it out on the GPIO header.
- Per-channel **and** per-component levels, combined as in Goal 3.
- Syslog is UDP only, configured as **one field** `host[:port]`; the port defaults to 514.
- Live view: only lines after you connect.
- **No on/off switch for the live log** (user): no connected WebSocket client means no logs; connected clients get the
  fan-out. (The Thread Console's switch exists because it runs commands; the log is read-only.)
- The live log's level is **not** a channel row in the settings; it belongs to the live view (see Web UI).
- Web UI is a **single page**. Settings cards fold; the live view has a **Fullscreen** button.
- ziggurat's log hook ships as a **build-time patch now**, with an **upstream issue and PR** (see Constraints).
- USB-JTAG is built and host-tested; on-board testing waits for a header breakout.

## Calls made by Claude (the user can overturn them)

- **Live level per viewer.** Each WebSocket client chooses its own level (the picker in the Live card's toolbar, or a
  parameter for `websocat` users). One viewer at Debug does not change what another gets.
- **Live log access** follows the device API's per-interface policy (as `/device/console/ws`): refused with 403 on an
  interface that does not serve the device API. Otherwise open, like the other read-only pages.
- **Syslog format:** RFC 5424 over UDP, one line per datagram. Severity from the level (Error 3, Warning 4, Info 6,
  Debug and Verbose 7), facility `local0`, HOSTNAME = the board's hostname, APP-NAME = the tag (so a receiver can
  filter by component), TIMESTAMP from `time()` (UTC, ms) once the clock is valid, NILVALUE before (the receiver
  stamps arrival), the board's uptime in ms at the start of MSG. Clock validity comes from the RTC/NTP feature being
  designed in parallel (session "RTC/NTP synchronization", 2026-11-10): it provides a cheap non-blocking "clock
  valid" query (`timesync_clock_valid()`, wired in `app_main.c` since the merge with time sync). No back-dating (nothing is stored). UART, JTAG and live lines keep
  ESP-IDF's uptime ms (`CONFIG_LOG_TIMESTAMP_SOURCE` unchanged). The target may be an IPv4 address, `[IPv6]:port`, or a hostname.
- **Defaults** (a fresh board behaves as today): UART Info, USB-JTAG Info, syslog Off with no target, component default
  Info, no component overrides.
- **Debug ceiling:** test builds only (user decision 2026-11-10, after implementation). `make build-test` keeps Debug
  and Verbose calls (`CONFIG_LOG_MAXIMUM_LEVEL` Verbose, from `p4/sdkconfig.defaults.test` into its own
  `p4/sdkconfig.test`; +49 KB of app); a release build stays at Info. The ceiling is `logging_level_ceiling()`, shown
  as `max_level` in `GET /device/logging`: levels above it are refused (PUT, console, live viewer), stored ones are
  lowered to it at boot, and the Logs page offers only levels up to it. Runtime levels still default to Info.
- **OpenThread's level follows its component level**: the `OPENTHREAD` component level (min with the highest channel
  level) is set with `otLoggingSetLevel`, so OpenThread does not format lines nobody will see.
- **ziggurat's tracing** joins the stream under tag `ziggurat`, done properly from the spike (lock-free ring because
  ziggurat logs with interrupts masked on one hart; drained by a task). Its level follows the `ziggurat` component
  level and is set on the zigbee task (ziggurat's critical sections only hold on one hart). The spike's `zlog` command
  is not carried over.
- **Known tags:** the board remembers the tags it has seen since boot (bounded set) so the UI can offer them when
  adding a component row. Component rows are capped (32) and stored.
- **UART console command** `log` shows and sets the same settings (channel levels, component levels, syslog target),
  for when the network is down.
- **Drops are counted per channel** and shown on the page (and in `GET /device/logging`). Syslog send errors are
  counted, never logged (anti-goal: feedback loop).
- **Prometheus:** per-channel dropped and sent counters on `/metrics`.

## Constraints from the code base

- **Styleguide** (`STYLEGUIDE.md`): component prefix (`logging_` for a new component), `esp_err_t` returns, host tests
  in `<component>/test_host/` with `TEST_CASE("<scenario>: <effect>", "[logging]")`, target tests in
  `p4/pytest/test_logging.py`, `make` targets for routine tasks, docs in `docs/` (new `docs/logging.md`, plus
  `docs/api.md`, `docs/README.md`, `CHANGELOG.md`).
- **Task rule:** code on the OpenThread task or the esp_timer task never calls lwIP, directly or through
  `httpd_queue_work`. Log calls come from every task, including those two: the tap only copies into a buffer and
  notifies; a separate task sends to sockets.
- **The tap point.** ESP-IDF v6.1 log v1. `esp_log_set_vprintf` receives text without the level or tag; the tag level
  check (`esp_log_level_set`) happens before it. A line can arrive in several calls: OpenThread's port
  (`components/openthread/src/port/esp_openthread_logging.c`) writes prefix, body and `\n` as three `esp_log_write`
  calls, and other tasks may log in between. The builder must get each complete line with its level and tag
  without garbling; how (prefix parsing, per-task assembly, wrapping `esp_log_write`, log v2) is the builder's
  choice, but it must hold for OpenThread's three-part lines and for ziggurat's lines.
- **Generation gate.** ESP-IDF's per-tag level should be set to min(component level, highest level any channel or
  viewer currently wants), so lines no channel accepts are never formatted. It is recomputed when a setting changes
  and when a viewer connects, disconnects or changes level. With no viewer connected, the live level counts as Off.
- **UART output must stay synchronous enough** that lines right before a crash still appear (today's behaviour). Panic
  output does not go through the log and is unaffected.
- **USB-JTAG today** is enabled as the secondary console (`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`, inherited),
  so stdout already mirrors there. Giving it its own level means the log path writes to it separately, non-blocking,
  dropping while no host reads. What happens to the secondary console (keep it for console input/output, or turn it
  off) is the builder's call, but the console must keep working on the UART.
- **Sockets:** `CONFIG_LWIP_MAX_SOCKETS=24` is an exact sum (comment in `p4/sdkconfig.defaults`, checked by
  `routing/task.c`); the syslog UDP socket adds one, and the comment and check change with it. Live viewers use the web
  server's sockets (`max_open_sockets` 7, shared with every page).
- **URI handler slots:** new routes need `EXTRA_HANDLER_SLOTS` in the vendored `esp_ot_br_server/src/esp_br_web.c`
  raised, logged in `p4/components/PATCHES.md`.
- **Web partition** (`web_0`/`web_1`, 2 MB SPIFFS each) is tight (zigbee.html was deleted for space): measure the new
  page's size.
- **WebSocket pattern:** `device_api/console_ws.c` (static slots, ping every 10 s, close after 30 s without pong or
  15 s without draining, the forwarding task that keeps the OpenThread task off lwIP) is the model for the live log.
  The live log differs in that it has many clients, never blocks the producer, and drops lines for a slow client
  instead of closing it at once.
- **Hostname resolution** for the syslog target runs off the logging hot path and never on the OpenThread task.
- **ziggurat's Rust side** (`crates/ziggurat-ot/src/log.rs`, ~100 lines in f727f27) lives in the upstream submodule
  (`zigpy/ziggurat`, pinned at f917f98); f727f27 exists only in the `zigbee-request-timeout` worktree's submodule
  (`.git/worktrees/zigbee-request-timeout/modules/p4/components/zigbee/ziggurat`). Where that code lives in the
  product: **user decision 2026-11-10: a patch for now**, kept in this repository and applied by
  `p4/components/zigbee/tools/build_staticlib.sh` like the radium patch (the submodule stays on upstream f917f98),
  **plus an upstream issue and PR** to zigpy/ziggurat offering the log hook (`ziggurat_platform_log` +
  `ziggurat_log_set_level`). The patch is dropped once upstream has an equivalent. The issue and PR texts are shown to
  the user before they are posted.
- **USB-JTAG hardware:** the rig has no breakout for header pins 27/28 yet (the user may find one). The JTAG channel is
  verified by host tests and the build; on hardware it stays unverified unless a breakout turns up before the on-board
  pass. So the secondary-console change must be conservative: the UART console must provably keep working.
- **Hardware budget** (user rule): no flashing inside implementation tasks; one batched on-board pass after review and
  fixes. The board is shared: re-read `/device/status` `app.version` before any on-board action.

## Interfaces (the contract)

REST, under the device API:

- `GET /device/logging` →
  `{"channels": {"uart": {"level": "info"}, "jtag": {"level": "info", "host": bool},
  "syslog": {"level": "off", "target": "" | "host[:port]", "state": "off" | "resolving" | "sending" | "unresolved" |
  "no_socket"}},
  "components": {"*": "info", "<tag>": "<level>", ...}, "known_tags": ["..."], "viewers": <n>,
  "counters": {"<channel>": {"sent": n, "dropped": n}, "live": {"dropped": n}}}`.
  Levels: `off`, `error`, `warning`, `info`, `debug`, `verbose`.
- `PUT /device/logging`: the same shape, settings fields only (`channels.*.level`, `channels.syslog.target`,
  `components`), any subset. Applied immediately, stored. Errors follow the existing device API style (400
  `invalid_request` with the field, 413, 500 `storage_failed`). The builder decides whether `components` replaces or
  merges (state it in `docs/api.md`).
- `GET /device/logging/ws`: WebSocket, text frames, one log line per frame, from the moment of connection. The client's
  level is set at connect (query parameter) and can be changed with a frame; the default is Info. Board notices (lines
  dropped for this client) are marked so the page can show them apart from log lines. 403 on interfaces that do not
  serve the device API.

UART console: `log` (show), `log level <uart|jtag|syslog> <level>`, `log tag <tag|*> <level|default>`,
`log syslog <host[:port]|off>`. Exact syntax is the builder's call.

## Web UI: Device → Logs

A single page in the existing style (nav entry under Device). Agreed mockup: `.superpowers/brainstorm/` `log-page-v2`,
with the syslog field merged and the live row removed as below.

- **Channels card** (folds; folded it shows a one-line summary such as
  `UART Info · USB-JTAG Off · Syslog Warning → 192.168.1.10:514`): rows UART, USB-JTAG, Syslog. Each row: level
  picker, where it goes (UART: baud and header pins; JTAG: header pins 27/28; Syslog: one `host[:port]` input), and a
  status (JTAG host / no host; syslog sending / unresolved / off; drop counts when not zero).
- **Components card** (folds; summary `everything else Info · ziggurat Debug · OPENTHREAD Info`): the default row,
  one row per override with a level picker and remove, and an add row whose input suggests known tags.
- **Live card** (user, 2026-11-10 sketch review): **disconnected when the page opens**; a status pill
  (Disconnected / Connected) with a **Connect / Disconnect** button next to it, as on the Thread Console. The
  WebSocket opens only on Connect and closes on Disconnect, so merely opening the page costs the board nothing. Then
  the viewer's own level picker, Pause, Clear, Copy all, a text filter, and
  ⛶ Fullscreen (Esc or Exit leaves). Lines are coloured by level as on the mockup. A notice line shows when lines were
  dropped for this viewer. Pausing keeps receiving but stops scrolling, or the builder may pause the stream; either
  way it must not make the board buffer.
- Changes apply immediately (no Save button) unless the builder finds the existing pages use Save; then follow them.

## Left to the builder

- How the tap gets whole lines with level and tag (see Constraints), the transit buffer's size and memory (internal
  RAM vs PSRAM), and the drop policy within the constraints.
- Component layout: a new `p4/components/logging/` with the core (levels, filter, syslog encoding, settings) testable
  on the host, plus the device API handlers in `device_api` like the console.
- NVS layout of the settings.
- The live WebSocket's slot count and per-client queue depth.
- Whether the measured Debug ceiling is Verbose or Debug (report the numbers).
- Test split: host tests for level combination, generation gate, line assembly, syslog encoding, target parsing
  (`host`, `host:port`, `[v6]:port`, bad input), settings parse/store; target tests for the REST API, the WebSocket
  (connect, per-viewer level, drop notice, 403 policy), syslog reception on a UDP listener on the test host, the
  console command, and that the live log costs nothing with no client.
