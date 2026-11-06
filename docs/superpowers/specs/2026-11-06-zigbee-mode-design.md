# Zigbee mode: the board as a Zigbee coordinator with ziggurat: design

Date: 2026-11-06. Status: draft for review. Spike: branch `spike/ziggurat-p4` (b190cfb), throwaway.

## 1. Purpose and scope

The same board and the same firmware image can be either a Thread border router (today) or a Zigbee coordinator. In
Zigbee mode the P4 runs [zigpy/ziggurat](https://github.com/zigpy/ziggurat), an open-source Zigbee stack in Rust, and
drives the C6 through its unchanged OpenThread RCP firmware over the existing SPI link. ZHA (or any zigpy application)
connects to the board with radio type "Ziggurat" and the path `ws://<board>/zigbee`.

Why: one PoE board, one firmware, one radio, for either kind of mesh. Ziggurat is designed around OpenThread RCP
radios, so the C6, its updates and its repair paths stay exactly as they are.

### Goals

- A stored **stack** setting, `thread` (default) or `zigbee`, chosen on the Device configuration page. Saving a new
  value reboots the board, and the board boots into that stack.
- Ziggurat runs on either radio: the on-board C6, or a USB RCP dongle on the OTG port (the CC2652P on the rig). The
  radio is a stored setting next to the stack, `c6` (default) or `usb`, and the same choice serves OTBR. *(Design
  call.)* Ziggurat always reaches its radio through OpenThread's raw link; what changes with the setting is which radio
  OpenThread drives. So the `zigbee` component has one radio backend, and the USB path is the one OTBR gets from the
  USB OTG work's channel piping (2026-11-06). The C6 path ships first; USB lands when that piping does, without
  changes to the `zigbee` component.
- In Zigbee mode, ziggurat is a real mode of the board: it starts at boot, it survives client disconnects, it recovers
  from radio failures, and it shows its state on its own page and in the API.
- The web UI follows the stack: Thread pages are hidden in Zigbee mode and Zigbee pages appear; the Thread API answers
  that it is unavailable. Everything about the board itself (Ethernet, network config, updates, C6 firmware and repair,
  diagnostics, metrics, names, About) works the same in both modes.
- Switching to Zigbee and back leaves the stored Thread network (datasets, settings) untouched.
- Done means: Zigbee mode runs as a production mode, and on-board end-to-end tests through zigpy pass: forming a
  network, a real device joining, commands and attribute reports both ways, a client reconnecting, the stack switch
  round trip. One manual acceptance pass with Home Assistant's ZHA closes it.

### Non-goals (this design)

- **Hybrid mode** (Thread and Zigbee at the same time, one on each radio). It comes later, on top of the channel
  piping. This design only keeps the door open (section 3, "Radio seam").
- The USB radio path itself (spinel over the OTG port, dongle quirks such as the CC2652P's missing rx-timing and
  rx-on-when-idle capabilities): that is the USB OTG work. This design consumes it.
- Separate firmware images per stack. One image for now; splitting later stays possible because the Zigbee code is one
  component plus pages.
- Zigbee features above the stack: no device list, no ZCL, no pairing UI on the board. ZHA owns all of that; the
  board is a radio and a stack, like a Zigbee dongle.
- Packet capture in Zigbee mode (the sniffer needs the radio that ziggurat holds), Zigbee OTA of end devices, Green
  Power beyond what ziggurat passes through, Zigbee R23.
- Home Assistant discovery of the Zigbee endpoint (zeroconf). ZHA is set up by hand with the path.
- Switching stacks without a reboot.

### Anti-goals (a failure even if it works)

- Any path through which Zigbee mode erases or rewrites the stored Thread network.
- Forking ziggurat. Upstream is consumed at a pinned commit; local changes are limited to build fixes that are also
  offered upstream.
- A Zigbee mode that silently half-works: a radio that stopped, a stack that panicked or a client that cannot connect
  must show on the page and in `/device/status`, not only in the serial log.

## 2. Facts this design rests on

From the spike and its on-board runs (2026-11-06):

- Ziggurat ships its whole stack as a C ABI staticlib for microcontrollers (`crates/ziggurat-ot`, embassy runtime, no
  std), meant to be linked into OpenThread RCP firmware. It needs 20 `ziggurat_platform_*` functions from the firmware:
  time and a one-shot timer, a wake-up, entropy, AES-128 block and CCM*, a host send, 12 radio operations, the EUI-64,
  reset, panic and post-mortem. It calls them, and expects its own `ziggurat_*` entry points to be called, from one
  thread of control.
- It builds for `riscv32imafc-unknown-none-elf` (the P4's ISA and single-float ABI) with stable Rust 1.99, with two
  local fixes: radium 0.7 (via bitvec) does not know that target lacks 64-bit atomics, and the target's prebuilt
  compiler-rt C objects are soft-float ABI, which ld refuses next to ESP-IDF's code (libgcc provides the same
  functions).
- The P4's OpenThread host build already enables the raw link API (the sniffer uses it), and over the spinel driver
  that API reaches the C6 RCP. Ziggurat's radio operations map onto it one to one.
- `otLinkSetPanId` clears the Active and Pending Operational Datasets, so it must never be called in Zigbee mode; the
  radio's PAN ID and extended address are set through the radio platform API instead (extended address byte-reversed,
  as OpenThread's own radio wrapper does).
- With the raw link enabled, the radio has to be enabled again and set to receive when idle, or the RCP stops after one
  frame (already known from the sniffer, `br_core` `op_raw_enable`).
- The staticlib's critical sections mask interrupts on the current hart only. The P4 has two cores, so every call into
  it must stay on one task.
- zigpy-ziggurat (1.2.1) speaks the binary protocol over WebSocket, one frame per binary message, and waits up to 5 s
  for a `hello` frame after every connect. `ziggurat-server` sends one per connection; the embedded stack sends one only
  when it starts. A later `hello` tells zigpy the firmware reset.
- Measured on the spike build: app 2.3 MB of an 8 MB slot (ziggurat about 300 KB); static internal RAM 203 KB of 572 KB
  (ziggurat 5 KB); internal heap free 353 KB in Thread mode and 340 KB in Zigbee mode with a formed network, so about
  13 KB at run time with no devices. PSRAM is about 31 MB free in both.
- On the board, the spike formed a network, ran an energy scan on all 16 channels, and a Tuya TS0041 (a sleepy end
  device) joined and was interviewed by zigpy. A reboot returned the board to its Thread network.
- Ziggurat is Apache-2.0; this project is AGPL-3.0-or-later. Apache-2.0 code can be combined into it; `NOTICE` gets
  the attribution.
- Ziggurat is in beta and changes quickly (several commits a day). Its binary protocol carries a version that zigpy
  checks.

## 3. Decisions

### What the user sees

- **Device > Configuration** (an existing Device page gets a "Radio stack" card, or a new page if none fits; builder's
  call) shows the current stack and a choice of Thread or Zigbee. Changing it asks for confirmation ("the board
  reboots; Thread devices lose this border router" or "Zigbee devices lose their coordinator"), saves, and reboots.
- In **Zigbee mode** the navigation shows Dashboard, a **Zigbee** page, the Device group without Routing and Packet
  capture, and About. The Management group (Network, Commissioner, Addresses, Tools, Console) and Topology are hidden.
  Opening a hidden page directly shows a short notice with a link to the Zigbee page instead of an error.
- The **Zigbee page** shows: stack state (starting, waiting for a client, running, radio fault), the connected client
  (address, connected since) or none, the radio as last configured by the stack (channel, PAN ID, coordinator IEEE
  address, short address, TX power), and radio counters (frames received, dropped, sent, failed, energy scans). It also
  shows the ZHA setup text: radio type "Ziggurat", path `ws://<board-name>/zigbee`.
- *(Design call.)* The extended PAN ID and whether joining is open are not shown. The staticlib hands the firmware only
  what `radio_configure` carries; both values exist only inside ziggurat's wire protocol, and decoding that beta
  protocol in C would be a second implementation to keep in step with upstream. If upstream adds a status entry point
  later, the page can show them.
- The **Dashboard** in Zigbee mode shows the same board cards as today and a Zigbee summary card in place of the
  Thread one.
- The **Thread Console** WebSocket and the Thread REST API answer 409 with the detail `zigbee mode`, like the sniffer
  mode refusals today.

### Stack setting

- Stored in NVS as part of the board's own settings (the builder picks the namespace next to the existing device
  settings). Missing or unreadable means `thread`, so every existing board keeps booting as today.
- API: `GET /device/stack` returns the stored and the running stack and radio; `PUT /device/stack` with
  `{"stack":"zigbee"|"thread", "radio":"c6"|"usb"}` (either field optional) stores it and reboots, the way other
  reboot-requiring device settings answer (the response goes out before the reboot). Same values as running: 200, no
  reboot. `radio` `usb` is refused (409) until the USB radio path exists. If the USB OTG work defines its own radio
  setting first, this design uses that one instead of adding a second.
- `/device/status` gets the running stack, and `mode` gains `ZIGBEE`.

### Board side: `br_core`

- A new mode, `BR_MODE_ZIGBEE`, next to `BR_RUNNING` and `SNIFFER`. It replaces the spike's reuse of `SNIFFER`.
- Boot in Zigbee mode: the radio comes up exactly as it does for Thread with the same radio setting (on the C6: probe,
  compatibility check, ROM recovery, C6 firmware update all unchanged; on USB: whatever the USB OTG work defines), and
  OpenThread is initialised on that radio, because ziggurat reaches the radio through OpenThread's spinel driver. Then, instead of starting Thread, `br_core` keeps the Thread netif down, never enables
  Thread, enables the raw link the way the sniffer does, and hands the radio to the `zigbee` component.
- Everything Thread-specific that `br_core` starts after OpenThread (border routing, TREL, SRP, NAT64, prefix
  delegation, mDNS records for the border router) is not started in Zigbee mode. The board's own mDNS hostname stays.
- C6 jobs (firmware push, ROM recovery) stay allowed in Zigbee mode, from the same pages. They stop OpenThread as they
  do today; afterwards the board reboots, which brings Zigbee mode back. (Restarting ziggurat in place is not worth it.)
- An RCP failure in Zigbee mode (spinel timeout, RCP reset that OpenThread cannot recover) enters `RADIO_FAULT` as in
  Thread mode, and the board reboots after a short delay so Zigbee mode comes back on its own. ZHA reconnects.
- *(Design call.)* An RCP reset that OpenThread recovers on its own (the C6 resets, OpenThread restores it, no fault
  is raised; see the sniffer's `test_rcp_reset_while_capturing_is_survived`) also reboots the board in Zigbee mode.
  Whether OpenThread's restore brings back the raw-link state ziggurat set (PAN ID, addresses, frame-pending table,
  promiscuous mode, channel) is unknown, and a radio that quietly lost it is the half-working state the anti-goals
  forbid. `br_core` therefore watches for the RCP reset in Zigbee mode and treats it like a failure. Re-applying the
  state in place is a later optimisation, once an on-board check shows what OpenThread restores.
- *(Design call.)* Fault reboots are capped like Thread mode's recoveries (`MODE_FSM_MAX_RECOVERIES` within its
  window): a count that survives the reboot (RTC no-init memory or NVS, builder's call) stops the loop. Past the cap
  the board stays in `RADIO_FAULT` with Ethernet, the web UI and OTA working, as a C6 that keeps failing does today.
- The Thread mode guard (patch 2 in `PATCHES.md`) learns the new mode: Thread changes and radio use are refused with
  409 `zigbee mode`.

### Board side: `zigbee` component

- New component `p4/components/zigbee`, prefix `zigbee_` (the staticlib owns `ziggurat_*`). It contains the platform
  layer, the radio backend, the transport, the status, and the prebuilt staticlib. The spike's `zigbee.c` is the
  reference for every platform function.
- **One task owns the stack.** Every `ziggurat_*` call runs on the OpenThread task, posted through its task queue, as
  in the spike. The timer is an esp_timer whose callback posts to that task. Nothing on the OpenThread task calls lwIP
  (STYLEGUIDE): frames to the client go through a queue to a sender task.
- **Radio seam.** The 12 radio operations and their 3 completions (receive, transmit done, energy scan done) form an
  ops table, `zigbee_radio_ops_t`, chosen at start. There is one backend now, `otlink` (OpenThread raw link on the C6,
  as in the spike). Hybrid mode later adds a second backend for a radio OpenThread does not own; how that radio's
  spinel channel reaches the `zigbee` component is the job of the channel piping architecture the USB OTG work is
  designing (2026-11-06), not of this component. Either stack can end up on either radio; nothing in the `zigbee`
  component may assume the C6.
- **Memory in PSRAM.** Ziggurat's heap (its routing, child, neighbour and key tables, packet queues; everything it
  allocates) lives in PSRAM, so the network can grow to the size of a typical ZHA home (100+ devices) without taking
  internal RAM from OpenThread, lwIP and the HTTP server. The staticlib's `esp-idf` allocator is hard-wired to internal
  RAM (`heap_caps_aligned_alloc`/`heap_caps_realloc`/`heap_caps_free` with `MALLOC_CAP_INTERNAL`). *(Design call.)*
  Instead of forking it, the build script renames those three imports inside `libziggurat_ot.a` (`objcopy
  --redefine-sym`, local to the archive), and the `zigbee` component provides the renamed functions on top of
  `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`. A PSRAM allocation failure is a panic, as it is for the staticlib today. The
  build script checks that the renamed symbols are the only heap imports left, so an upstream change cannot slip back
  into internal RAM unnoticed. The same change is offered upstream as an allocator option, which would retire the
  rename. Static data (about 5 KB) stays where the linker puts it. The Zigbee page and `/metrics` show the stack's
  current and peak PSRAM use.
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
- **Status and metrics.** The component keeps the counters and the last radio configuration for the Zigbee page,
  `/device/zigbee` (GET, read-only status) and `/metrics` (Zigbee counters with a `zigbee_` prefix, only in Zigbee
  mode).
