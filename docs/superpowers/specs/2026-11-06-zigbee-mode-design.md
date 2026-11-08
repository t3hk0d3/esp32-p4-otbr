# Zigbee mode: ziggurat on a radio channel: design

Date: 2026-11-06 (revised the same day: built on the radio channel mux, mode per channel). Status: draft for review.
Spike: branch `spike/ziggurat-p4` (b190cfb), throwaway. Depends on: the radio channel mux design,
`docs/superpowers/specs/2026-11-06-radio-channel-mux-design.md` on branch `feat/radio-mux` (approved 2026-11-06,
77e683b), which defines the consumer API and the mode-consumer registration used here.

## 1. Purpose and scope

The board's radios (the on-board C6, and RCP dongles on the USB OTG port) become **channels** of a radio channel mux,
and each channel has one mode: `off`, `otbr`, `rcp-proxy` or **`ziggurat`** (a channel whose radio is not detected or
not compatible reports `unavailable` instead, keeping its stored mode). This design covers the `ziggurat` mode: the P4 runs
[zigpy/ziggurat](https://github.com/zigpy/ziggurat), an open-source Zigbee stack in Rust, on one channel, with a small
spinel client of its own that drives the channel's OpenThread RCP firmware. OpenThread is not involved. ZHA (or any
zigpy application) connects to the board with radio type "Ziggurat" and the path `ws://<board>/zigbee`.

Why: one PoE board, one firmware, any of its radios, for either kind of mesh. Ziggurat is designed around OpenThread
RCP radios, so the C6 firmware, its updates and repair paths stay as they are, and a stock OpenThread RCP dongle (the
CC2652P on the rig) works the same way.

### Goals

- A channel set to Ziggurat mode runs ziggurat on that radio. The setting is the mux's per-channel mode (stored per
  device: the C6, or a dongle by VID/PID/serial), chosen on the Radios page in the Device group. Changing a channel into or
  out of Ziggurat mode reboots the board, because ziggurat cannot be stopped once started.
