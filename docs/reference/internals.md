# Internals

How the board does what the guides describe: the rules behind radio modes, the C6's maintenance, the update check,
the diagnostics figures and logging. You don't need this page to run the board. The API shapes and error codes are in
[HTTP API](../api.md).

## Radios

The guide is [Radios and USB dongles](../radios.md). The API calls a radio a **channel** (`GET /device/channels`);
this section uses the API's words.

### Channels

| Channel | Id | Notes |
| --- | --- | --- |
| ESP32-C6 (built-in) | `builtin-c6` (kind `builtin`; `c6` before) | Always listed. Its line is fixed (no baud setting). `off` holds it in reset. |
| USB dongle | `usb-<vid>-<pid>`, plus `-<serial>` when the device reports a serial number | Listed while attached, and afterwards as `unavailable` (not detected) with its settings until you forget it. |

- Ids are lowercase hex (`usb-10c4-ea60-abc123`); characters outside letters and digits are dropped from the serial,
  and an id has at most 47 characters. A stored mode follows the id, so the same dongle gets its settings back.
- One dongle is supported at a time. Two identical bridges without serial numbers share an id; the second cannot be
  opened while the first is attached.
- Supported USB drivers: cp210x, ch34x and plain CDC-ACM. Anything else (FTDI, for example) is listed as `unavailable`.
- A CH34x bridge (CH340, the OllyZig) reports receive errors on the dongle's UART (overrun, parity, framing) in its
  status packets: each one counts in the channel's `uart_errors` counter, and the log shows at most one warning per
  10 s with the packet's bytes. Rising `uart_errors` with `rx_errors` points at the bridge losing bytes, not the RCP.
  The decoding of the packet follows the ch34x driver's own defines (`CH34X_UART_STATE_TRANSIENT_MASK`, byte 0) and is
  not confirmed against a datasheet; confirm it on a board.
- A channel's `state` is `idle` (mode `off`), `starting`, `running`, `fault` (with a reason) or `busy` (the C6's
  maintenance: its boot steps, a firmware push or a ROM recovery).
- There are two kinds of channel: the board's built-in RCP (`builtin`: the C6 over SPI here, its maintenance done by the
  board) and USB dongles (`usb`). `channel_state` (`absent`, `reinit`, `ready`, `fault`) is the channel's own lifecycle;
  `mode_state` is the state of the mode bound to it in that mode's words (see [API](../api.md#radio-channels)).