- **Access.** Same trust model as the rest of the HTTP API: no login, anyone who reaches the board's web port can use
  the endpoint. The docs say so, as for the USB serial proxy.

### Building ziggurat

- Upstream ziggurat is a git submodule at a pinned commit (`p4/components/zigbee/ziggurat`). Bumping it is a
  deliberate commit, checked against the zigpy-ziggurat version the tests pin.
- The `zigbee` component's CMake builds the staticlib with cargo as part of the normal build (`make build`), with the
  two fixes from section 2 applied by the build script (the radium patch through cargo's `--config patch`, and the
  removal of soft-float archive members) and the heap-symbol rename for PSRAM. Cargo runs on the host if it has the `riscv32imafc-unknown-none-elf` target,
  otherwise in the pinned `rust:<version>-slim` Docker image as in the spike. CI installs the target with rustup.
- The Rust toolchain version is pinned (`rust-toolchain.toml` in the component or the image tag), so release builds
  are reproducible.
- `NOTICE` gains ziggurat's attribution; `docs/building.md` gains the Rust prerequisite.

## 4. Testing

- **Host tests** (Unity on the `linux` target, `p4/components/zigbee/test_host/`): stack setting parsing and defaults;
  CCM* against published vectors (encrypt, decrypt, a wrong MIC, a zero-length payload); radio configuration
  (extended address byte order, never the dataset-clearing call); `hello` capture and replay; client takeover;
  queue-full backpressure and the ready signal. Test seams only where a host test needs them (STYLEGUIDE).