- At most one channel in Ziggurat mode (the staticlib's C ABI drives one radio).
- Ziggurat mode is a real mode: it starts at boot, survives client disconnects, recovers from radio resets in place,
  and shows its state on its own page and in the API.
- The Zigbee side does not depend on OpenThread at all. Whether a release also allows an OTBR channel next to the
  Ziggurat channel ("hybrid") is the mux design's call; nothing here prevents it.
- The web UI follows the channels: Thread pages appear when a channel runs OTBR, Zigbee pages when one runs Ziggurat.
  Everything about the board itself (Ethernet, network config, updates, C6 firmware and repair, diagnostics, metrics,
  names, About) works whatever the channels do.
- Moving the C6 from OTBR to Ziggurat and back leaves the stored Thread network (datasets, settings) untouched.
- Done means: Ziggurat mode runs as a production mode on the C6 and on a USB dongle, and on-board end-to-end tests
  through zigpy pass: forming a network, a real device joining, commands and attribute reports both ways, a client
  reconnecting, a radio reset survived, the mode switch round trip. One manual acceptance pass with Home Assistant's
  ZHA closes it.
- The first Zigbee release waits for the mux (the user's call, 2026-11-06). There is no interim path through
  OpenThread's raw link.

### Non-goals (this design)

- The mux itself: transports, framing, hot-plug, per-channel settings storage and API, the C6 bring-up (probe,
  compatibility, ROM recovery, firmware push). That is the mux design. This design consumes its consumer API.
- More than one Ziggurat channel.
- Separate firmware images per mode. One image for now; splitting later stays possible because the Zigbee code is one
  component plus pages.
- Zigbee features above the stack: no device list, no ZCL, no pairing UI on the board. ZHA owns all of that; the board
  is a radio and a stack, like a Zigbee dongle.
- Packet capture on a Ziggurat channel, Zigbee OTA of end devices, Green Power beyond what ziggurat passes through,
  Zigbee R23.
- Home Assistant discovery of the Zigbee endpoint (zeroconf). ZHA is set up by hand with the path.
- Board-side Zigbee settings (access key, allowed clients, takeover policy, TX power cap, own port). The Zigbee page
  is status only (the user's call, 2026-11-06); ZHA owns the network settings and the Radios page picks the radio.
- Changing a channel's mode without a reboot.

### Anti-goals (a failure even if it works)

- Any path through which Ziggurat mode erases or rewrites the stored Thread network.
- Forking ziggurat. Upstream is consumed at a pinned commit; local changes are limited to build-time fixes that are
  also offered upstream.
- A Ziggurat channel that silently half-works: a radio that stopped or lost its settings, a stack that panicked or a
  client that cannot connect must show on the page and in `/device/status`, not only in the serial log.

## 2. Facts this design rests on

From the spike and its on-board runs (2026-11-06):

- Ziggurat ships its whole stack as a C ABI staticlib for microcontrollers (`crates/ziggurat-ot`, embassy runtime, no
  std), meant to be linked into OpenThread RCP firmware. It needs 20 `ziggurat_platform_*` functions from the firmware:
  time and a one-shot timer, a wake-up, entropy, AES-128 block and CCM*, a host send, 12 radio operations, the EUI-64,
  reset, panic and post-mortem. Its own `ziggurat_*` entry points include the radio completions (receive, transmit
  done, energy scan done). It has no stop: leaving it is an MCU reset.
- The 12 radio operations: configure (channel, TX power, short address, PAN ID, extended address, promiscuous, rx on
  when idle), set channel, set promiscuous, source-match clear / add short / add extended (the RCP sets frame pending
  in its auto-ACK from that table, for sleepy children), transmit (PSDU without FCS, channel, CSMA on/off, max frame
  retries, max CSMA backoffs; one in flight; the frame is already secured and final; completion acked, no ack, channel
  access failure, aborted or failed), energy scan (channel, duration, completion with max RSSI). Received frames go to
  ziggurat without FCS, with channel, RSSI, LQI and a µs timestamp. ACK, CSMA and retries are done by the RCP.
- The staticlib's critical sections mask interrupts on the current hart only. The P4 has two cores, so every
  `ziggurat_*` call must stay on one task.
- It builds for `riscv32imafc-unknown-none-elf` (the P4's ISA and single-float ABI) with stable Rust 1.99, with two
  local fixes: radium 0.7 (via bitvec) does not know that target lacks 64-bit atomics, and the target's prebuilt
  compiler-rt C objects are soft-float ABI, which ld refuses next to ESP-IDF's code (libgcc provides the same
  functions).
- zigpy-ziggurat (1.2.1) speaks the binary protocol over WebSocket, one frame per binary message, and waits up to 5 s
  for a `hello` frame after every connect. `ziggurat-server` sends one per connection; the embedded stack sends one only
  when it starts. A later `hello` tells zigpy the firmware reset.
- The spike drove the C6 through OpenThread's raw link. That proved the stack, the platform layer and the radio
  operations on this board: a network formed, an energy scan ran on all 16 channels, a Tuya TS0041 (a sleepy end
  device) joined and was interviewed by zigpy. It also found that the raw-link path has hazards this design avoids by
  not using OpenThread: `otLinkSetPanId` clears the stored Thread datasets, and whether OpenThread's RCP restore keeps
  raw-link state is unknown.
- Measured on the spike build: app 2.3 MB of an 8 MB slot (ziggurat about 300 KB); static internal RAM 203 KB of 572 KB
  (ziggurat 5 KB); internal heap free 353 KB in Thread mode and 340 KB with ziggurat running a formed network
  (OpenThread still loaded), about 13 KB with no devices. PSRAM is about 31 MB free.
- ESP-IDF's own standalone spinel API (`esp_radio_spinel`) is not usable here: OpenThread host builds exclude it
  (`srcs_ftd_mtd.cmake`), and it is UART-only with no per-frame retry or backoff control.
- The mux consumer API, as the mux design committed to it (2026-11-06): a consumer exchanges whole spinel frames with
  its channel (HDLC for a dongle is done by the mux; the C6's SPI frames pass through). TX hands over one spinel frame;
  the mux never keeps the pointer past the call; TX after GONE fails at once. Events RX_FRAME (a frame-pool slot with
  a µs receive time taken at the transport), RESET (the mux reset or reopened the radio) and GONE (unplugged or dead)
  are posted into a queue the consumer hands over at attach time. There is no TX completion event. A consumer can ask
  for a channel reset (C6: GPIO reset; dongle: USB close and reopen), answered by RESET. Per-channel counters,
  including frame-pool drops, are readable by the consumer. A consumer attaches when its channel is present and in its
  mode. Spinel-level RCP reboots are not turned into RESET; the consumer sees them in its own parsing. In a build
  where no component has registered a `ziggurat` consumer, the mux refuses the reserved mode name (409
  `not_available`) and does not offer it. The mux has no Zigbee rules of its own: it enforces what the registered
  consumer declares (section 3, "Mux registration"). Zigbee is not part of the mux implementation; this work owns all
  of it (the user's scope, 2026-11-06). Every stick
  that appears starts `off`; the mux probes its firmware (spinel, ZNP, EZSP; a Ziggurat-on-stick firmware is told apart
  by its vendor property and fits only `rcp-proxy`), resets it, and switches it to its stored mode only if the firmware
  fits (`ziggurat` needs spinel); a PUT of a mode that does not fit gets 409 `incompatible`. So a Ziggurat consumer only
  ever sees a spinel RCP. Each channel reports a `firmware` object (type, version, chip, baud, source, time). The C6 is
  probed by rcp_sync as before. Nothing in the
  mux calls into the consumer from a transport task or an ISR, and nothing in it calls exit() or DieNow.
- Ziggurat is Apache-2.0; this project is AGPL-3.0-or-later. Apache-2.0 code can be combined into it; `NOTICE` gets
  the attribution.
- Ziggurat is in beta and changes quickly (several commits a day). Its binary protocol carries a version that zigpy
  checks.

## 3. Decisions

### What the user sees

- **Device > Radios** (the mux design's page): the channel list offers Ziggurat as a mode for each channel, because
  the `zigbee` component is built in. Choosing it asks for confirmation ("the board reboots; devices on this radio's current network lose it"),
  saves, and reboots. Offering Ziggurat on a second channel while one already has it is refused with a short reason.
- **Navigation, reorganised by protocol** (the user's call, 2026-11-06): the **Management** menu is renamed
  **Thread**, and **Topology** and **Routing** move into it, so the top bar reads Dashboard, Thread, Zigbee, Device,
  About. The Thread menu holds Network, Topology, Routing, Commissioner, Addresses, Tools and Console. Device keeps the
  board's own pages (Radios, Ethernet, Firmware, Diagnostics) and **Packet capture**, which only needs a radio with RCP
  firmware, not Thread (the user's call; to be revisited later). The regrouping itself is implemented by the mux plan
  (the user's call); this work only adds the Zigbee link. **Zigbee** is a top-level link to its one
  page. The change applies in every mode, so the menu looks the same whether or not Zigbee is in use.
- With a Ziggurat channel, the **Zigbee** link appears. The Thread pages follow the mux design's shared
  rule: with no channel in `otbr` at all (a deliberate Zigbee-only board), the Thread menu is hidden (Packet capture
  follows the mux design's rule for the sniffer), and opening one directly
  shows a notice linking to the Radios page; with an OTBR channel that is absent or faulted, those pages stay and show
  "no radio in OTBR mode". With a working OTBR channel too, both sets of pages are shown.
- The **Zigbee page** shows: which channel (radio) it runs on and that channel's detected firmware (the mux's
  `firmware` type, version and chip), stack state (starting, waiting for a client, running,
  radio resetting, no radio), the connected client (address, connected since) or none, the radio as last configured
  by the stack (channel, PAN ID, coordinator IEEE address, short address, TX power), radio counters (frames received,
  dropped, sent, failed, energy scans, RCP resets), and the stack's PSRAM use (current and peak). It also shows the ZHA
  setup text: radio type "Ziggurat", path `ws://<board-name>/zigbee`.
- *(Design call.)* The extended PAN ID and whether joining is open are not shown. The staticlib hands the firmware only
  what `radio_configure` carries; both values exist only inside ziggurat's wire protocol, and decoding that beta
  protocol in C would be a second implementation to keep in step with upstream. If upstream adds a status entry point
  later, the page can show them.
- The **Dashboard** shows a Zigbee summary card when a channel runs Ziggurat, next to or instead of the Thread card.
- With no OTBR channel, the board mode is the mux design's `NO_RADIO`, and the Thread REST API and the Thread Console
  WebSocket answer 503 as that design defines.

### Settings and API

- The channel's mode is the mux's setting and API; this design adds no stack or radio setting of its own.
- `GET /device/zigbee`: read-only status (the page's content) when a channel runs Ziggurat; 409 otherwise.
- `/device/status` gains `zigbee_channel` (the channel id or null), next to the mux design's `otbr_channel`; this
  design adds it.
- The channel API itself (`GET /device/channels`, `PUT /device/channels/{id}` with `mode` `ziggurat`, the reply's
  `"reboot": true`, 409 `mode_taken` and `hybrid_not_supported`) is the mux design's
  (`2026-11-06-radio-channel-mux-design.md` on `feat/radio-mux`).
- `/metrics` gains Zigbee counters with a `zigbee_` prefix, only while a channel runs Ziggurat.

### Board side: `br_core`

- With no channel in OTBR mode, OpenThread is not initialised at all. Nothing in Ziggurat mode touches the stored
  Thread datasets, because nothing that could is running.
- The board mode and the Thread API's refusals then follow the mux design's `NO_RADIO`.
- Everything else about which mode starts when, and how `br_core` hands a channel to its consumer, follows the mux
  design.

### Board side: `zigbee` component

- **Mux registration.** At startup the `zigbee` component registers the mux's consumer for the mode `ziggurat`,
  declaring: firmware type `spinel`; at most one channel; entering or leaving the mode reboots; and its attach and
  detach, with its event queue handed over at attach. The mux enforces these; a second `ziggurat` channel gets the
  mux's 409 `mode_taken`. The mux only delivers GONE and never reboots for it; the `zigbee` component falls back to
  "no radio" (below).
- New component `p4/components/zigbee`, prefix `zigbee_` (the staticlib owns `ziggurat_*`). It contains the platform
  layer, the spinel client, the WebSocket transport, the status, the heap shim and the prebuilt staticlib. The spike's
  `zigbee.c` is the reference for the platform functions that are not about the radio.
- **Its own task owns the stack.** A `zigbee` task (core and priority builder's call, next to the existing board-core
  layout) runs every `ziggurat_*` call and the spinel client. The esp_timer for the stack's timer, the mux's event
  queue and the WebSocket's inbound frames all post to this task; nothing else calls into ziggurat. Frames to the
  client go through a queue to a sender task, so the `zigbee` task never blocks on a socket.
- **Spinel client.** A small spinel client in C on OpenThread's plain-C spinel encoder and decoder (`lib/spinel`),
  talking to the channel's RCP through the mux's consumer API. It implements ziggurat's 12 radio operations and turns
  the RCP's frames back into ziggurat's 3 radio completions. Its scope, to be confirmed by the builder against
  OpenThread's own `RadioSpinel`:
  - at attach and after every RESET: reset the RCP, check the protocol version and the radio capabilities it needs
    (energy scan, ACK timeout, CSMA backoff, retransmit, source match / frame pending), read the EUI-64, then enable
    the PHY and the raw stream;
  - configure, channel, TX power, PAN ID, short and extended address (in the byte order the RCP expects, as
    `RadioSpinel` sends it), promiscuous mode, source-match enable and table edits, receive on the chosen channel,
    staying in receive between frames (where an RCP lacks rx-on-when-idle, as the CC2652P does, the client puts it
    back to receive after each transmit);
  - transmit through the raw stream with the per-frame metadata ziggurat passes (CSMA, retries, backoffs, channel),
    one frame in flight, completion mapped to ziggurat's five results;
  - energy scan on one channel for a duration, completing with the peak RSSI;
  - received raw-stream frames, FCS stripped, with channel, RSSI, LQI and timestamp.
  Requests carry transaction IDs and one timeout; a request that times out counts as a radio fault (below).
- **Received frames** arrive as slots of the channel's frame pool (zero copies before the client's parse on the C6,
  one on a dongle). The client parses a slot, hands the PSDU to ziggurat (which copies it) and releases the slot at
  once, so a slow stack never holds the pool. The receive timestamp ziggurat gets is the µs time the mux takes at the
  transport. Pool drops are counted by the mux and shown on the Zigbee page. The mux has no TX completion event; the
  transmit result comes from the RCP's own spinel reply.
- **Radio resets and loss.** *(Design call.)* The client keeps the last configuration and the source-match table.
  - On RESET from the mux, or an unsolicited spinel reset status from the RCP (it rebooted on its own): the client
    runs its attach sequence again and replays the configuration and the source-match table, then receives again. A
    transmit in flight completes as failed. The reset is counted and shown; the stack keeps running and the network
    keeps its children (they retry).
  - On a request timeout: the client asks the mux to reset the channel (the same channel reset OTBR's hardware reset
    uses: a GPIO reset on the C6, a reopen on a dongle) and then handles the RESET as above.
  - At boot, a stored Ziggurat channel whose dongle is not plugged in: the stack is not started until the mux attaches
    the channel when the device appears. So an unplugged dongle leaves a board waiting for it, not a reboot loop.
  - On GONE: *(design call, the user's choice 2026-11-06)* no reboot. The stack keeps running in a "no radio" state,
    like OTBR's `NO_RADIO`: a transmit in flight and every later one fails at once, a configure or source-match edit
    is stored (for the replay) but not sent, and an energy scan reports failure. The client connection stays up, and the
    network's state stays in ziggurat's memory. The page and `/device/zigbee` show "no radio". Nothing else on the
    board is touched: Ethernet, the web UI, other channels (and an OTBR channel, once hybrid exists) keep running.
  - When the radio comes back (the mux attaches the same channel again): the client runs its attach sequence and
    replays the stored configuration and source-match table, exactly as after RESET. The stack is not started a second
    time. Children that went quiet retry and rejoin as usual. (A different stick is a different channel id, so moving
    the network to it is a mode change, which reboots by the mode rule.)
  - C6 jobs (firmware push, ROM recovery) run in Ziggurat mode as in any mode (the mux design). During the job the
    consumer sees GONE and runs without a radio; afterwards the mux attaches it again and the client replays. No
    reboot. Attach is one path in the mux (boot, plug-in, return after GONE, mode change), so attach, GONE, attach
    again on the same channel is a normal case.
- **Memory in PSRAM.** Ziggurat's heap (its routing, child, neighbour and key tables, packet queues; everything it
  allocates) lives in PSRAM, so the network can grow to the size of a typical ZHA home (100+ devices) without taking
  internal RAM from lwIP, the HTTP server or an OTBR channel. The staticlib's `esp-idf` allocator is hard-wired to
  internal RAM (`heap_caps_aligned_alloc` / `heap_caps_realloc` / `heap_caps_free` with `MALLOC_CAP_INTERNAL`).
  *(Design call.)* Instead of forking it, the build script renames those three imports inside `libziggurat_ot.a`
  (`objcopy --redefine-sym`, local to the archive), and the `zigbee` component provides the renamed functions on top
  of `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`. The build script checks that the renamed symbols are the only heap imports
  left, so an upstream change cannot slip back into internal RAM unnoticed. The same change is offered upstream as an
  allocator option, which would retire the rename. Static data (about 5 KB) stays where the linker puts it.
- **Crypto.** AES-128 block on the hardware AES engine, CCM* on top of it (as in the spike) unless ESP-IDF's PSA API
  is a clean fit; tested against published CCM vectors either way.
- **Transport.** WebSocket `/zigbee` on the existing HTTP server, binary protocol frames as binary messages. One client
  at a time; a new connection takes over from the old one (zigpy reconnects on its own and must not be locked out by a
  dead socket). The stack's first frame (its `hello`) is kept and sent to every client that connects later. The socket
  is kept off the LRU purge while it is in use. The stack starts at boot, not on the first connection, so a running
  network keeps working (routing, children, frame pending) while ZHA restarts.
- **Backpressure and limits.** The outbound queue is bounded; a full queue makes `ziggurat_platform_host_send` return
  false and the stack is told when there is room again (the staticlib supports this). A frame larger than a fixed
  maximum closes the socket. With no client, outbound frames are dropped.
- **Panic and reset.** A ziggurat panic logs its message, records it so the next boot's `last_reset` reports it to the
  client (ziggurat sends it after `hello`), and aborts (core dump, reboot). A hard reset request from the client
  reboots the board.
- **Access.** Same trust model as the rest of the HTTP API: no login, anyone who reaches the board's web port can use
  the endpoint. The docs say so, as for the RCP proxy.

### Building ziggurat

- Upstream ziggurat is a git submodule at a pinned commit (`p4/components/zigbee/ziggurat`). Bumping it is a
  deliberate commit, checked against the zigpy-ziggurat version the tests pin.
- The `zigbee` component's CMake builds the staticlib with cargo as part of the normal build (`make build`), with the
  two fixes from section 2 applied by the build script (the radium patch through cargo's `--config patch`, and the
  removal of soft-float archive members) and the heap-symbol rename for PSRAM. Cargo runs on the host if it has the
  `riscv32imafc-unknown-none-elf` target, otherwise in the pinned `rust:<version>-slim` Docker image as in the spike.
  CI installs the target with rustup.
- The Rust toolchain version is pinned (`rust-toolchain.toml` in the component or the image tag), so release builds
  are reproducible.
- `NOTICE` gains ziggurat's attribution; `docs/building.md` gains the Rust prerequisite.

## 4. Testing

- **Host tests** (Unity on the `linux` target, `p4/components/zigbee/test_host/`):
  - spinel client against recorded and hand-built spinel frames: the attach sequence, every radio operation's request
    encoding, received-frame and transmit-done decoding (FCS stripped, results mapped), energy scan, a request
    timeout, RESET replay of configuration and source-match table, GONE (no-radio state: failures, stored edits),
    re-attach after GONE (replay, no second start);
  - CCM* against published vectors (encrypt, decrypt, a wrong MIC, a zero-length payload);
  - the heap shim (PSRAM only, alignment, counters);
  - `hello` capture and replay, client takeover, queue-full backpressure and the ready signal.
  Test seams only where a host test needs them (STYLEGUIDE); the mux consumer API is the natural seam for the client.
- **Target tests** (`p4/pytest/test_zigbee.py`), with zigpy-ziggurat pinned in the target test environment and the
  rig's aux C6 devkit as the Zigbee test device (the user's call, 2026-11-06), its on-board RGB LED being the light. The suite
  flashes it with Espressif's Zigbee on/off light example (esp-zigbee-sdk) when it is not already running it, and
  factory-resets it over its serial port so it joins on command. Thread suites that need the aux C6 as their `ot_cli`
  peer flash that back the same way; neither suite assumes what the other left on it. Run once with the C6 channel and
  once with the CC2652P dongle channel in Ziggurat mode:
  - mode switch: the channel set to Ziggurat, reboot, the Zigbee page and `/device/zigbee` report it, the Thread pages
    and API are refused when no OTBR channel is left; the C6 set back to OTBR, the board rejoins its stored Thread
    network with the dataset unchanged.
  - form a network from zigpy, energy scan all channels.
  - the aux C6 joins; zigpy interviews it; on/off commands from zigpy change the light; an attribute report from the
    light reaches zigpy.
  - zigpy disconnects and reconnects: `hello` within 5 s, the network and the joined device are still there.
  - a second client takes over from the first.
  - radio reset (the C6: `rcp_glitch`; the dongle: the consumer-requested channel reset, a USB close and reopen): the stack keeps running, the
    reset counter goes up, the client stays connected and the joined device still answers.
  - radio gone (the C6: `rcp_kill`; the dongle: unplug or a port power cut if the rig can do it): no reboot; the page
    shows "no radio", the client stays connected, Ethernet and the web UI keep working; zigpy requests fail cleanly.
  - radio back (the dongle plugged in again, or the C6 released): the stack replays, the joined device answers again
    without being paired again, and the board never rebooted (uptime unchanged).
  - memory: with the network formed and the test device joined, internal heap free is not lower than with the
    channel idle, and the stack's PSRAM use is reported.
  - hybrid (an OTBR channel and a Ziggurat channel at once), only if the mux release allows it.
- **Manual acceptance** before release: Home Assistant's ZHA set up against the board, a real device paired from ZHA
  and controlled, a ZHA restart, a board reboot.
- Hardware runs follow the usual budget: one batched pass after review and fixes.

## 5. Behaviour contract (BDD skeleton)

```
zigbee
    registration
        At startup
            - registers the mux consumer for mode ziggurat: firmware spinel, one channel, reboot on enter and leave,
              with attach and detach
    start
        At boot with a channel in Ziggurat mode
            - attaches to the channel through the mux with its own event queue
            - runs the spinel attach sequence (reset, version, capabilities, EUI-64, PHY and raw stream on)
            - starts the stack on the zigbee task before any client connects
            - keeps the stack's first outbound frame as its hello
        With the RCP missing a required capability or speaking another protocol version
            - does not start the stack
            - reports the radio as incompatible on the page and in /device/zigbee
        With no channel in Ziggurat mode
            - does nothing
        With the Ziggurat dongle's firmware detected as anything but spinel (ZNP, EZSP, Ziggurat-on-stick, unknown)
            - is never attached: the mux keeps the channel off with its reason and the stored mode
            - does not start the stack, and the Zigbee page shows the mux's reason
        With the Ziggurat channel reported unavailable (stored_mode ziggurat; dongle not detected, or radio not compatible)
            - does not start the stack
            - reports the channel as unavailable on the page and in /device/zigbee
            - starts the stack when the mux later attaches the channel (the dongle is plugged in)
    spinel client
        configure
            - sends channel, TX power, PAN ID, short and extended address, promiscuous, source-match enable and receive
            - keeps the configuration for replay
            - returns nonzero on an RCP error status
        source match
            - clears, adds short and adds extended entries on the RCP
            - keeps the table for replay
        transmit
            - sends one raw-stream frame with CSMA, retries, backoffs and channel
            - on the RCP's answer to it (parsed from RX_FRAME), reports acked, no ack, channel access failure, aborted
              or failed to the stack
            - with a frame in flight or longer than the radio allows, returns nonzero without sending
            - on an RCP without rx-on-when-idle, puts the radio back to receive afterwards
        energy scan
            - scans one channel for the duration and reports the peak RSSI to the stack
        received frame
            - passes the PSDU without FCS, with channel, RSSI, LQI and timestamp, to the stack
            - releases the pool slot before returning
        request timeout
            - asks the mux to reset the channel, or treats it as gone where the mux cannot
        On RESET from the mux, or an unsolicited spinel reset status from the RCP (an RCP reboot the mux did not cause)
            - runs the attach sequence again
            - replays configuration and source-match table and receives again
            - fails a transmit in flight
            - counts the reset; the stack keeps running
        On GONE
            - keeps the stack running and does not reboot
            - fails a transmit in flight and every later transmit and energy scan at once
            - stores configure and source-match edits without sending them
            - reports "no radio" on the page and in /device/zigbee
        On attach after GONE (the radio is back)
            - runs the attach sequence and replays the stored configuration and source-match table
            - does not start the stack again
            - reports the radio as running
    WebSocket /zigbee
        On connect while the stack runs
            - sends the kept hello first
            - makes this socket the client, replacing any previous one
        On a binary frame
            - passes it to the stack on the zigbee task
        On a frame over the maximum size
            - closes the socket
        On a non-binary frame
            - ignores it
        With no channel in Ziggurat mode
            - refuses the connection
    host send (from the stack)
        With a client and room in the queue
            - queues the frame for the client and returns true
        With a full queue
            - returns false, and signals the stack once the queue drains
        With no client
            - drops the frame and returns true
    ccm crypt
        Encrypt
            - writes ciphertext and MIC matching the reference vectors
        Decrypt with the right MIC
            - writes the plaintext and returns 0
        Decrypt with a wrong MIC
            - returns nonzero
    heap (the renamed staticlib allocator)
        On alloc, realloc and free
            - serves the request from PSRAM, never internal RAM
            - honours the requested alignment
            - counts current and peak bytes in use
        On PSRAM exhausted
            - returns NULL (the staticlib then panics)
    panic
        - logs the message, keeps it for the next boot's last-reset report and aborts
    status
        - counts frames received, dropped, sent, failed, energy scans and RCP resets
        - keeps the last radio configuration (channel, PAN ID, short and extended address, TX power), the channel it
          runs on, and the client's address and connect time

br_core
    boot
        With no channel in OTBR mode
            - does not initialise OpenThread
            - leaves the stored Active and Pending datasets unchanged

device_api
    GET /device/zigbee
        With a channel in Ziggurat mode
            - returns channel, state, client, last radio configuration, counters and PSRAM use
        Otherwise
            - answers 409
    GET /device/status
        - reports zigbee_channel (the channel id, or null)

web UI
    navigation
        In every mode
            - names the former Management menu "Thread" and lists Topology and Routing inside it
            - has no top-level Topology link and no Routing under Device; Packet capture stays under Device
        With a Ziggurat channel
            - shows the Zigbee link
        With no channel in otbr at all
            - hides the Thread menu
        With the OTBR channel absent or faulted
            - keeps those pages, showing "no radio in OTBR mode"
        With only OTBR channels
            - shows Dashboard, Thread, Device and About
    hidden page opened directly
        - shows a notice with a link to the Radios page
    Radios page, channel mode
        On choosing Ziggurat while another channel has it
            - refuses with a reason
```

## 6. Left to the builder

- The Zigbee page layout, and how the navigation learns the channel modes (from `/device/status` or a value baked into
  the page).
- The `zigbee` task's core, priority and stack size.
- The exact spinel property set and attach sequence, confirmed against OpenThread's `RadioSpinel`, and which
  capabilities are required versus worked around per RCP.
- Whether CCM* uses PSA or stays on the AES engine.
- The outbound queue depth and the maximum frame size.
- Exact metric names.
- How the e2e tests factory-reset and drive the aux C6 (its serial console or a GPIO), and which esp-zigbee-sdk example
  version they pin.

## 7. Risks

- **Dependency on the mux.** Ziggurat mode ships with the mux. The consumer API above is what the mux design committed
  to; if it changes, the spinel client's seam changes with it. The mux design's consumer-API section is reviewed from
  this side before either is planned.
- **Our own spinel client.** It is new code where the spike leaned on OpenThread's. Mitigation: a small property set,
  host tests against recorded frames, and both radios (C6 and CC2652P) in the target tests. Dongle quirks (the
  CC2652P's missing rx-timing and rx-on-when-idle) are handled by capability, not by device name.
- **Upstream churn.** Ziggurat is in beta. A submodule bump can change the platform functions, the protocol version
  zigpy expects, or the build. Mitigation: pin both ziggurat and zigpy-ziggurat, bump them together, and run the target
  tests on every bump.
- **PSRAM speed.** PSRAM is slower than internal RAM, and every frame the stack handles touches its heap. Ziggurat's
  timing-critical work (acks, CSMA, retries) is done by the RCP, and the P4's cache hides most of the difference, so
  this should not show; the target tests' latency (zigpy request to device and back) is the check.
- **The two build fixes.** If a Rust or ziggurat update changes radium or compiler-rt, the build fails loudly (the
  script checks its patch applied). Offer the radium target fix upstream; ask ziggurat upstream whether it wants a
  P4 target, which would retire both fixes.
- **One task, two cores.** Any `ziggurat_*` call made off the `zigbee` task is a data race the compiler cannot see.
  The code review checks every entry point.
- **Rust toolchain in the build.** A second toolchain raises the bar for contributors; Docker as the fallback and the
  documented prerequisite keep it workable.
