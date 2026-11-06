# Radios

The board treats every radio as a **channel**: the on-board ESP32-C6 and a USB radio dongle on the USB-A port. Each
channel runs in one **mode**, stored on the board and changed through the [Radios page](#the-radios-page) or
`PUT /device/channels/{id}` ([API](api.md#radio-channels)) without a reboot.

Without a dongle the board behaves as before: the C6 channel in `otbr` mode.

## Channels

| Channel | Id | Notes |
| --- | --- | --- |
| On-board C6 | `c6` | Always listed. Its line is fixed (no baud setting). `off` holds it in reset. |
| USB dongle | `usb-<vid>-<pid>`, plus `-<serial>` when the device reports a serial number | Listed while attached, and afterwards as `unavailable` (not detected) with its settings until you forget it. |

- Ids are lowercase hex (`usb-10c4-ea60-abc123`); characters outside letters and digits are dropped from the serial,
  and an id has at most 47 characters. A stored mode follows the id, so the same dongle gets its settings back.
- One dongle is supported at a time. Two identical bridges without serial numbers share an id; the second cannot be
  opened while the first is attached.
- Supported USB drivers: cp210x, ch34x and plain CDC-ACM. Anything else (FTDI, for example) is listed as `unavailable`.
- A channel's `state` is `idle` (mode `off`), `starting`, `running`, `fault` (with a reason) or `busy` (a C6 job runs).
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

The C6 is not probed this way; the boot probe reports its firmware.

## RCP proxy

A channel in `rcp-proxy` serves its radio to one peer. Two independent settings choose how, both stored per channel
under `rcp_proxy`:

- **Direction**: `listen` (default) waits for one peer on `port` (default 6638); `connect` dials out to `host:port`
  (`host` is an address or a name, `.local` names go over mDNS; a host is required).
- **Adapter**: `tcp` (default) or `udp`.

| | TCP | UDP |
| --- | --- | --- |
| listen | The first client wins; later ones are refused with a reset. TCP keepalive notices a dead client. | The first source that sends becomes the peer. Other sources are dropped and counted until the peer has been silent for 30 s, or you kick it. |
| connect | Retries with back-off 1, 2, 4, 8, 16, then 30 s; the back-off resets to 1 s after a connection that lasted 60 s. A connect that gets no answer gives up after 10 s. | Datagrams go to `host:port`; only datagrams from that address are accepted. |

Listen sockets and UDP sources follow the [network access rules](networking.md) (per-VLAN rule, Thread-mesh refusal).
Ports 80 and 179 are refused.

**Framing.** A USB channel passes bytes as they are. On the C6 the proxy turns spinel frames into HDLC bytes and back,
so a Linux `otbr-agent` (`spinel+hdlc+uart` through `socat`) can drive the C6; when a peer connects (TCP) or first sends
(UDP), the board resets the handshake so the host sees the RCP's reset. For UDP, `framing` is `hdlc` (default: the stream
is cut at HDLC frame boundaries, one frame per datagram, which suits spinel) or `raw` (each received chunk becomes a
datagram of at most 1024 bytes; loss and reordering corrupt the stream, so use it for testing only).

**Kick.** `{"rcp_proxy": {"kick": true}}` drops the current TCP peer or releases the UDP lock at once; it is not stored.

**Behaviour.** A peer that does not read until the buffer overflows is disconnected. Unplugging the dongle drops the
peer; the channel serves again when the radio returns. Changing a proxy setting re-opens the listener or connection.
The channel's counters and the peer's address are in `GET /device/channels` (`rcp_proxy.state`: `listening`,
`connecting`, `backoff`, `connected`). A firmware push or recovery of the C6 is refused with 409 `c6_busy` while its
proxy has a peer.

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
time give 409 `busy`, and so does a mode change while a move runs) and cannot carry baud, flow control or proxy
settings: send them in a separate request. Setting `otbr` on a channel while no channel has it simply starts Thread
there; `off` stops it.

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

## The Radios page

`/radios.html`, first in the Device menu.

- The **table** lists every known channel (unplugged ones greyed out) with firmware, mode, state, activity and a
  Configure button. The head says which radio Thread runs on.
- The **Configure dialog** has the mode cards (a mode that cannot be chosen is greyed out with the reason: not in this
  build, the firmware does not fit, or unplugged), the `rcp-proxy` settings (direction, adapter, host or port, framing for
  UDP) and, for USB, baud and flow control. Settings are saved first, then the mode.
- Choosing Thread border router on a channel while another runs it turns Save into **Move Thread here**. After the 202
  the page polls until the move settles and shows a failure with its reason.
- With no channel in `otbr` the Thread menu is hidden and the Thread pages show a notice linking here. When the OTBR
  channel is only unplugged or faulted, the menu stays and the pages say "no radio in OTBR mode" with its state. Packet
  capture needs a radio in Thread border router mode.

## API summary

| Request | Purpose |
| --- | --- |
| `GET /device/channels` | `available_modes` and every channel |
| `PUT /device/channels/{id}` | change mode, line, proxy settings, or kick; 202 for a move |
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
- **C6 jobs**: a firmware push with OpenThread on the dongle leaves OpenThread alone; with a proxy peer on the C6 it gives
  409 `c6_busy`.
- **Boot**: a stored dongle that is unplugged gives `NO_RADIO` without a fault; plugging it in gives `BR_RUNNING`.
- **Heap**: about 21 KB of internal DMA RAM is taken at the first C6 open; a proxy session takes an 8 KB ring (TCP) plus
  codec buffers.
