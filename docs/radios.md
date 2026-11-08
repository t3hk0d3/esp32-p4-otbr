# Radios

The board treats every radio as a **channel**: the on-board ESP32-C6 and a USB radio dongle on the USB-A port. Each
channel runs in one **mode**, stored on the board and changed through the [Radios page](#the-radios-page) or
`PUT /device/channels/{id}` ([API](api.md#radio-channels)) without a reboot.

Without a dongle the board behaves as before: the C6 channel in `otbr` mode.

## Channels

| Channel | Id | Notes |
| --- | --- | --- |
| ESP32-C6 (built-in) | `c6` | Always listed. Its line is fixed (no baud setting). `off` holds it in reset. |
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
- A channel's `state` is `idle` (mode `off`), `starting`, `running`, `fault` (with a reason) or `busy` (a C6 job runs,
  or the C6's boot steps).
- `DELETE /device/channels/{id}` (the page's "Forget this radio") removes an unplugged dongle and its settings. The C6
  and attached radios cannot be forgotten (409 `in_use`).

## Modes

| Mode | What it does |
| --- | --- |
| `unavailable` | Reported only, never set: the radio is absent, has no driver, or the C6 does not answer. The stored mode is kept and applies when the radio is usable again. Line and proxy settings can still be changed. |
| `off` | Listed, unused. The C6 in `off` is held in reset. |
| `otbr` | The board's Thread border router runs on this channel's RCP. At most one channel. |
| `rcp-proxy` | The channel's bytes go to one peer over TCP or UDP, see [RCP proxy](#rcp-proxy). Works with any firmware. |
| `ziggurat` | Reserved for the Zigbee work. Refused with 409 `not_available` in this build and not offered on the page. |

Defaults: the C6 is `otbr`; a new dongle is `off`; baud 460800; flow control off.

What fits: `otbr` needs spinel (OpenThread RCP) firmware; `rcp-proxy` and `off` fit anything. A PUT of a mode the
detected firmware does not fit is refused with 409 `incompatible`, naming the detected type. OTBR together with a
registered `ziggurat` consumer is refused with 409 `hybrid_not_supported`.

Line settings per USB channel: baud (38400, 57600, 115200, 230400, 460800, 921600, 1000000, 2000000) and flow control
(cp210x only; the other bridges have none).

## Detection at plug-in

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
| OpenThread RCP | `spinel` | the version string, and the chip when the firmware reports it (OllyZig: `CC13XX_CC26XX`) | `otbr`, `rcp-proxy`, `off` |
| Ziggurat-on-stick firmware | `ziggurat` | version | `rcp-proxy`, `off` |
| Z-Stack | `znp` | the ZNP version, chip when known | `rcp-proxy`, `off` |
| Silicon Labs NCP | `ezsp` | the ASH/EZSP version | `rcp-proxy`, `off` |
| nothing answered | `unknown` | none | `rcp-proxy`, `off` |

`unknown` is not an error: the stick may run something else or need a baud outside the list. The baud that answered
becomes the channel's baud if none was set. When a channel runs `otbr`, the version the handshake returns is recorded
again with source `passive`. There is no detection endpoint: unplug and replug, or reboot.

The C6 is not probed this way; the boot probe reports its firmware. It runs at every boot, whatever channel is in
`otbr` (with Thread elsewhere it only updates the C6's status and listing), together with the maintenance jumper's
detection and the automatic ROM recovery. A fault it finds with the C6 in `otbr` ("no rcp firmware", "rcp
incompatible") stays: the C6 is listed unavailable and Thread does not start on it until a push. Until those steps
end, the C6 is listed `busy` ("the C6's boot steps run") and nothing runs on it: its stored mode (`otbr`, `rcp-proxy`)
starts once, after the steps, so a proxy peer never sees the RCP reset by the boot. Should the boot steps never begin
(the network did not come up, so the border router never started), the stored mode starts after 30 s without them.

## RCP proxy

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
follow the [network access rules](networking.md) (per-VLAN rule, Thread-mesh refusal). In split mode a TCP or UDP proxy
client is judged by the route back to it, so a routed client that reached the management address is refused when its
route back leaves on an interface that does not serve the proxy. A connect host that is a name is
looked up in the background: the channel shows `connecting` meanwhile, and the other radio's proxy keeps running.
Ports 80 and 179 are refused. Two channels cannot listen on the same adapter and port: a PUT that would make a second
channel in `rcp-proxy` listen where another one (also an unplugged one) is stored listening is refused with 409
`port_taken`, naming the port and that channel.

**Framing.** A USB channel passes bytes as they are. On the C6 the proxy turns spinel frames into HDLC bytes and back,
so a Linux `otbr-agent` (`spinel+hdlc+uart` through `socat`) can drive the C6; when a TCP client becomes the peer (its
first bytes passed the check in the security notes below) or a UDP peer first sends, the board resets the handshake so
the host sees the RCP's reset. For UDP, `framing` is `hdlc` (default: the stream
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
while its proxy has a peer.

**Security notes.** The proxy has no authentication and no encryption: anyone the [network access
rules](networking.md) let through can drive the radio, so serve it only on a trusted VLAN. A web page in any LAN
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

## Moving Thread

`PUT /device/channels/{id}` with `{"mode": "otbr"}` on a channel while another channel has `otbr` **moves** Thread.
The stored Thread network (dataset) is kept; only the radio changes.

1. The target is probed first, through the channel, while Thread still runs on the old radio. If it does not answer as a
   spinel RCP, the PUT is refused (409 `probe_failed`, with the reason) and nothing changes.
2. The PUT answers **202** `{"moving": true, "moved_from": "<id>"}`. The target shows `starting` until it settles.
3. Open sessions (REST calls, the console) are given up to 45 s to end; then OpenThread stops on the old radio and starts
   on the new one. Thread devices lose the border router for a few seconds.
4. The stored modes change only after the new radio runs: the old channel becomes `off`.
5. **Rollback.** If OpenThread does not start on the target, it is started again on the old radio and the target shows
   `fault` with the reason (`GET /device/channels`). Only if that also fails does the board enter `RADIO_FAULT`.
6. **Reboot.** If OpenThread's own initialisation gets stuck on the new radio (it cannot be abandoned from outside) or
   on the old radio during the rollback, the board reboots and comes back on the old radio; the stored modes are
   unchanged. The fault reason says "rebooting" first.

A move takes up to about a minute, runs as a long operation (an update, a firmware push or a recovery at the same
time give 409 `busy`, and so does a mode change while a move runs, or while the board is still stopping or starting
OpenThread on its radio, e.g. after an unplug) and cannot carry baud, flow control or proxy settings: send them in a
separate request.

A **start** (`otbr` on a channel while no channel has it) and a **stop** (another mode on the channel that has `otbr`)
work the same way, as long operations with **202**, and cannot carry baud, flow control or proxy settings either
(400 `bad_value`): send the new mode's proxy settings in a request of their own, before or after it.

- Start: `{"moving": true, "moved_from": null}`. The target is probed first (409 `probe_failed`, nothing changed), then
  OpenThread starts on it; its mode is stored only once it runs. If it does not start, the target shows `fault` with
  the reason and the board stays `NO_RADIO`.
- Stop: `{"moving": true, "moved_from": "<the channel itself>"}`. Open sessions are given up to 45 s to end, OpenThread
  stops, and only then the new mode (`off` or `rcp-proxy`) is stored and applied; the board becomes `NO_RADIO`. If the sessions do not end, nothing changes: the channel keeps running `otbr`
  with `error` `drain_timeout`, and Thread runs on.
- A channel that runs `rcp-proxy` cannot take `otbr` directly (409 `probe_failed`, "set it off first"): set it `off`
  first. The Radios page does that by itself.

## Unplugging the Thread radio

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

## The Radios page

`/radios.html`, first in the Device menu.

- The **table** lists every known channel (unplugged ones greyed out) with firmware, mode, state, activity and a
  Configure button. The head says which radio Thread runs on.
- The **Configure dialog** has the mode cards (a mode that cannot be chosen is greyed out with the reason: not in this
  build, the firmware does not fit, or unplugged), the `rcp-proxy` settings (direction, adapter, host or port, framing for
  UDP) and, for USB, baud and flow control. Settings are saved first, then the mode.
- Choosing Thread border router on a channel while another runs it turns Save into **Move Thread here**. After the 202
  the page polls until the move settles and shows a failure with its reason. A start and a stop of Thread are followed
  the same way ("Starting Thread on ...", "Stopping Thread on ..."; a refused stop says Thread keeps running there).
  A radio that runs `rcp-proxy` is set `off` before it takes Thread.
- With no channel in `otbr` the Thread menu is hidden and the Thread pages show a notice linking here. When the OTBR
  channel is only unplugged or faulted, the menu stays and the pages say "no radio in OTBR mode" with its state. Packet
  capture needs a radio in Thread border router mode.

## API summary

| Request | Purpose |
| --- | --- |
| `GET /device/channels` | `available_modes` and every channel |
| `PUT /device/channels/{id}` | change mode, line, proxy settings, or kick; 202 for a move, start or stop of Thread |
| `DELETE /device/channels/{id}` | forget an unplugged radio |
| `GET /device/status` | `mode` (including `NO_RADIO`) and `otbr_channel` |

The shapes and every status code are in [HTTP API and web UI](api.md#radio-channels).

## Verifying on a board

The on-board tests are in `p4/pytest/test_channels.py` (the C6 alone) and `p4/pytest/test_dongle.py` (marker
`usb_dongle`, needs the OllyZig and a person at the board: run pytest with `-s -m usb_dongle`). Beyond the tests, the checks that need a logic analyser, a test build or long observation:

- **Gap under load** (C6): an RCP-heavy session for 10 minutes (proxy peer streaming, an energy scan) leaves `rx_errors`
  at or near 0; with a logic analyser on CS and INT, no CS fall comes less than 100 µs after the previous rise (if
  errors appear, raise `C6_GAP_US`).
- **Refused connects** (`test_proxy_connect_refused_soak`, 10 minutes by default): afterwards the board still serves three waves
  of 7 concurrent HTTP requests (every web slot) and the proxy still accepts a client. `/device/diag` has no socket count, so this sees
  exhaustion, not a slow leak below the socket budget; compare the log's back-off schedule (1 to 30 s) by eye.
- **Pool exhaustion**: a proxy peer that does not read: drops are counted and the RCP keeps answering.
- **Reset timing**: from a new peer's connect to the first valid header from the C6 takes 300-500 ms.
- **Quick replug** (under 1 s): one close, then a normal detection; Thread starts once.
- **Start failure after a probe** (wrong baud forced through the test build): Thread back on the C6, the dongle in `fault`;
  with a stuck init, a reboot onto the old radio.
- **Drain timeout**: hold a REST session for more than 45 s during a move: the target shows `fault` `drain_timeout`, Thread
  stays on the source.
- **Silent dongle**: the recovery ladder (reopen, spinel reset), and after three recoveries `RADIO_FAULT` without abort.
- **Dead dongle after a reset** (the CH340 OllyZig that stops answering after a reopen), OpenThread on it: one reopen,
  then `RADIO_FAULT` "RCP did not answer its reset", no more reopens in the log; a PUT of `otbr` on the channel starts it
  again, and an unplug and replug detects it again. The "no answer after 3 resets" give-up is checked only with a
  consumer that resets on its own timeouts (Zigbee mode, on the merged branch). `uart_errors` counts, and at most one
  CH34x warning per 10 s, with no "Unsupported notification type" lines.
- **USB reopen under traffic** (`test_usb_reopen_under_traffic_survives`, test build: the `usb_reopen` console command):
  120 reopens while the dongle streams answers: no abort (PATCHES.md patch 33) and the radio answers again (the first
  request after the last reopen may be lost, a cut outgoing frame). Checked 2026-11-07. The CH340 never stayed silent
  after a reopen on the bench, also after forced overruns (`usb_stall <ms>` masks a core's interrupts) and with spinel
  resets in between (about 400 reopens); `ch34x diag|init|clear` (test build) read the chip's registers and send the
  open-time init that ESP-IDF's ch34x driver skips, if it ever does. `usb_reopen` runs the reset path of a consumer's
  reset (only on an attached or leased dongle; a failed reopen detaches it, and three in a row with nothing received
  give up with "no answer after 3 resets"), so the test sends a burst the dongle answers between two reopens.
- **C6 jobs**: a firmware push with OpenThread on the dongle leaves OpenThread alone, also while the dongle captures or
  starts (the board mode is the dongle's); unplugging the dongle during it gives `NO_RADIO` at once; with a proxy peer on
  the C6 it gives 409 `c6_busy`.
- **Boot**: a stored dongle that is unplugged gives `NO_RADIO` without a fault; plugging it in gives `BR_RUNNING`.
- **Heap**: about 21 KB of internal DMA RAM is taken at the first C6 open, about 39 KB (pool, buffers, 16 KB ring) at
  the first dongle open (never on a board without a dongle); a proxy session takes an 8 KB ring (TCP) plus codec
  buffers. Compare the free and the minimum free internal heap (`GET /device/diag`) after boot with no dongle against
  v0.4.0 on the same board: at most 72 KB less (the seven new tasks, below, the queues, the C6 buffers and the
  proxy's resolver). Measured 2026-11-07 (build 2f3b159, 60 s after boot): 284 KB free with no dongle against 350 KB
  on the v0.4.0-era master test build, 65 KB less; 238 KB with a dongle plugged in. The budget was 40 KB before the
  first measurement and was raised to the measured figure plus headroom.
- **Task stacks** (not shrunk: no high-water marks measured yet): `radio_mux` 6144, `usb_lib` 4096, `USB-CDC` 4096,
  `c6` transport 4096, `rcp_proxy` 6144, `rcp_resolve` 4096, `br_radio` 6144 bytes. Read their high-water marks after
  the checks below (a C6 move, a dongle detection, a proxy with a `.local` connect host) before shrinking any.
- **C6 drops under REST load**: with Thread on the C6 and the REST load of `test_unplug_during_rest_load` (without the
  unplug) for 5 minutes, the C6 channel's `drops` and `rx_errors` stay at or near 0 and pings to a Thread device do not
  get lost more than without the load (compare v0.4.0's open ping-loss note).
- **C6 otbr → rcp-proxy → otbr with an otbr-agent peer** (handles of earlier attaches): `RADIO_MUX_AGENT_TEST=1
  pytest -s -m usb_dongle -k otbr_agent` (on the host network by default: it creates `wpan0` and routes on the host;
  with `RADIO_MUX_AGENT_NET=otbr-p0 RADIO_MUX_AGENT_BOARD=<the board's fdaa:1:: address>` the container runs on that
  macvlan network instead and keeps them to itself; either way it advertises a Border Agent on the LAN while it runs.
  Passed that way 2026-11-07). While the old OpenThread drains, the agent gets its RCP's answers and resets, never the board's,
  and the board's OpenThread does not log frames it did not ask for; afterwards Thread is back on the C6 with the same
  network.
- **Two channels in `rcp-proxy` on the same port**: refused with 409 `port_taken` (`test_proxy_port_taken`); a
  listener held by another service shows `backoff` with `last_error` "Address already in use" on the page.
- **Unplug with the console open** running `channel supported` (and `state`) in a loop while the OTBR dongle is
  unplugged: `NO_RADIO`, never `RADIO_FAULT`, no reboot (uptime keeps growing); the open console's commands get
  answers or errors, never a hang, until the session closes with the teardown.
- **Sustained radio -> host stream** (one-MPS IN transfers, PATCHES.md patches 31 and 32: eight transfers in flight;
  checked 2026-11-07: 44808 of 44808 pipelined spinel replies over the proxy, 0 errors; with one, the CH340 at 460800 under Zigbee load counted `uart_errors` 9 and `rx_errors` 5 in
  6 minutes): `rcp-proxy` raw, or `otbr`, on the CH340 at 460800 baud with a sustained radio -> host stream (a spinel
  `PROP_VALUE_GET` flood, or the dongle's own TX burst test) while the REST load of `test_unplug_during_rest_load`
  (without the unplug) runs for 5 minutes: the channel's `uart_errors`, `drops` and `rx_errors` stay 0. Repeat the
  Zigbee load that showed the overrun. Repeat on the CP2102N if one is at hand.
- **HTTP refusal in TCP listen** (port 6638): `curl -X POST http://<board>:6638/ -d x` is refused (`refused` + 1, an RST
  or FIN for curl) with `to_radio` unchanged; `otbr-agent` through `socat` on the C6 still comes up (the reset replay
  once its first bytes pass); a kick during the hold (a client connected with `nc` that has not sent yet) drops it and
  the next client is served; the preconnect case: a client that connects and stays silent stays `listening` (no peer,
  no C6 reset), a second connection replaces it (the first is closed) and becomes the peer with its first spinel
  bytes, and a silent client that then sends `POST / HTTP/1.1` is dropped (`refused` + 1) with `to_radio` unchanged.
- **Stop with an open session**: hold a REST session longer than 45 s while setting the OTBR channel `off`: the PUT
  answers 202, the channel stays `otbr` running with `error` `drain_timeout`, Thread keeps running.
