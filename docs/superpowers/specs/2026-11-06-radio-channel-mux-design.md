# Radio channel mux: every radio is a channel: design

Status: draft for review, 2026-11-06, after a builder check. Research: five research agents (OpenThread integration,
transports, Ziggurat, prior art, firmware coupling), plus the Ziggurat session's input. The consumer API (section 3,
"Frames, events and copies") was reviewed and agreed with the Zigbee session. Spike: branch `spike/dongle-rcp` (throwaway).
Builds on `feat/usb-serial-proxy` (not released). Companion design: `docs/superpowers/specs/2026-11-06-zigbee-mode-design.md`
on `feat/zigbee-mode`, which consumes this one.

> **Zigbee is not part of this implementation.** The Zigbee stack, its spinel client, its task, its pages, its API
> fields and its tests belong to the Zigbee work, which another agent builds on `feat/zigbee-mode`. This design
> implements only the architectural placeholders that work plugs into (section 3, "Ziggurat mode: placeholder").
> Everything said here about Ziggurat describes that seam, not code this implementation writes.

## 1. Purpose and scope

Today the board has exactly one radio, the on-board C6, and almost every layer knows it. This design turns radios into
**channels**: the C6 is one channel, and a USB radio dongle on the USB-A (OTG) port is another. Each channel runs in
one **mode**:

- **Unavailable**: not detected or not compatible; reported by the board, never chosen.
- **Off**: the channel is listed but unused.
- **OTBR**: the board's own OpenThread border router runs on that channel's RCP.
- **Ziggurat**: the board's on-board Zigbee stack (zigpy/ziggurat) runs on that channel's radio. The Zigbee mode
  design specifies the stack; this design gives it its radio.
- **RCP proxy**: the channel's bytes are served to one TCP client (today's USB serial proxy), whatever firmware the
  radio runs.

Why: dongle support and the parallel Zigbee work both need a radio to be a pluggable thing, not "the C6". The user's
words: "some elegant architectural muxer solution, ideally without unnecessary buffer copy". The C6-or-dongle choice
is "part of the muxer core", so the C6 is not special. A missing dongle can't be used: it is listed as unavailable
(the user first said it "just disappears from mux channels", then chose to keep known dongles listed).

Goals:

- One channel mux core that owns every radio transport (the C6 over SPI, USB dongles over CDC), its presence, reset,
  line settings and counters, and hands frames or bytes to exactly one consumer per channel.
- A stored mode per channel, set through the API and a web page. Changing it never needs a reboot, except into or out
  of Ziggurat (ziggurat cannot stop).
- OTBR runs on the C6 or on a dongle (proven: a ZigStar OllyZig, CC2652P with Koenkk's OpenThread RCP firmware 2025.3.1,
  CH340, 460800 baud, joined a real network as a router in the spike).
- Moving OTBR between channels at runtime, without a reboot, keeping the stored Thread network.
- No avoidable buffer copies (section 3, Copies), and no polling latency: a frame wakes its consumer.
- Plugging and unplugging a dongle in any mode never crashes the board.
- A board without a dongle, after the upgrade, behaves exactly as today: the C6 channel in OTBR mode.

Non-goals (this design):

- **Zigbee.** No Zigbee stack, spinel client, Zigbee task, page, endpoint or `/device/status` field. Only the
  placeholders in section 3, "Ziggurat mode: placeholder". The Zigbee work (another agent) does the rest.
- **Hybrid** (OTBR on one channel and Ziggurat on another at the same time). The model allows it; the first release
  refuses it (section 3, Mode rules). It comes later.
- Two channels in OTBR mode: one OpenThread instance, a singleton ESP port, no multipan RCP firmware. Not planned.
- More than one USB dongle. It needs a USB hub with the host controller forced to full speed (`fsls_only`, because
  full-speed devices behind a high-speed hub are refused: no transaction translator). The model is N channels; the
  first release supports one dongle.
- Firmware updates for dongles. The C6 keeps its update, recovery and jumper paths unchanged.
- Probing a stick that is in use: detection runs only on an `off` channel (section 3, Firmware detection).
- An own zero-copy USB bulk driver. The espressif CDC-ACM driver stays (section 3, Copies, says what that costs).
- Network adapters beyond TCP and UDP (spinel2quic and others come later), and authentication or encryption on the
  plain TCP and UDP adapters.