- `DELETE /device/channels/{id}` (the page's "Forget this radio") removes an unplugged dongle and its settings. The C6
  and attached radios cannot be forgotten (409 `in_use`).

### Modes

| Mode | What it does |
| --- | --- |
| `unavailable` | Reported only, never set: the radio is absent, has no driver, or the C6 does not answer. The stored mode is kept and applies when the radio is usable again. Line and proxy settings can still be changed. |
| `off` | Listed, unused. The C6 in `off` is held in reset. |
| `otbr` | The board's Thread border router runs on this channel's RCP. At most one channel. |
| `rcp-proxy` | The channel's bytes go to one peer over TCP or UDP, see [RCP proxy](#rcp-proxy). Works with any firmware. |
| `ziggurat` | The board's Zigbee coordinator (Ziggurat) runs on this channel's spinel RCP and serves ZHA, see [Zigbee coordinator](../zigbee.md). At most one channel. Setting it, or leaving it, reboots the board (`"reboot": true`). |

Defaults: the C6 is `otbr`; a new dongle is `off`; baud 460800; flow control off.

What fits: `otbr` needs spinel (OpenThread RCP) firmware; `rcp-proxy` and `off` fit anything. A PUT of a mode the
detected firmware does not fit is refused with 409 `incompatible`, naming the detected type. `otbr` and `ziggurat` may
run at once on different channels; a PUT of `otbr` on the channel that runs `ziggurat` is a reboot that stores the
channel Thread leaves as `off` in the same commit.

Line settings per USB channel: baud (38400, 57600, 115200, 230400, 460800, 921600, 1000000, 2000000) and flow control
(cp210x only; the other bridges have none).

### Detection at plug-in

Every time a dongle appears (plugged in, or attached at boot) the board runs the same steps:

1. The channel starts as `off`. Nothing else uses an `off` channel, so probing cannot disturb a stack or a client.
2. It probes the firmware, in this order, and the first answer wins: spinel at 460800 then 115200; Z-Stack ZNP
   (`SYS_PING`, then the version) at 115200; EZSP (an ASH reset) at 115200 then 460800. The whole detection is capped at
   **3 s**. Bootloaders are never probed.
3. It closes and reopens the device, so probe bytes leave nothing behind.
4. If the detected firmware fits the stored mode, the channel switches to that mode. If not, it stays `off` and keeps
   the stored mode, with a reason such as "stored mode otbr needs spinel; detected znp". A radio reflashed elsewhere is
   checked again at its next plug-in.

What each firmware shows in the Firmware column:

| Detected | Type | Version and chip | Fits |
| --- | --- | --- | --- |
| OpenThread RCP | `spinel` | the version string, and the chip when the firmware reports it (OllyZig: `CC13XX_CC26XX`) | `otbr`, `ziggurat`, `rcp-proxy`, `off` |
| Ziggurat-on-stick firmware | `ziggurat` | version | `rcp-proxy`, `off` |
| Z-Stack | `znp` | the ZNP version, chip when known | `rcp-proxy`, `off` |
| Silicon Labs NCP | `ezsp` | the ASH/EZSP version | `rcp-proxy`, `off` |
| nothing answered | `unknown` | none | `rcp-proxy`, `off` |

`unknown` is not an error: the stick may run something else or need a baud outside the list. The baud that answered
becomes the channel's baud if none was set. When a channel runs `otbr`, the version the handshake returns is recorded
again with source `passive`. There is no detection endpoint: unplug and replug, or reboot.

The C6 is not probed this way; its boot steps report its firmware. They run at every boot, whatever channel is in
`otbr` (with Thread elsewhere they only update the C6's status and listing), together with the maintenance jumper's
detection and the automatic ROM recovery, as the C6's first maintenance (see [Maintenance](#maintenance-reinit)). A
fault they find ("no rcp firmware", "rcp incompatible") stays: the C6 is listed unavailable, Thread on it shows
`RADIO_FAULT` with that reason, and nothing starts on it until a push. Until the steps end the C6 is in `reinit`, listed
`busy` ("the C6's boot steps run"), and nothing runs on it: its stored mode (`otbr`, `ziggurat`, `rcp-proxy`) starts
once, after the steps, so a proxy peer never sees the RCP reset by the boot. The steps run on a task of their own, also
when the network does not come up.

### RCP proxy

A channel in `rcp-proxy` serves its radio to one peer. Two independent settings choose how, both stored per channel
under `rcp_proxy`:

- **Direction**: `listen` (default) waits for one peer on `port` (default 6638); `connect` dials out to `host:port`
  (`host` is an address or a name, `.local` names go over mDNS; a host is required).
- **Adapter**: `tcp` (default) or `udp`.

| | TCP | UDP |
| --- | --- | --- |
| listen | The first client wins; later ones are accepted and closed at once (a FIN, or a reset when they had already sent bytes). A client that opens with an HTTP request is refused too (see Security notes). TCP keepalive notices a dead client. | The first source that sends becomes the peer. Other sources are dropped and counted until the peer has been silent for 30 s, or you kick it. |
| connect | Retries with back-off 1, 2, 4, 8, 16, then 30 s; the back-off resets to 1 s after a connection that lasted 60 s. A connect that gets no answer gives up after 10 s. | Datagrams go to `host:port`; only datagrams from that address are accepted. |

Listen sockets, TCP clients (by the address they came in on and by their own address), UDP sources and connect targets
follow the [network access rules](../networking.md) (per-VLAN rule, Thread-mesh refusal). In split mode a TCP or UDP proxy
client is judged by the route back to it, so a routed client that reached the management address is refused when its
route back leaves on an interface that does not serve the proxy. A connect host that is a name is
looked up in the background: the channel shows `connecting` meanwhile, and the other radio's proxy keeps running.
Ports 80 and 179 are refused. Two channels cannot listen on the same adapter and port: a PUT that would make a second
channel in `rcp-proxy` listen where another one (also an unplugged one) is stored listening is refused with 409
`port_taken`, naming the port and that channel.

**Framing.** A USB channel passes bytes as they are. On the C6 the proxy turns spinel frames into HDLC bytes and back,
so a Linux `otbr-agent` (`spinel+hdlc+uart` through `socat`) can drive the C6; when a TCP client becomes the peer (its
first bytes passed the check in the security notes below) or a UDP peer first sends, the board resets the RCP so the
host sees its reset. Frames the host sends before that reset is done (at most 1 s) are dropped, so the host's own
RESET is answered by the reset's notification. For UDP, `framing` is `hdlc` (default: the stream
is cut at HDLC frame boundaries, one frame per datagram, which suits spinel) or `raw` (each received chunk becomes a
datagram of at most 1024 bytes; loss and reordering corrupt the stream, so use it for testing only).

**Kick.** `{"rcp_proxy": {"kick": true}}` drops the current TCP peer or releases the UDP lock at once; it is not stored.

**Behaviour.** A peer that does not read until the buffer overflows is disconnected. On a USB channel (raw bytes), a
write to the dongle that fails part-way (a USB write timeout) ends the peer session: a TCP peer is disconnected, so
the dongle never sees the rest of its stream spliced onto a cut frame; a UDP peer is let go and its datagrams are
refused for 2 s (counted as refused), so the rest of its cut stream does not follow; after that it, or another source,
locks again (a UDP host gets no signal: its next frames start on a dongle that saw a cut one, another reason UDP raw is
for testing). While the dongle is not attached at all the peer's bytes are dropped and the peer stays. On the C6 a frame that fails is lost as a whole and the peer stays. Unplugging the dongle drops the
peer; the channel serves again when the radio returns. Changing a proxy setting re-opens the listener or connection.
The channel's counters and the peer's address are in `GET /device/channels` (`rcp_proxy.state`: `listening`,
`connecting`, `backoff`, `connected`). When a listener cannot be opened (the port is used by another service) or a
connect fails (the host does not resolve, the access rules refuse it, no answer), the proxy backs off and tries again;
`rcp_proxy.last_error` says why the last try failed, and while it backs off that is also the channel's `error` (the
Radios page shows a warning "Backoff" with it). A firmware push or recovery of the C6 is refused with 409 `c6_busy`
while its proxy has a peer (the proxy refuses to let go: see [Maintenance](#maintenance-reinit)).

**Security notes.** The proxy has no authentication and no encryption: anyone the [network access
rules](../networking.md) let through can drive the radio, so serve it only on a trusted VLAN. A web page in any LAN
browser can open a connection to `http://<board>:6638/` (the port is not on the browsers' blocked list) and send an
HTTP request; in TCP listen the proxy therefore holds the first bytes a new client sends, whenever they come (up to
8, at most 50 ms after the first of them), and refuses the client, counted as refused and logged at most every 10 s,
when they start with an uppercase HTTP method and a space (`GET `, `POST `, `PUT `, `DELETE `, `HEAD `, `OPTIONS `,
`PATCH `, `CONNECT `, `TRACE `). A client that has sent nothing is not the peer: it gets none of the radio's bytes,
the C6 is not reset for it and a firmware push is not refused for it (`c6_busy`), and a new connection replaces it (the
silent one is closed), so a browser's idle preconnected connection cannot lock out the real host. A host therefore
sends first, as spinel, ZNP and ASH hosts do. Nothing of such a request reaches the radio. Radio clients start with
a frame byte (`0x7E` spinel, `0xFE` ZNP, ASH (EZSP) with `0x1A` cancel or the `0xC0` reset; `0x7E` ends an ASH frame),
never with a letter, and pass at their first byte. UDP and `connect` are not checked: a browser can send UDP only
inside STUN/DTLS (WebRTC), not bytes it chooses, and `connect` dials a host you chose.

**One task for both channels.** A single proxy task serves the C6 and the dongle. Work that blocks it holds up the other
channel for that time: resolving a `connect` host (DNS, or mDNS for `.local` names: up to about 2 s per try, again at
every back-off) and writing a large raw chunk to a slow dongle (a 4 KB chunk at 38400 baud takes about 1 s; the write
gives up after its timeout). Prefer an address over a name for `connect`, and a high baud on a raw channel, when both
channels run `rcp-proxy`.

### Moving Thread

`PUT /device/channels/{id}` with `{"mode": "otbr"}` on a channel while another channel has `otbr` **moves** Thread.
The stored Thread network (dataset) is kept; only the radio changes.

1. The target is probed first, through the channel, while Thread still runs on the old radio. If it does not answer as a
   spinel RCP, the PUT is refused (409 `probe_failed`, with the reason) and nothing changes.
2. The PUT answers **202** `{"moving": true, "moved_from": "<id>"}`. The target shows `starting` until it settles.
3. Open sessions (REST calls, the console) are given up to 45 s to end; then OpenThread stops on the old radio. Thread
   devices lose the border router for a few seconds. If the sessions do not end, nothing changes (`drain_timeout`).
4. **Commit.** The stored modes change now, before the new radio starts: the target becomes `otbr`, the old channel
   `off` (both in one store). A move is a rebind: from here on Thread belongs to the target.
5. **No rollback.** OpenThread starts on the target. If it does not start, Thread stays there: the target shows `fault`
   with the reason and the board is `RADIO_FAULT`. Retry with another PUT of `otbr` on the target, or move Thread back
   (another move).
6. **No reboot.** If OpenThread's own initialisation gets stuck on the target (it cannot be abandoned from outside), the
   board shows `RADIO_FAULT` "an earlier OpenThread start was abandoned: reboot" and does not reboot by itself: nothing
   can start OpenThread again until a reboot. A PUT of `otbr` on another channel moves the binding there (its start is
   refused the same way); then reboot, and Thread starts on that channel.

A move takes up to about a minute, runs as a long operation (an update, a firmware push or a recovery at the same
time give 409 `busy`, and so does a mode change while a move runs, or while the board is still stopping or starting
OpenThread on its radio, e.g. after an unplug) and cannot carry baud, flow control or proxy settings: send them in a
separate request.

A **start** (`otbr` on a channel while no channel has it) and a **stop** (another mode on the channel that has `otbr`)
work the same way, as long operations with **202**, and cannot carry baud, flow control or proxy settings either
(400 `bad_value`): send the new mode's proxy settings in a request of their own, before or after it.

- Start: `{"moving": true, "moved_from": null}`. The target is probed first (409 `probe_failed`, nothing changed), its
  mode is stored, then OpenThread starts on it. If it does not start, the target shows `fault` with the reason and the
  board is `RADIO_FAULT`.
- Stop: `{"moving": true, "moved_from": "<the channel itself>"}`. Open sessions are given up to 45 s to end, OpenThread
  stops, and only then the new mode (`off` or `rcp-proxy`) is stored and applied; the board becomes `NO_RADIO`. If the sessions do not end, nothing changes: the channel keeps running `otbr`
  with `error` `drain_timeout`, and Thread runs on.
- A channel that runs `rcp-proxy` cannot take `otbr` directly (409 `probe_failed`, "set it off first"): set it `off`
  first. The Radios page does that by itself.

### Maintenance (reinit)

Work on a radio itself is the channel's `reinit` state: the C6's boot steps, a firmware push and a ROM recovery on the
C6, and a dongle's firmware detection after plug-in. While a channel is in `reinit` no mode may use it: what its mode
sends is refused, its resets are not carried out, and nothing reaches it from the radio.

Before a push or a recovery of the C6, the mode on it is asked to let go of the radio, and the work runs only once it
did:

- **Thread** lets go once its open sessions ended (up to 45 s; the board is `STARTING` meanwhile), then OpenThread
  stops. It refuses while it captures ("a capture is running"), starts ("the border router is starting") or is being
  moved ("OpenThread is being moved to another radio"), and after the 45 s ("OpenThread is busy, try again"): 409
  `busy`.
- **Zigbee** always lets go: it shows `no_radio` "radio reinit: firmware push" (or "ROM recovery") and its client stays
  connected; transmits fail meanwhile.
- **The RCP proxy** refuses while a peer is connected (409 `c6_busy`); otherwise its session and listener close.
- A mode that does not answer within 50 s refused ("<mode> did not let go of the radio in time").

When the work and the C6's boot steps end, the C6 is `ready` and its mode starts again (Thread on the new firmware,
Zigbee replays its radio configuration, the proxy listens again), or the C6 is in `fault` (no RCP firmware, a failed
recovery) and its mode shows why. A refused maintenance changes nothing.

The push and the recovery answer once the mode on the C6 runs again or says it does not, at most 180 s after the work
(`mode_state` shows `starting` until then). A push that installed the image but whose mode did not come back answers
502 `restart_failed` with the mode and its reason, for Thread, Zigbee and the proxy alike; Thread is not forced into a
fault by the wait, it goes on starting. A ROM recovery, and a push of `c6_installer.bin` through ESP-Hosted, answer
with their own result: the mode is `fault` ("no rcp firmware") afterwards by design. After an abandoned OpenThread
start (see [Moving Thread](#moving-thread)) a push does not reboot the board: it answers `restart_failed` with
"an earlier OpenThread start was abandoned: reboot". A push of the C6 while Thread runs on a USB radio leaves that
radio's `rcp_compatible` alone. A reset a mode asks for, and the recovery of a
dongle after a USB error, are not maintenance: the mode gets its usual `RESET`.

### Unplugging the Thread radio

When the dongle that runs OpenThread is unplugged:

- The board mode becomes **`NO_RADIO`** at once. The Thread REST API, the Web GUI forms and the console WebSocket answer
  503 `no radio in OTBR mode`; static pages, `/device/*` and the Radios page keep working. The board does not reboot.
- OpenThread is torn down after the sessions that are open finish (normally immediately; a slow HTTP client can hold it
  for up to 20 s per round, never longer than that client). The channel shows `unavailable`, the stored mode stays
  `otbr`, and the fault reason says the radio was unplugged.
- Replug: the dongle is detected again (3 s), then OpenThread starts on it and the board returns to `BR_RUNNING` with the
  same network.
- There is no automatic fallback to the C6; pick another channel to move Thread there.
- Unplugging a dongle in any other mode drops its proxy peer or stops it, and nothing else changes.
- A dongle that stays plugged in but goes silent gets a close and reopen, then a spinel reset; after three failed recoveries
  the board enters `RADIO_FAULT` with the reason (no abort). A PUT of `otbr` on that channel starts it again.
- In `otbr` a dongle that stays plugged in but does not answer its reset ends in `RADIO_FAULT` "RCP did not answer its
  reset": OpenThread's own recovery tries a spinel reset, then one reopen, then reports the reset failure. A proxy on a
  dongle (`rcp-proxy`) never resets the radio over USB (raw bytes), so it is not affected.
- A dongle's reset (a close and reopen over USB) is not repeated forever for a consumer that resets on its own
  timeouts (the Zigbee consumer, not part of `otbr` or `rcp-proxy`): the third reset request in a row with nothing
  received from the dongle in between (no frame, in a raw proxy no byte) is not carried out. The dongle is closed, the
  channel shows `fault` "no answer after 3 resets", and its consumer is detached as for an unplug. Anything received
  clears the count, as does a new attach. To recover, unplug and replug the dongle (it is detected again, as a new
  device), or PUT the channel's mode again (the same mode: it opens the dongle and attaches its consumer again). The
  C6 is not affected: its reset is the reset GPIO.
- Requests to the radio that are in flight when it goes, or that come before OpenThread is torn down (the console's
  `channel supported`, a REST call), are answered by the board itself: settings "work", reads that can fail fail, the
  channel masks read as 11 to 26, and a recovery that resets the radio completes against those answers instead of
  failing. So an unplug ends in `NO_RADIO`, without `RADIO_FAULT` and without a reboot, whichever task used the radio.
- **Remaining risks.** An unplug during OpenThread's own start (inside its initialisation, before the radio answered
  once) leaves that start abandoned: the board goes to `NO_RADIO`, and when the radio returns it shows `RADIO_FAULT`
  "an earlier OpenThread start was abandoned: reboot" until a reboot. An unplug in the few milliseconds between a radio
  reset and the radio's answer to it, while a task other than OpenThread's runs a recovery, still reboots the board
  (OpenThread aborts the process if that task returns); no such path is known to be reachable.
## C6 base image and ROM recovery

The C6 has a small, frozen program, the **updater**, next to two slots for the radio firmware. The updater, the C6
bootloader and the C6 partition table together are the **base image**; the P4 app carries a copy of it for the
[maintenance jumper](../maintenance-jumper.md).

- The embedded base image version is content-addressed (`base-<8 hex>`, `tools/base_version.cmake`: a hash of the
  updater sources, the shared protocol and board headers, the updater `sdkconfig.defaults`, the C6 partition table and
  the IDF version), so an ordinary P4 update does not touch the C6. Only a P4 release whose base image really changed
  runs the automatic ROM recovery once at boot.
- The recovery erases the C6's `otadata`; the updater then follows the C6 bootloader, which boots the first valid slot
  (`ota_0`, then `ota_1`), so a radio firmware installed by a push into `ota_1` survives. If `rcp.installed` is empty
  afterwards, no valid image was left in either slot: upload `c6_rcp.bin`.
- Each flash region is written and verified up to three times; every retry reconnects to the ROM loader and drops the
  link speed (1.5 Mbaud, then 460800, then 115200 baud), so a noisy wire costs time, not the recovery; the worst case
  is about two minutes.
- The C6 console over the jumper is logged as `c6:` lines, at most 20 per second; a C6 boot loop shows one
  `c6: <n> lines suppressed` line per second instead.
- A radio firmware push: the P4 resets the C6 into its updater, streams the image to it frame by frame (nothing is
  buffered on the P4), the C6 checks it and switches its boot slot only when the transfer is complete, then the C6
  resets. A failed transfer leaves the previous firmware in place. With `from_url` the P4 retries the whole transfer up
  to three times.
- Compatibility of `c6_rcp.bin` is decided by OpenThread when it starts: it rejects a radio firmware whose spinel/RCP
  API version does not match the P4's OpenThread build (`rcp incompatible`). The version string is only informative.

## Self-update

- **The check** reads `versions.txt` of the latest release (`.../releases/latest/download/versions.txt`, about 120
  bytes, no GitHub API): the release tag, the P4 app version and the `c6_rcp` version. The Firmware page starts a check
  when it opens and the last one is older than an hour; nothing runs in the background. The result is cached until the
  next check or reboot, and a failed check keeps the last good result.
- **Comparison.** The first 16 characters of a version (`YYYYMMDDTHHMMSSZ`) are the build time, and versions are
  ordered by it. The release is `newer`, `older` (the board runs a dev build made after the release), `same` (equal
  time and git hash; `-dirty` is ignored) or `unknown`. `newer` and `unknown` offer an install. The radio firmware is
  compared by equality, so an update usually pushes it too.
- **Install order.** A fresh check, then every file from `.../releases/download/<tag>/`, so the files come from the
  release that was checked: the bundle (a failure changes nothing), then the radio firmware, then one reboot. If the
  radio push fails after the bundle was committed, the board still reboots into the new P4 and reports
  `"rcp": "failed"`; the next update pushes only the radio firmware.
- **Trust.** Bundles and radio images are not signed. Trust rests on TLS to GitHub, verified against the certificate
  bundle built into the firmware; the repository is fixed at build time. Certificate expiry dates are not checked
  (mbedTLS is built without time checks, `MBEDTLS_HAVE_TIME_DATE` off).
- **OTA.** The bundle holds the web image and the app image; both are written to the inactive pair, the web image is
  read back and checked against its SHA-256, and only then the boot partition is switched. A new app that does not run
  for 60 s with the HTTP server listening is rolled back by the bootloader. Flash writes use XIP from PSRAM, so traffic
  keeps flowing during an update.

## Diagnostics

- `GET /device/diag` is the snapshot of a sampler that runs once a second. Tasks beyond the 48 it tracks are left out
  (one log line). If the sampler cannot start, the boot goes on and `/device/diag` answers 503 `not_started`.
- The OpenThread lock statistics cover everything that takes the lock through `esp_openthread_lock_acquire/release`
  (libopenthread, `br_core`, the Thread REST server). The border router internals in the prebuilt
  `libopenthread_br.a` only use the task-switching lock, which is not counted: their holds show up only as wait time
  of the others.
- The chip temperature sensor runs in its most accurate range, −10 to 80 °C (about ±1 °C), and is read once a second
  without a lock (ESP-IDF's read masks interrupts and can switch ranges with a 300 µs wait, which disturbed a USB
  dongle). It moves in 1 °C steps on a given board even though it is shown with one decimal.
- Tasks are pinned: core 0 for the network path (lwIP, Ethernet, OpenThread, mDNS), core 1 for management (HTTP,
  workers, `rcp_sync`, console).
- A supervisor reboots the board when the OpenThread lock is not free for 2 minutes or the HTTP server task does not
  answer for 2 minutes (console: `supervisor: the OpenThread lock is stuck` or `... the HTTP server task is stuck`). The
  core dump is kept in the `coredump` partition.

## Logging

The guide is [Logging](../logging.md); the API is in [HTTP API](../api.md#logging).

### The path of a line

- Every ESP-IDF log call passes through the logging component (it wraps `esp_log` and `esp_log_va` at link time), and
  OpenThread's lines are formatted whole and handed to the same path (tag `OPENTHREAD`), as are Ziggurat's (`ziggurat`).
- ESP-IDF's per-tag level is the **gate**: a tag's component level, capped by the highest level any channel or viewer
  wants. A line above it is not formatted at all, which is why `debug` and `verbose` cost nothing while nobody asks.
- A line at or below the UART level goes to ESP-IDF's own UART writer, synchronously and unchanged. A line some other
  channel wants is formatted once into a 16 KB transit ring in internal RAM, and the drain task (`log_out`, core 1,
  below the HTTP server's priority) writes it to USB-JTAG and syslog and queues it for each live viewer.
- Lines logged in a constrained context (an interrupt, the flash cache off, the scheduler suspended or not running) and
  before the capture starts go to the UART only.
- OpenThread's own log level follows the `OPENTHREAD` component, and Ziggurat's follows `ziggurat`, so neither formats
  lines nobody will see. OpenThread's lines on the UART now read `I (123) OPENTHREAD: ...`, like every other tag.
- A line is cut at 512 bytes, 256 for OpenThread, on a UTF-8 character boundary, marked `…[cut]`.
- Each viewer has an 8 KB queue in PSRAM. Live log frames are always valid UTF-8: an ill-formed byte sequence in a line
  is replaced by U+FFFD, as RFC 6455 clients close the connection on anything else.

### Feedback loops

Lines that sending a line produces must not be sent again.

- The drain task, a live log send and the syslog lookup run as **sinks**: whatever they log themselves (lwIP, the
  USB-JTAG driver, the web server's send path) goes to the UART only.
- Some lines come from outside a sink but are still caused by the sends: the web server's dispatch around a send, and
  the Ethernet driver's `receive len=` line for every received frame (the TCP ACK of a WebSocket frame, the ICMP
  answer of a syslog datagram to a closed port). Syslog and the live viewers therefore get the **network-path tags**
  at `info` at most: the tags starting with `httpd` (the web server), `esp.emac` (the Ethernet MAC and its DMA),
  `esp_eth` (the Ethernet driver and its netif glue), `esp_netif` (esp_netif, its handlers and bridge glue) and `lwip`
  (lwIP's own debug output, when it is built to go through the logger). The UART and USB-JTAG are not capped.
- The capture applies the same cap, so such a line is not even formatted when only syslog or a viewer would take it.

### USB-JTAG

The USB-Serial-JTAG driver is installed at start with a 1 KB TX buffer and written without waiting. A host counts as
connected while it sends USB start-of-frame packets (a few ticks without one and it is gone). While no host is there,
the JTAG channel is left out of the capture, so its lines are neither formatted nor counted as dropped; a line already
in the ring when the host leaves is skipped uncounted. A host that is there but does not read fills the TX buffer:
those lines are dropped and counted. If the driver does not install, one error line at boot says so and the channel
stays silent.

### Syslog

- The target is parsed once per save; an IP address is never looked up. A name is looked up by a short-lived task
  (`log_dns`, 4 KB stack, gone after the lookup), never on `log_out`: a DNS timeout must not hold up USB-JTAG and the
  viewers. The drain only checks for the answer, and starts the next lookup when one is due.
- States: `off`, `resolving` (the first lookup runs), `sending`, `unresolved` (the lookup failed and there is no
  earlier address), `no_socket` (resolved, but lwIP had no UDP socket to give). A failed lookup and a missing socket are
  tried again after 5 s, doubling to 60 s, counted from the end of the last try. A name that resolved is looked up
  again every 10 minutes and after every save; a failed refresh keeps sending to the last address. A save that changes
  the target, or turns syslog off, while a lookup runs drops that lookup's answer.
- A send error drops the line and counts it; it never logs (that line would be sent again).
- Severity: error 3, warning 4, info 6, debug and verbose 7. HOSTNAME is the board's hostname. The TIMESTAMP is UTC with
  milliseconds once the clock is set, NILVALUE before.
- The syslog socket is one of the board's lwIP sockets and UDP PCBs; both limits are summed in `p4/sdkconfig.defaults`.

### Counters

`sent` and `dropped` per channel, plus `live`, are in `GET /device/logging` and the
[metrics](../metrics.md#logging-metrics). A line the full transit ring refused is counted against each channel that
wanted it, and every open viewer gets a `# dropped <n> lines` notice for it. A viewer whose queue is full loses lines
on its own and is told the same way. Counters restart at 0 on reboot.

### Known limitations

- **A producer stuck mid-line stalls the other channels.** A record in the transit ring is reserved, then formatted,
  then committed. Until the oldest reserved record is committed, the drain cannot take any later one, so USB-JTAG,
  syslog and the live log stop until it commits. The window holds only the formatting of one line (no lock, no wait),
  but a task preempted inside it for long, by higher-priority tasks keeping its core busy, holds the others up as long.
  The UART is not affected: it is written before the ring.
- **The Thread Console's `loglevel` command bypasses the settings.** It calls ESP-IDF's `esp_log_level_set` directly.
  The gate of that tag then no longer matches the settings: lines can be filtered out that a channel wants, or
  formatted that none does, and the Logs page does not show it. The next save of the logging settings (Logs page,
  `PUT /device/logging`, `log`) sets every gate again.
- The capture's routing decision reads the channel levels from one atomic word; a tag's gate is ESP-IDF's per-tag
  level, which ESP-IDF looks up under its own lock (waiting at most 10 ms).