- **Target tests** (`p4/pytest/test_zigbee.py`), with zigpy-ziggurat pinned in the target test environment and the
  rig's aux C6 devkit as the Zigbee test device (the user's call, 2026-11-06: it is free for testing). The suite
  flashes it with Espressif's Zigbee on/off light example (esp-zigbee-sdk) when it is not already running it, and
  factory-resets it over its serial port so it joins on command. Thread suites that need the aux C6 as their `ot_cli`
  peer flash that back the same way; neither suite assumes what the other left on it.
  - stack switch: `PUT /device/stack` zigbee, reboot, `mode` ZIGBEE, Thread pages and API refused; back to thread, the
    board rejoins its stored Thread network with the dataset unchanged.
  - form a network from zigpy, energy scan all channels.
  - the same suite with the radio setting `usb` and the CC2652P, once the channel piping is in.
  - the aux C6 joins; zigpy interviews it; on/off commands from zigpy change the light; an attribute report from the
    light reaches zigpy.
  - zigpy disconnects and reconnects: `hello` within 5 s, the network and the joined device are still there.
  - a second client takes over from the first.
  - C6 failure at run time (`rcp_kill`, as in the sniffer tests, with no boot-time hold): `RADIO_FAULT`, then the
    board reboots back into Zigbee mode.
  - C6 reset that OpenThread recovers (`rcp_glitch`): the board reboots back into Zigbee mode, the client reconnects
    and the joined device still answers.
  - repeated faults: past the cap the board stays in `RADIO_FAULT` and serves its web UI.
  - memory: with the network formed and the test device joined, internal heap free stays within a few KB of Thread
    mode's, and the stack's PSRAM use is reported.