- Automatic fallback: if the OTBR channel becomes unavailable, Thread stays down until it comes back or the user picks another
  channel (the user's decision).

Anti-goals (a failure even if it works):

- A board with no dongle behaving differently after the upgrade: boot, the border router, the C6 firmware pages, the
  existing pytest suite.
- Any unplug, RCP silence or mode change aborting the board (OpenThread's recovery ends in `exit()`, the CDC driver's
  close has `ESP_ERROR_CHECK`s).
- A mode change touching the stored Thread network (datasets, settings).
- A channel that silently half-works: absent, faulted or busy channels show in the API and on the page.
- Mux code running consumer code on a transport task or an interrupt, or the OpenThread task calling lwIP.

Success: the existing on-board suite passes unchanged on the C6; OTBR runs on the dongle and on the C6, moved between
them at runtime; unplugging the OTBR dongle leaves the board up with Thread down, and replugging brings Thread back;
the proxy serves the dongle and the C6 (otbr-agent on a Linux host); the consumer API and the mode-consumer
registration (section 3) stay as agreed with the Zigbee work, so its component plugs in without changes here.

## 2. Facts this design rests on

From the spike (`spike/dongle-rcp`) and the research (merged notes in the session scratchpad,
`mux-research/synthesis.md`):

- ESP-IDF v6.1 picks the spinel interface at build time: one static `SpinelInterfaceAdapter<UartSpinelInterface or
  SpiSpinelInterface>` in `components/openthread/src/port/esp_openthread_radio_spinel.cpp`. `SpinelDriver::Init`
  already takes the abstract `ot::Spinel::SpinelInterface&`; the coupling is the static adapter and a handful of
  concrete-only calls (`Enable`, `Disable`, `RegisterRcpFailureHandler`, `AfterRadioInit`).
- The same file hard-codes the radio capabilities it requires from any RCP (`s_radio_caps`, 0x1FF with rx-on-when-idle).
  The CC2652P reports 0x7f (no rx-timing, no rx-on-when-idle). OpenThread uses rx-timing only for CSL and wake-up
  receive; ot-br-posix requires only ack-timeout, tx-retries and CSMA backoff, plus tx-security and tx-timing for Thread
  1.2 and later. After init, OpenThread works from the RCP's real caps.
- The spike's shadow component (`tools/dongle_spike.cmake`: symlinks to IDF's openthread component, plus a generated,
  patched copy of that one file, with a fatal error if IDF's text drifts) builds and runs.
- OpenThread's RX frame buffer and HDLC decoder have no locking; they run under the OpenThread lock, from whichever
  task holds it (the OpenThread task in `Process`, or any task blocked in a synchronous spinel call in `WaitForFrame`).
  A frame can only be put into it from inside the interface's `Process`/`WaitForFrame`.
- OpenThread's RCP recovery ends in `DieNow` (`exit()`) after three failures, or when a reset fails. br_core survives
  today only by parking the task or rebooting.
- br_core already stops and restarts OpenThread at runtime (the C6 firmware push: sessions drained, REST flag cleared,
  netif and border router torn down, `esp_openthread_deinit`, then `init` again). The dataset and frame counters are
  host settings and survive; the EUI-64 comes from the RCP and changes with it. An incompatible RCP parks the task
  inside `esp_openthread_init` (`init_abandoned`, reboot only), so a radio must be probed before OpenThread gets it.
- The CDC-ACM driver (2.4.1) hands `data_cb` a pointer into its one IN transfer buffer, valid only until the callback
  returns (append is unsupported on the P4). TX always copies into its one OUT buffer and blocks. Unplug arrives as a
  DEVICE_DISCONNECTED event; `cdc_acm_host_close` must then run exactly once, from one task.
- Flow control: CP210x through the proxy's own vendor request, CH34x none, plain CDC none. DTR and RTS are set together
  in one request to avoid half-pulses on auto-reset circuits.
- The proxy's 10-40 ms comes from polling: a 5 ms `select` on a 10 ms FreeRTOS tick plus a 5 ms coalescing window. An
  eventfd written from `data_cb` wakes a `select` at once (IDF's SPI interface uses the same pattern for the C6's INT
  pin).
- lwIP always copies on `send()` (`LWIP_NETIF_TX_SINGLE_PBUF=1` forces `tcp_write` to copy), so the TCP side has no
  zero-copy path to gain.
- The C6 over SPI is frame based: a 5-byte header and one spinel frame per transaction, INT pin to an eventfd, a
  cache-aligned DMA buffer, a reset flag in the first transaction after init.
- Ziggurat (see the Zigbee design) needs: configure, set channel, promiscuous, a source-match table, transmit with
  CSMA/retries/ack in the RCP, energy scan, the EUI-64, and RX frames with metadata. All its calls run on one task. It
  starts once and cannot stop. With its own small C spinel client (OpenThread's plain-C `lib/spinel/spinel.c`), it
  needs no OpenThread at all.

## 3. Decisions

### Channels

- A channel has an **id**, a **kind** (`c6` or `usb`), a **mode**, a **state**, and per-kind details.
- Ids are stable, so a stored mode follows the device: `c6` for the on-board radio; `usb-<vid>-<pid>` for a dongle,
  plus `-<serial>` when the device reports a serial number *(design call: a CH340 has none, so two identical CH340
  dongles would share an id; acceptable while one dongle is supported)*.
- The C6 channel always exists. A USB channel exists while its device is attached: with a supported driver (cp210x,
  ch34x, plain CDC-ACM) it can take a mode; without one (FTDI, anything else) it is listed as `unavailable`. An
  unplugged dongle the board has seen before stays listed as `unavailable` (reason: not detected), with its stored
  settings, which apply again when it returns *(the user's decision)*. `DELETE` forgets it.
- **State** (within the mode): `idle` (mode `off`), `starting`, `running`, `fault` (with a reason), `busy` (a C6 job
  runs).

### Modes and rules

- Modes *(the user's list)*: `unavailable`, `off`, `otbr`, `ziggurat`, `rcp-proxy`.
  - `unavailable` is reported, never set: the channel was not detected or is not compatible, so no mode can run on
    it. Examples: the C6 not answering at boot or running its factory ESP-Hosted firmware; an attached USB device
    without a supported driver; a known dongle that is unplugged. The channel's reason says which. A stored mode is kept underneath and applies once the
    channel becomes available. A PUT that sets `unavailable` is refused (400); a PUT of any mode on an `unavailable`
    channel is refused (409 `unavailable`); its line and `rcp_proxy` settings can still be changed and are stored
    *(design call: a dongle can be prepared before it is plugged in)*.
  - A radio that is present and supported but fails in a mode (a ZNP dongle set to `otbr` fails the probe) is not
    `unavailable`: it is that mode in `fault`, because it may still work in another mode (`rcp-proxy`).
  - `rcp-proxy` is the serial proxy (the user's name for it): the channel's bytes go to one TCP client.
- At most one channel in `otbr` (OTBR's registration declares one). A mode at its declared limit refuses another channel
  with 409 `mode_taken`. *(Hybrid, OTBR plus a registered Ziggurat consumer at once, is refused in the first release
  with 409 `hybrid_not_supported`; nothing else in the design depends on that refusal.)*
- Setting `otbr` on a channel while another one has it **moves** Thread: the old channel becomes `off`, OpenThread
  stops and starts on the new one, without a reboot. *(Design call: one request to move Thread instead of two that leave
  a gap.)* The order *(design calls)*:
  1. The new channel's RCP is probed first, through the channel, while Thread still runs on the old one. A failed
     probe refuses the move (409 `probe_failed`, with the reason) and changes nothing.
  2. Then OpenThread stops on the old channel and starts on the new one.
  3. The stored modes change only after the new channel runs.
  4. If the start fails anyway, br_core starts OpenThread on the old channel again and the move reports the failure.
     Only if that also fails does the board end in `RADIO_FAULT`.
  - A move takes up to a minute (session drain, probe, init), so it runs as a long operation (`device_api/long_op.c`):
    the PUT answers 202 and the channel shows `starting` until it settles; the page polls.
  - A plain stop (`otbr` to `off`) and a start on a channel with no OTBR elsewhere work the same way.
- `ziggurat` is a reserved mode with no consumer in this implementation: it is listed in the API's mode values, refused
  with 409 `not_available`, and not offered on the page. Its rules (at most one channel, a reboot on entering or
  leaving it) come from the consumer the Zigbee work registers (section 3, "Ziggurat mode: placeholder").
- `rcp-proxy` and `off` apply at once, on any channel.
- Defaults: `c6` is `otbr`; a new dongle is `off`. Every stick is detected when it appears, and gets its stored mode
  only if its firmware fits (section 3, Firmware detection) *(the user's decision)*.
- Line settings per channel: baud (default 460800, the OpenThread RCP standard; the proxy accepts its existing list)
  and flow control (CP210x only). The proxy's direction, adapter and port are `rcp-proxy` settings (section 3, RCP
  proxy mode).
- The C6 in `off` is held in reset *(design call: no RF next to an active dongle, nothing to watch)*. C6 jobs (firmware
  push, recovery) run in any mode except `rcp-proxy` with a connected peer, which refuses them (409 `c6_busy`)
  *(design calls)*. While a job holds the C6, its consumer sees `GONE`. Afterwards the C6 comes back and the consumer
  is attached again (below), with no reboot. For OTBR this is today's push behaviour, with OpenThread stopped and
  started again.

### Layers

- **Transport** (one per channel, owned by the mux): presence, open/close, reset, line settings, counters, and a wake
  primitive.
  - **USB transport**: the existing usb_serial glue, moved into the mux. One USB host task and one CDC client, started
    at boot whenever the mux runs (no longer only when the proxy is enabled). One mux task is the only one that opens
    and closes a device. DEVICE_DISCONNECTED becomes a GONE event.
  - **C6 transport**: the mux owns SPI2, the INT pin, and the C6 reset GPIO for every mode *(design call: Ziggurat and
    the proxy on the C6 need it anyway, and OpenThread then has one interface for every channel)*. rcp_sync's probe,
    push, recovery and jumper paths stay; the C6 transport calls them as its update and recovery operations, and
    releases SPI2 to them during a job, as the mode FSM does today.
- **Framing adapter** (chosen by the mode): `raw` (bytes, for the proxy on a USB channel), `hdlc` (USB bytes to and from
  spinel frames, for OTBR and Ziggurat on a dongle, and C6 frames to and from HDLC bytes for the proxy on the C6), or
  `identity` (C6 frames for OTBR and Ziggurat).
- **Consumer** (the mode): one per channel, attached and detached by the mux when the mode or presence changes.

### Frames, events and copies

- A channel delivers spinel frames from a per-channel **frame pool**: fixed slots, DMA-capable and cache-line aligned,
  so the C6's SPI DMA lands directly in a slot. A consumer gets a slot reference and releases it when done. Slot size
  and count are the builder's call (the largest spinel frame here is 2048 bytes).
- Events: `RX_FRAME` (a slot, with a receive time in µs from `esp_timer` taken at the transport), `RESET` (the mux reset
  or reopened the radio: a C6 GPIO reset, a dongle reopened; spinel-level reset notifications stay visible to the
  consumer's own spinel parsing), `GONE` (unplugged or the transport failed for good). There is no TX completion event:
  every consumer sees the RCP's own answer to its frames.
- Any consumer can ask its channel for a reset (C6: the GPIO reset; dongle: close and reopen); it gets `RESET` when the
  radio is back. OTBR's `HardwareReset` uses the same operation.
- A consumer attaches whenever its channel is present and in its mode, and that is one path, not several:
  - at boot;
  - when the device is plugged in after boot;
  - when the channel comes back after `GONE` (replugged, or a C6 job finished);
  - after a mode change.
  A consumer that was attached, got `GONE` and is attached again must be a normal case, never an error *(agreed with
  the Zigbee work, whose consumer keeps running through `GONE` and replays its configuration on the new attach)*. A
  stored mode whose device is unplugged at boot means the consumer waits, unattached.
- Each channel's counters (frames in and out, bytes, pool-exhaustion drops, resets) are readable by its consumer and
  in the API.
- Delivery: a consumer hands the mux a FreeRTOS queue at attach; the mux posts events into it. The OTBR consumer
  instead gets an eventfd to put in OpenThread's mainloop `select` (below). The mux never calls into a consumer from a
  transport task or an interrupt.
- TX: the consumer passes one spinel frame (or bytes, for the proxy); the mux encodes and sends it and never keeps the
  caller's pointer past the call. TX after GONE fails at once; it never blocks on a vanished device.
- Copies per received frame, the floor with the CDC driver:

  | Path | Copies |
  |---|---|
  | C6, OTBR | 1 (slot to OpenThread's RX frame buffer); same as today |
  | C6, Ziggurat | 0 before ziggurat's own parse |
  | Dongle, OTBR | 2 (CDC buffer decoded into a slot, slot to OpenThread's RX frame buffer) |
  | Dongle, Ziggurat | 1 |
  | Dongle, proxy | 1 into the channel's ring, then lwIP's own copy |

  A frame pool exhausted by a slow consumer drops the frame and counts it; it never blocks the transport.

### OTBR mode

- OpenThread reaches every channel through one project `SpinelInterface` subclass over a mux channel
  (`MuxSpinelInterface`, name the builder's): `Process` and `WaitForFrame` copy pending slots into OpenThread's RX frame
  buffer under the OpenThread lock, `UpdateFdSet` adds the channel's eventfd, `SendFrame` passes the frame to the
  channel, `HardwareReset` asks the channel for a reset.
- ESP-IDF's `esp_openthread_radio_spinel.cpp` is replaced by a project copy that uses this interface and takes the
  required radio caps from the channel (the ot-br-posix set by default). It is generated from IDF's file with a guarded
  patch, like the spike's shadow component, or vendored *(builder's call)*; either way it is listed in `PATCHES.md`.
  An upstream ESP-IDF change (a runtime "custom spinel interface" radio mode with per-interface caps) is offered later.
- Start: br_core probes the channel's RCP (spinel reset, version, caps) through the channel before `esp_openthread_init`,
  so an incompatible or wrong-baud radio becomes a channel fault, never `init_abandoned`. Then it starts OpenThread as
  today.
- Move or stop: br_core's existing teardown (sessions drained, REST flag, netif, border router, `esp_openthread_deinit`)
  runs on its own task, then starts again on the new channel. A drain timeout refuses the change, as a push does today.
- Unplug of the OTBR channel: the mux marks the channel GONE; the interface stops delivering, `SendFrame` drops and
  `WaitForFrame` times out quickly; br_core tears OpenThread down before RadioSpinel's recovery gives up. The board
  stays up, Thread is down, the mode stays `otbr` on the now `unavailable` channel, and Thread starts again when it returns.
  Nothing aborts.
- A new board mode, `NO_RADIO`, means "no OpenThread because no present channel is in `otbr`" (no channel set, or its
  dongle unplugged). The Thread API answers 503 then, as in `STARTING`. *(Design call: `RADIO_FAULT` keeps meaning a
  broken radio; an unplugged dongle is not a fault.)*
- The sniffer stays as today: it borrows the OTBR channel's radio through OpenThread's raw link, so it needs OTBR
  running.

### Firmware detection

- Every time a stick appears (plugged in, or attached at boot), it goes through the same steps *(the user's decision)*:
  1. It starts as `off`. Nothing else uses an `off` stick, so the probe can't disturb a stack or a client.
  2. The mux detects its firmware (probes below).
  3. The mux resets it (close and reopen over USB), so the probe bytes leave nothing behind.
  4. If the detected firmware fits the stored mode, the stick switches to that mode. If it doesn't, it stays `off`,
     with the stored mode kept and a reason such as "stored mode otbr needs spinel; detected znp". A stick reflashed
     elsewhere is therefore checked again on its next plug-in.
- What fits:
  - `otbr` and `ziggurat` need `spinel`.
  - `rcp-proxy` fits anything, `unknown` included, so a stick behind a remote stack the probes don't know still works.
  - `off` fits anything.
- A brand-new stick has the stored mode `off`, so it is only detected and listed.
- The C6 is not probed this way: rcp_sync's boot probe already reports its firmware.
- Probes, in an order that keeps the side effects to the end (universal-silabs-flasher's approach). The first that
  answers wins. The whole detection is capped at 3 s *(the user's decision)*; the builder splits that budget across
  the probes (about 500 ms each, the EZSP reset a little longer):
  1. **spinel** at 460800, then 115200. This covers an OpenThread RCP, and a Ziggurat-on-stick firmware, told apart by
     `SPINEL_PROP_VENDOR_ZIGGURAT` and given the type `ziggurat`, which fits only `rcp-proxy`.
  2. **Z-Stack ZNP**: `SYS_PING` and a version query at 115200.
  3. **EZSP** (Silicon Labs NCP): an ASH reset at 115200, then 460800. This restarts the stick, which is why it runs
     last.
  - Bootloaders are never probed: stray bytes can start their menus.
  - A spinel stick answers in milliseconds. A stick that answers nothing is listed as `unknown` after at most 3 s.
- The result is stored with the channel's settings and shown with the channel, also while it is unplugged:
  - `type` (`spinel`, `ziggurat`, `znp`, `ezsp`, `unknown`);
  - the version;
  - the radio chip when the firmware reports it (spinel's version string such as
    `OPENTHREAD/1.4.0-Koenkk-2025.3.1; CC13XX_CC26XX`, ZNP's version reply). The USB descriptor names only the bridge
    (CH340, CP2102N), never the radio;
  - the baud that answered;
  - the source (`probe` or `passive`) and the time.
  - If no probe answers, the type is `unknown`. That is not an error: the stick may run something else, or need a baud
    outside the list.
- Passive updates: when a channel runs `otbr` or `ziggurat`, the spinel handshake already returns the version, and the
  channel records it with source `passive`.
- A PUT of a mode that doesn't fit the detected firmware is refused (409 `incompatible`, naming the detected type).
- There is no separate detection endpoint *(the user's decision)*: unplugging and replugging a stick (or a reboot)
  runs detection again, for example after flashing it.
- The answering baud becomes the channel's baud when none was set.
- The mode-start probe (OTBR's, Ziggurat's spinel attach) stays the final check.

### RCP proxy mode

An `rcp-proxy` channel is configured on two independent axes *(the user's decision)*:

- **Direction**:
  - `listen`: the board waits for one peer.
  - `connect`: the board dials out to one configured `host:port` and the far side listens. This is the production
    direction from the usb-serial design's section 5.
  - Default `listen` *(design call: `connect` needs a host before it can do anything)*.
- **Network adapter**:
  - `tcp` or `udp` for now, default `tcp`.
  - An adapter is a pluggable piece behind one small internal interface: open (listen or connect), send, receive into
    the channel, peer status, close.
  - It declares whether it is a byte stream or carries datagrams.
  - More advanced adapters (spinel2quic, for example, which would also bring encryption) plug in later without touching
    the channel, the framing or the other modes.

The bridge itself does not depend on either axis:

- **Shared behaviour**: today's usb_serial TCP half becomes the proxy consumer, with overflow disconnect, counters and
  `kick`.
- **Wake-up**: it is woken by the channel's eventfd instead of polling. The coalescing window shrinks to what the
  measurements justify *(builder's call)*, which removes the 10-40 ms.

**TCP**:
- Listen: first client wins, under the netcfg access rules (the per-VLAN rule, the Thread-mesh refusal), with
  keepalive.
- Connect: dials out to `host:port`.
  - The host is an IP address or a hostname; `.local` names go over mDNS.
  - A lost or refused connection retries with back-off (1 s doubling to 30 s) *(design call)*.
  - Keepalive notices a dead peer.

**UDP**:
- A datagram adapter needs message boundaries, so it has a `framing` setting *(design calls)*:
  - `hdlc` (default): the stream is cut at HDLC frame boundaries, one frame per datagram. This suits spinel and other
    HDLC-framed protocols; a lost datagram is a lost frame, which spinel's request/response retries tolerate.
  - `raw`: each received chunk becomes a datagram, up to 1024 bytes. Loss and reordering corrupt the stream, so the
    page warns that this is for testing.
- Listen: the first source address that sends becomes the peer. Datagrams from any other source are dropped and
  counted, until the peer has been silent for 30 s or is kicked *(design call)*. The netcfg access rules apply to the
  source.
- Connect: datagrams go to `host:port`, and only datagrams from that address are accepted.

**The C6 in `rcp-proxy`**:
- The proxy uses the `hdlc` framing adapter in reverse, so a Linux otbr-agent (`spinel+hdlc+uart` through socat, or a
  UDP-to-pty bridge) can drive the board's C6.
- On UDP with `hdlc` framing, a C6 frame maps directly to one datagram.
- The mux replays the SPI reset-flag handshake when a peer connects (TCP) or first sends (UDP).

Settings per channel, under `rcp_proxy`: `direction`, `adapter`, `port` (the local port to listen on, or the remote
port to connect to; default 6638), `host` (connect only), and `framing` (UDP only).

### Ziggurat mode: placeholder

This implementation writes no Zigbee code. It provides the seam the Zigbee work plugs into:

- **Mode-consumer registration.** A component registers a consumer for a mode name at startup. The registration
  declares:
  - the firmware type it needs (`spinel`);
  - how many channels may use the mode at once;
  - whether entering or leaving the mode reboots the board;
  - its attach and detach entry points: attach hands over the consumer's event queue, detach takes it back.
  OTBR and `rcp-proxy` register through the same interface, so the interface is exercised and tested here. The mux
  enforces what a registration declares; it holds no Zigbee rules of its own.
- **The reserved name.** `ziggurat` is a known mode value with no registered consumer, so it is refused with 409
  `not_available` and not offered on the page. When the zigbee component is built in and registers, the mode becomes
  available with the rules it declares. The Zigbee design declares one channel and a reboot on enter and leave.
- **`GONE` is only delivered.** The mux never reboots because a consumer's channel went away; each consumer decides
  what `GONE` means for it, and is attached again when the channel returns. OTBR's answer is `NO_RADIO`; `rcp-proxy`
  drops its peer. The Zigbee work's consumer keeps its stack running in a "no radio" state and replays its
  configuration on the next attach.
- **The consumer API** (section 3, "Frames, events and copies"): frame-pool slots, `RX_FRAME`/`RESET`/`GONE` posted into
  the consumer's queue, consumer-requested reset, TX of one frame. It is agreed with the Zigbee work and implemented
  here for OTBR and `rcp-proxy` anyway.
- **Host tests** check the registration rules with a stub consumer that declares one channel and reboot on change:
  limits, the reboot reply, attach, `GONE` and attach again on the same channel, a C6 job as `GONE` then re-attach,
  and the refusal of an unregistered mode.

Everything above the radio belongs to the Zigbee work: its spinel client, task, replay on `RESET`, behaviour on
`GONE`, pages, endpoint and status fields.

### Board mode, faults and status

- br_core's board mode stays the OTBR consumer's view (`STARTING`, `BR_RUNNING`, `SNIFFER`, `RADIO_FAULT`, plus
  `NO_RADIO`). Channel health lives in the channel. A C6 fault in `otbr` follows the existing recovery ladder through
  the C6 transport's reset operation.
- A dongle's reset is close and reopen over USB *(design call: no DTR/RTS pulse, which can drop an auto-BSL dongle into
  its bootloader; no VBUS cycling, since the port's VBUS switch is not known to be controllable)*. `caps.hw_reset` is
  false for dongles.
- A dongle that stays plugged in but goes silent in `otbr`: OpenThread's hardware-reset requests become reopens. If the
  radio does not answer within the existing recovery ladder, the channel goes to `fault` and br_core tears OpenThread
  down from its own task (`RADIO_FAULT`, with the reason) before RadioSpinel's recovery reaches `DieNow`. The builder
  owns how the interface holds OpenThread off until then (for example, `HardwareReset` waiting for the mux's verdict).
  A fault is retried on replug or on a PUT of the mode; nothing retries forever on its own *(design call)*.
- br_core's radio operations that are C6-specific today (reset, push, ROM recovery) become channel operations,
  absent for dongles.
- The REST "OpenThread ready" flag keeps one source of truth in br_core; the mux does not write it.

### API

- `GET /device/channels`: `{"channels": [<channel>...]}`, including known dongles that are unplugged (mode
  `unavailable`, `stored_mode` holding the mode they return to), where a channel is
  `{"id", "kind", "label", "mode", "state", "error", "line": {"baud", "flow_control"}, "usb": {"vid", "pid", "serial",
  "driver"} | null, "caps": {"firmware_update", "hw_reset", "flow_control"}, "rcp_proxy": {"direction", "adapter", "port", "host", "framing", "state", "peer"} | null,
  "firmware": {"type", "version", "chip", "baud", "source", "at"} | null, "counters": {...}}`. Exact counter names are the builder's.
- `PUT /device/channels/{id}`: any of `mode`, `baud`, `flow_control`, and an `rcp_proxy` object with any of its settings (`direction`, `adapter`,
  `port`, `host`, `framing`); `connect` without a `host` is refused (400). Replies with the channel; with
  `"reboot": true` when the change reboots; with `"moved_from": "<id>"` when Thread moved. Errors: 404 unknown
  channel, 400 bad value, 409 `unavailable`, `incompatible`, `hybrid_not_supported`, `mode_taken` (a mode at its channel limit), `not_available`, `probe_failed`, `c6_busy`,
  `drain_timeout`.
- `DELETE /device/channels/{id}` forgets an unplugged dongle and its settings; it then leaves the list *(design
  call)*. The C6 and attached devices can't be deleted (409).
- `/device/status` keeps its fields: `mode` (with `NO_RADIO`), `rcp` and `jumper` (the C6), and gains `otbr_channel`
  (id or null). Existing pages and tests keep working. (The Zigbee work adds its own fields later; nothing here.)
- `/device/usb_serial` (never released) is replaced by the channel API, not kept as an alias.
- `/device/radio/*` (C6 firmware, recovery, log) stay C6-only.

### Web UI

- A **Radios** page in the Device group: one row per channel with its label, state, mode selector, line settings and,
  in `rcp-proxy` mode, its direction, adapter, port or host, and the peer. Moving Thread asks for confirmation ("Thread moves from the C6 to the
  dongle; Thread devices lose the border router for a few seconds"); a mode whose consumer reboots on change says so
  before saving. Layout is the builder's call.
- The Dashboard's radio card names the OTBR channel.
- The Thread pages (Management group, Topology, Routing, Packet capture) depend on why Thread is down *(design call,
  shared with the Zigbee design, which follows it)*:
  - No channel at all in `otbr` (a deliberate choice, for example the board runs only Zigbee): the pages are hidden,
    and opening one directly shows a short notice linking to the Radios page.
  - The OTBR channel is unavailable or faulted (unplugged, broken): the pages stay and show "no radio in OTBR mode" with
    the channel's state and a link to the Radios page. A transient unplug should not reshuffle the navigation.
  - The Thread REST API and the console WebSocket answer 503 in `NO_RADIO`, as in `STARTING`.
- The channel list lives on the Radios page; there is no separate stack or radio setting elsewhere.

### Code layout

- A new component (name the builder's, prefix per `STYLEGUIDE.md`) with the registry, mode rules, framing and frame
  pool, and the transports. Pure logic (mode rules, ids, the HDLC adapter, pool accounting) in its own files with host
  tests, following `usb_serial/core.c` and `br_core/mode_fsm.c`.
- usb_serial splits: the USB glue and discovery move into the mux; the TCP half becomes the proxy consumer.
- br_core gets the channel handle for OTBR, the probe step, `NO_RADIO`, and channel-based radio operations.

### Left to the builder

Component and type names; the shadow copy vs a vendored copy of the radio spinel file; frame pool sizes; the mux task's
priority and core (within `board_cores.h`); NVS namespace and keys; the proxy's coalescing; the Radios page layout;
counter names; how the interface's `WaitForFrame` bounds its wait after GONE.

## 4. Testing

- Host: mode rules (limits, moves, hybrid refusal, defaults), channel ids, the HDLC adapter (both directions, CRC,
  escapes, frames split across chunks), frame pool accounting (exhaustion drops and counts), the proxy settings check
  (`connect` needs a host, `framing` only with UDP), the UDP peer lock (the first source wins, others dropped, released
  after 30 s of silence), the connect back-off schedule, the firmware-fit rules, and the probe answer parsers (a spinel
  version string, a ZNP version reply, an EZSP reset answer, silence).
- On the board, no dongle: the whole existing suite, unchanged, as the regression gate; `GET /device/channels` lists
  `c6` in `otbr`; PUT validation; `c6` to `off` and back (Thread stops, `NO_RADIO`, starts again with the same network).
- On the board, with the OllyZig (marked, needs the dongle): OTBR moved to the dongle and back, network kept; unplug
  under OTBR (no reboot, `NO_RADIO`), replug (detected as spinel with chip `CC13XX_CC26XX`, Thread back); a stored mode
  that doesn't fit stays `off` with its reason (the OllyZig's Z-Stack backup, flashed once, as the ZNP case); the proxy on the dongle with zigpy or otbr-agent, latency
  well under the old 10-40 ms; the proxy on the C6 with otbr-agent on the host; `connect` over TCP to a host-side
  listener, including the back-off after the listener stops and starts; UDP in both directions with `hdlc` framing.
- Ziggurat: no tests here beyond the stub consumer in the host tests; the Zigbee work tests its own consumer.

## 5. Later

Hybrid; more than one dongle (hub, `fsls_only`); an own USB bulk driver with an IN pool and async OUT if profiling
asks for it; the upstream ESP-IDF change; more proxy network adapters (spinel2quic first); a Ziggurat stop
upstream, which would remove the reboots.