- **Manual acceptance** before release: Home Assistant's ZHA set up against the board, a real device paired from
  ZHA and controlled, a ZHA restart, a board reboot.
- Hardware runs follow the usual budget: one batched pass after review and fixes.

## 5. Behaviour contract (BDD skeleton)

```
br_core
    boot
        With stack zigbee stored
            - initialises OpenThread on the C6 and leaves Thread disabled and the netif down
            - enables the raw link with the radio receiving when idle
            - enters BR_MODE_ZIGBEE and starts the zigbee component
            - starts none of border routing, TREL, SRP, NAT64, prefix delegation
            - leaves the stored Active and Pending datasets unchanged
        With no stack stored or an unreadable value
            - boots as stack thread
        With stack zigbee and radio usb stored
            - initialises OpenThread on the USB radio instead of the C6, and otherwise boots as with radio c6
        With the radio failing to start
            - enters RADIO_FAULT as in Thread mode
    rcp failure
        In BR_MODE_ZIGBEE
            - enters RADIO_FAULT
            - reboots after the fault delay
        In BR_MODE_ZIGBEE past the recovery cap
            - stays in RADIO_FAULT and does not reboot
    rcp reset recovered by OpenThread
        In BR_MODE_ZIGBEE
            - reboots, counted against the same cap
    radio job (firmware push, ROM recovery)
        In BR_MODE_ZIGBEE
            - is allowed
            - reboots after the job

device_api
    GET /device/stack
        - returns the stored and the running stack
    PUT /device/stack
        With a different valid stack
            - stores it, answers 200, then reboots
        With the running stack
            - answers 200 and does not reboot
        With an unknown value or a malformed body
            - answers 400 and stores nothing
    GET /device/zigbee
        In Zigbee mode
            - returns state, client, last radio configuration and counters
        In Thread mode
            - answers 409
    GET /device/status
        - reports the running stack, and mode ZIGBEE in Zigbee mode
    Thread REST, Web GUI handlers and the console WebSocket
        In Zigbee mode
            - answer 409 with detail "zigbee mode"

zigbee
    start
        At boot in Zigbee mode
            - starts the stack on the OpenThread task before any client connects
            - keeps the stack's first outbound frame as its hello
    WebSocket /zigbee
        On connect while the stack runs
            - sends the kept hello first
            - makes this socket the client, replacing any previous one
        On a binary frame
            - passes it to the stack on the OpenThread task
        On a frame over the maximum size
            - closes the socket
        On a non-binary frame
            - ignores it
        In Thread mode
            - refuses the connection
    host send (from the stack)
        With a client and room in the queue
            - queues the frame for the client and returns true
        With a full queue
            - returns false, and signals the stack once the queue drains
        With no client
            - drops the frame and returns true
    radio configure (otlink backend)
        On success
            - sets channel, PAN ID, short address, extended address (byte-reversed), TX power and promiscuous mode
            - enables source matching and receives
            - never calls otLinkSetPanId
        On an OpenThread error
            - returns nonzero
    radio transmit
        On completion
            - reports acked, no ack, channel access failure, aborted or failed to the stack
        With a frame longer than the radio allows
            - returns nonzero without transmitting
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
        - counts frames received, dropped, sent, failed and energy scans
        - keeps the last radio configuration (channel, PAN ID, short and extended address, TX power) and the client's
          address and connect time

web UI
    navigation
        In Zigbee mode
            - shows Dashboard, Zigbee, Device (without Routing and Packet capture) and About
        In Thread mode
            - is unchanged
    hidden page opened directly
        - shows a notice with a link to the Zigbee page
    Device configuration, radio stack card
        On a change
            - asks for confirmation, sends PUT /device/stack and shows the reboot progress
```

## 6. Left to the builder

- Where the stack card lives on the Device pages, the Zigbee page layout, and how the navigation learns the stack
  (from `/device/status` or a value baked into the page).
- The NVS namespace and key for the stack setting.
- Whether CCM* uses PSA or stays on the AES engine.
- The outbound queue depth and the maximum frame size.
- Exact metric names.
- How the e2e tests factory-reset and drive the aux C6 (its serial console or a GPIO), and which esp-zigbee-sdk example
  version they pin.

## 7. Risks

- **Dependency on the channel piping.** The USB radio path waits for the USB OTG work. If that work does not let
  OpenThread drive a USB radio at run time (the dongle spike chose UART spinel with a build-time sdkconfig overlay),
  ziggurat on USB needs its own spinel backend after all, as hybrid mode does.

- **Upstream churn.** Ziggurat is in beta. A submodule bump can change the platform functions, the protocol version
  zigpy expects, or the build. Mitigation: pin both ziggurat and zigpy-ziggurat, bump them together, and run the target
  tests on every bump.
- **PSRAM speed.** PSRAM is slower than internal RAM, and every frame the stack handles touches its heap. Ziggurat's
  timing-critical work (acks, CSMA, retries) is done by the RCP, and the P4's cache hides most of the difference, so
  this should not show; the target tests' latency (zigpy request to device and back) is the check.
- **The two build fixes.** If a Rust or ziggurat update changes radium or compiler-rt, the build fails loudly (the
  script checks its patch applied). Offer the radium target fix upstream; ask ziggurat upstream whether it wants a
  P4 target, which would retire both fixes.
- **RCP reset recovery.** OpenThread restores the RCP after a C6 reset for its own MAC; whether the raw-link state set
  by ziggurat survives is unknown, hence reboot as the recovery.
- **One task, two cores.** Any `ziggurat_*` call made off the OpenThread task is a data race the compiler cannot see.
  The code review checks every entry point.
- **Rust toolchain in the build.** A second toolchain raises the bar for contributors; Docker as the fallback and the
  documented prerequisite keep it workable.

## 8. Future: hybrid mode

With a second RCP (the CC2652P on the USB OTG port, which already runs OTBR through the USB serial work and a UART
wrapper), the board can run both stacks at once: OpenThread on one radio and ziggurat on the other, in either
assignment. What this design leaves ready for it: the radio seam in the `zigbee` component (a spinel backend for the
radio OpenThread does not own), the stack setting (gains a third value), and `br_core` modes that already separate
"Thread runs" from "Zigbee runs". What hybrid needs on top comes from the channel piping architecture the USB OTG
branch is working out (2026-11-06): how a radio's spinel channel (C6 over SPI, a dongle over USB) is piped to whichever
stack owns it. On the Zigbee side that means a second `zigbee_radio_ops_t` backend that drives ziggurat's radio
functions over a piped spinel channel (or upstream's spinel phy ported to the embassy runtime), shaped to fit that
architecture once it is written down. Until then this design adds nothing for it beyond the radio seam.
