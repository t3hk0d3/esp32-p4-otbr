# USB Serial Proxy: a USB serial dongle served over TCP: design

Status: draft for review, 2026-11-06 (revised after an adversarial review). Proof of concept, experimental.

## 1. Purpose and scope

The long-term goal is support for RCP USB dongles on this board. This proof of concept takes the smallest useful
slice of it: a USB serial dongle plugged into the board's USB-A (OTG) port is served, byte for byte, on a TCP port,
like `ser2net` on a Raspberry Pi. The proxy does not care what runs over the serial line. A host on another machine
drives the dongle as if it were plugged in locally, for example:

- a Zigbee stack (ZHA/zigpy, Zigbee2MQTT) talking ZNP or EZSP to a Zigbee coordinator firmware;
- `otbr-agent` on Linux, or zigpy's Ziggurat, talking spinel to an OpenThread RCP firmware.

The board's own border router on the C6 is not involved and keeps running the whole time. The P4's OpenThread stack
never talks to the dongle.

Test hardware (user's): an SMLIGHT SLZB-06 in USB mode. Its USB bridge is a Silicon Labs CP2102N, the radio a TI
CC2652P, running SMLIGHT's Zigbee (ZNP) firmware or, switched with SMLIGHT's flasher, its Thread RCP firmware.

Success: a Zigbee stack on another machine (ZHA or Zigbee2MQTT, `socket://<board>:6638` / `tcp://<board>:6638`)
reaches the SLZB-06 through the board as it ships, forms a Zigbee network and a device joins it; the loopback check
(section 4) passes with no corrupted byte; the C6 border router shows no change against its baseline during the
test. Spinel through the same proxy (`otbr-agent` via `socat`) is a second check, run if the SLZB-06 is switched to
its Thread firmware *(my call: Zigbee first, because the dongle already runs it and Zigbee stacks speak TCP natively,
so no reflash and no pty are needed)*.

Goals:

- Transparent byte bridge between the dongle's USB serial port and one TCP client. No knowledge of the protocol on
  top: no spinel, HDLC, ZNP or EZSP parsing.
- Never corrupts the stream silently: a byte that cannot be delivered ends the session, it is not skipped.
- Dongles with CP210x, CH34x or CDC-ACM USB bridges, using the USB host drivers the build already pulls in.
- Serial settings (baud rate, hardware flow control) and the TCP port stored as settings; off by default.
- Plugging, unplugging and replugging the dongle, clients coming and going, and turning the proxy on and off never
  need a reboot and never crash the board.

Non-goals (for this PoC):

- The C6 behind the proxy, or the P4's own OpenThread host using the dongle.
- Flashing dongle firmware, or resetting the dongle by any means other than what the client sends over the line.
- Advertising the port over mDNS (zigpy's and SMLIGHT's `_slzb-06._tcp` style discovery).
- A web UI page. The setting and status are reachable through the device API only.
- RFC 2217 (remote serial port control): the client cannot change baud rate or toggle DTR/RTS over the network.
- Authentication or encryption on the TCP port (see Access below for what limits it instead).
- USB hubs (`USB_HOST_HUBS_SUPPORTED` is off), more than one dongle, or more than one client at a time.
- Connect mode (the board dialling out to a configured host). It is the production direction (section 5); the PoC
  ships listen mode only.

Anti-goals (would make it a failure even if it works):

- Anything the proxy does disturbing the C6 border router: OpenThread timing, the web server, TREL, captures, the
  radio itself (RF next to the C6 antenna, power on the shared 5 V rail).
- A stalled or slow TCP client blocking USB reads, or a misbehaving dongle blocking the network stack.
- A dead client (laptop asleep, stack killed) holding the dongle until the board reboots.
- Silent corruption: spliced frames reaching either side.
- Two clients fighting over the dongle, each re-initialising the radio in turn.

## 2. Facts this design rests on

Checked in this repository and the components it uses:

- `p4/dependencies.lock` already contains `espressif/usb` (1.6.0), `espressif/usb_host_cdc_acm` (2.4.1),
  `espressif/usb_host_cp210x_vcp` and `espressif/usb_host_ch34x_vcp` (2.2.0), pulled in privately by
  `espressif/esp-serial-flasher` for the P4 target (which itself uses its UART port, not USB). The proxy component
  declares them itself; it must not rely on the flasher's private dependency. The three VCP "drivers" are one
  `cdc_acm_host` client with three open functions.
- `usb_host_install` picks the High-Speed OTG peripheral by default. The secondary console
  (`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`, `p4/sdkconfig`) sits on the separate full-speed PHY, so the HS
  OTG peripheral is free.
- `cdc_acm_host_close` and its endpoint reset and interface release use `ESP_ERROR_CHECK`
  (`cdc_acm_host.c` around 748–756): a close racing an unplug can abort the whole board. A device must not be opened
  from `new_dev_cb` (`cdc_acm_host.h`).
- The CDC data callback runs in the CDC driver task, and the next bulk IN transfer is submitted only after it
  returns; on the P4 (`SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE`) the callback must consume everything (no append).
- The CH34x driver covers PIDs 7522, 7523 and 5523 only. The CDC fallback would open any device with two bulk
  endpoints (FTDI, mass storage).
- lwIP limits are already used up exactly: `CONFIG_LWIP_MAX_SOCKETS=18` = web 10 + routing 7 + HTTP client 1,
  enforced by `SOCKETS_NEEDED` in `routing/task.c`; `CONFIG_LWIP_MAX_ACTIVE_TCP=16` is used up too. An existing
  `p4/sdkconfig` keeps its old values (see `docs/development.md`).
- lwIP's TCP keepalive defaults to two hours; the TCP send buffer is 5760 bytes (`CONFIG_LWIP_TCP_SND_BUF_DEFAULT`).
- The OpenThread task runs on core 0; on core 1 the web server (priority 5) and the device API workers, including
  `pcap_stream` and `console_ws`, run at priority 5.
- Per-interface access: `netcfg_policy_allows` returns true for everything in single mode (`policy.c`), including
  addresses on the OpenThread netif, which `netcfg/ifaces.c` maps to the Thread side. The side lookup exists only for
  an HTTP request (`netcfg_ifaces_request_side(httpd_req_t *)`, `netcfg_glue.h`), and `match_v4`/`match_v6` read
  `s_net` without a lock.
- The Thread console's settings route (`/device/console`, `ot_console` component) is the pattern for an
  off-by-default stored setting with a one-session status.

Checked in SMLIGHT's open SLZB-06 ESP32 firmware (<https://github.com/smlight-dev/slzb-06-firmware>, v0.9.9; the
current v2.x firmware is closed):

- In USB mode the SLZB-06's own ESP32 drives `MODE_SWITCH` (GPIO 4) high, which routes the CC2652P's UART to the
  CP2102N. The firmware only defines RX/TX pins for the radio and advertises `data_flow_control: software` over mDNS.
  This *suggests* no RTS/CTS between the radio and the CP2102N; firmware cannot prove the board wiring.
- The CC2652P's reset (GPIO 33) and bootloader pin (GPIO 32) belong to the SLZB-06's ESP32. But SMLIGHT flashes that
  ESP32 with esptool through the CP2102N (`tools/flash.sh`), which implies the usual auto-reset wiring: **the
  CP2102N's DTR/RTS reset the SLZB-06's own ESP32**. An ESP32 reset drops `MODE_SWITCH` to 0 at boot (the radio UART
  is disconnected until USB mode is restored from its config) and re-drives the radio's reset pin.
- SMLIGHT's serial defaults are 115200 baud and TCP port 6638 (its Zigbee firmware). The baud rate of its Thread RCP
  firmware for the CC2652P is not in the open source.
- The SLZB-06 is bus-powered in USB mode, ESP32 and Ethernet PHY included.

Not yet checked (step 1 on the board checks them):

- That the Type-A port is wired to the P4's HS OTG peripheral (Waveshare's wiki says so; nothing in this repository
  shows it), whether VBUS is always on or behind a GPIO-controlled switch, and whether PoE leaves enough budget for
  the SLZB-06 (likely 150–300 mA, unmeasured).
- That opening the port with DTR and RTS set together leaves the SLZB-06's ESP32 running (its uptime does not reset).
- The baud rate of the SLZB-06's Thread RCP firmware, needed only for the spinel check.

## 3. Decisions

Calls marked *(my call)* were not made by the user; they can be changed in review.

### Behavior

- **Off by default.** Turning it on starts the USB host and the TCP listener; turning it off stops both, closes any
  client, and releases the USB host stack completely (see Board side for the order).
- **Dongle handling.** When a USB device appears, the proxy picks a driver by its USB IDs (CP210x, CH34x), else falls
  back to CDC-ACM only for a device that declares the CDC classes (communications 0x02 / data 0x0A). Everything else
  (FTDI, CH9102F without CDC descriptors, mass storage) is reported as unsupported and left closed.
- **Opening without resetting.** On open the proxy sets the baud rate, 8N1 and flow control, and sets DTR and RTS
  **together in one request** (the CP210x `SET_MHS` with both mask bits), never one at a time, so the auto-reset
  circuit never sees the EN/IO0 pulse pattern *(my call: both asserted)*. They are never toggled afterwards.
- **Baud rates.** The CP210x silently rounds a rate it can't do; the proxy reads the rate back after setting it and
  reports a mismatch in `dongle.error` (best effort: some chips round in hardware only).
- **Flow control is CP210x only** *(my call)*. None of the drivers can turn on hardware flow control: the CP210x
  driver only defines `CP210X_CMD_SET_FLOW` (0x13), CH34x has no documented request, CDC-ACM has no standard one. The
  proxy sends the CP210x vendor request itself. On CH34x and CDC-ACM dongles `flow_control: true` is not applied and
  the status says so. The SLZB-06 test runs with flow control off.
- **Settings are checked on their own, applied per dongle** *(my call)*. A PUT is validated without looking at the
  dongle. Applying happens on open and on every change; a failure (a rate the chip refuses or rounds, flow control
  unsupported, the open itself failing) is shown in `dongle.error` and the dongle stays closed, or open without flow
  control for the unsupported case.
- **Unplug** closes the device and the TCP client; the next supported plug-in is served again.
- **TCP side.** One listener on the stored port, default **6638** *(my call: the port SMLIGHT and zigpy users already
  know)*, IPv4 and IPv6, backlog 1. `TCP_NODELAY` on the client socket (Nagle plus delayed ACK would add ~200 ms
  stalls that hurt ASH and spinel timers). Data passes unchanged in both directions.
- **One client, the first one keeps it** *(my call, reversed from newest-wins)*. While a client is connected, a new
  connection is accepted and closed at once, and counted as refused. This stops a ZHA probe, a second stack or a port
  scan from kicking the running stack and re-initialising the radio. A stuck session is freed by keepalive or by
  `PUT {"kick": true}`.
- **Liveness** *(my call)*: explicit TCP keepalive on the client socket (idle 10 s, interval 2 s, 3 probes), so a
  vanished client is closed within about 16 s.
- **No silent corruption** *(my call)*:
  - Dongle → client: bytes go into a ring sized above the TCP send buffer (at least 8 KB). If the ring would
    overflow (the client can't keep up), the client is disconnected and the event counted; bytes are never skipped
    mid-stream.
  - Client → dongle: the socket is read only while a dongle is open; when it isn't, bytes read from the client are
    discarded and counted, so stale requests are never replayed into a freshly plugged radio.
  - A new session starts clean: both rings are flushed when a client connects and when a dongle opens.
- **No dongle connected:** the listener still accepts; the client is kept open and served once a dongle appears
  *(my call: lets the client and the dongle come up in any order)*. What it sent meanwhile is discarded (above).
- **Access**:
  - Its own service class, `NETCFG_SVC_USB_SERIAL`, not `NETCFG_SVC_DEVICE_API`, checked on each accepted connection
    by the side its local address is on *(my call)*.
  - **Both Ethernet VLANs reach it (user's decision):** in split mode the management VLAN and the Thread backbone
    VLAN are both allowed, since Home Assistant usually sits on the backbone/IoT side. In single mode the one
    Ethernet interface is allowed.
  - Connections arriving on the OpenThread netif (from the Thread mesh) are always refused, in single mode too
    *(my call)*. The policy therefore tells the backbone Ethernet interface apart from the OpenThread netif, which
    today share `NETCFG_SIDE_THREAD`.
  - Raw access to the radio means anyone who connects can rewrite the coordinator; off by default and the
    first-client rule are what limit it. The docs page says so.

### Settings API

- `GET /device/usb_serial` →

  ```json
  {
    "enabled": true,
    "port": 6638,
    "baud": 115200,
    "flow_control": false,
    "dongle": {"vid": "10c4", "pid": "ea60", "driver": "cp210x", "open": true, "flow_control": false, "error": null},
    "client": {"peer": "192.168.10.23", "since_s": 812},
    "bytes_to_dongle": 12345,
    "bytes_from_dongle": 67890,
    "discarded_from_client": 0,
    "overflow_disconnects": 0,
    "refused_connections": 0
  }
  ```

  `dongle` is `null` with no device attached, or has `"driver": null` for an unsupported one. `dongle.open` says the
  port is open and bridged, `dongle.flow_control` whether flow control is actually on, and `dongle.error` is `null`
  or a short text such as `"flow control not supported by ch34x"` or `"baud 921600 read back as 923076"`. `client` is
  `null` with no client. Counters are since boot.
- `PUT /device/usb_serial` with any subset of `enabled`, `port`, `baud`, `flow_control`, and the one-shot
  `kick: true` (closes the current client) → the same body as GET. Stored in NVS (namespace and key are the
  builder's call, following `ot_console`/`netcfg`; `kick` is not stored). Changes apply live: a new port restarts the
  listener (dropping the client); new serial settings are applied to the open dongle. Invalid values answer 400 in the
  device API's error format: a port outside 1–65535 or taken by the board's own services, a baud rate not in the
  fixed list 38400, 57600, 115200, 230400, 460800, 921600, 1000000, 2000000, a non-boolean flag. Nothing in the check
  depends on which dongle is attached.
- Default baud **115200** *(my call: SMLIGHT's and most Zigbee coordinators' default)*.
- Works in every border router mode (it does not touch OpenThread).

### Board side

These were "left to the builder" before the review; they decide whether the PoC works, so they are fixed here
*(my calls)*:

- **One owner task.** A single proxy task owns every state change: open, close, settings, enable/disable, client
  accept and close. The HTTP handlers and the USB callbacks only post messages to it (a queue) and, for GET, read a
  snapshot. Nothing else calls `cdc_acm_host_close` or touches the sockets' lifecycle, so close never races an unplug.
- **USB on core 1, above the web server.** `usb_host_install` is called from a core-1 task so its interrupt lands
  on core 1. The USB host library daemon and the CDC driver task are pinned to core 1 at a priority above the web
  server and the device API workers (above 5), and below the routing and lwIP tasks. Core 0 (OpenThread, lwIP) gets
  no new task.
- **The data callback only copies.** It copies the received bytes into the dongle → client ring and returns at
  once; it never calls lwIP or blocks. The owner task (or a sender task under it) drains the ring into the socket,
  coalescing into large `send()` calls (at most one per few milliseconds or per ring threshold) so the core-0 tcpip
  thread is not woken for every 64-byte USB packet.
- **Shutdown order** (on disable, and before a setting change that needs it): stop accepting; close the client;
  `cdc_acm_host_close` the device; `cdc_acm_host_uninstall`; `usb_host_device_free_all` (retried while it answers
  not-finished); wait for the all-free event; `usb_host_uninstall`; the daemon task ends.
- **lwIP budget.** `SOCKETS_NEEDED` gains a `USB_SERIAL_SOCKETS` term (listener, client, one connection being
  refused = 3); `CONFIG_LWIP_MAX_SOCKETS` and `CONFIG_LWIP_MAX_ACTIVE_TCP` in `p4/sdkconfig.defaults` rise by at
  least that, plus headroom for TIME_WAIT from refused connections. The build check's message already tells a builder
  with an old `p4/sdkconfig` what to do.
- **netcfg** gains a side lookup by socket (local address of an accepted connection) next to the HTTP one, and the
  `s_net` reads it shares are made safe against a trial apply or revert from another task.
- Pure parts are split out and host-tested, per `STYLEGUIDE.md`: driver choice from USB IDs and classes, settings
  validation, the first-client rule, the ring and its overflow decision.
- Left to the builder: component name (e.g. `usb_serial`), NVS keys, exact priorities and stack sizes within the
  rules above, ring sizes above the floor, the coalescing threshold, metrics in `/metrics` (optional).

### Client side (documented, not shipped)

- Zigbee: ZHA takes `socket://<board>:6638` as the serial path; Zigbee2MQTT takes `tcp://<board>:6638` with the
  matching adapter type (`zstack` for the SLZB-06's ZNP firmware).
- Linux, `otbr-agent`: `socat` turns the TCP stream into a pty, for example
  `socat -d pty,raw,echo=0,link=/tmp/ttyRCP tcp:<board>:6638`, then
  `otbr-agent ... 'spinel+hdlc+uart:///tmp/ttyRCP'`. The baud rate on the pty does not matter; the board sets the
  real one.
- Ziggurat: the same pty, or TCP directly if its version supports it.
- A short page in `docs/` describes these, marked experimental, with the warning that anyone who reaches the port
  controls the radio.

## 4. Testing

- Host tests for the pure parts (driver choice, settings validation, first-client rule, ring overflow).
- Pytest checks that need no dongle: off by default, validation, persistence, `kick`, a second connection refused,
  and 100 enable/disable cycles with no crash.
- On-board, following the hardware verification budget (one batched pass after review):
  1. **Hardware facts.** The Type-A port powers the SLZB-06 (USB mode, Zigbee firmware) and it enumerates as
     `cp210x`; the SLZB-06's uptime does not reset when the port opens; hot-plug it 10 times on PoE power and the
     board's reset reason stays clean.
  2. **Loopback integrity.** A spare CP2102/CH340 adapter with TX jumpered to RX: a host script sends random data
     through TCP and compares the echo, 5 minutes at each of 115200, 460800 and 921600, while the web server is
     loaded and a packet capture streams. Zero mismatches; any overflow disconnect is reported, never corruption.
  3. **Zigbee.** ZHA or Zigbee2MQTT through the proxy forms a Zigbee network and a device joins. *Before this step,
     the SLZB-06's current Zigbee network is backed up (zigpy-znp backup) or confirmed disposable: forming
     overwrites it.*
  4. **Robustness.** Unplug and replug the dongle mid-session; kill and restart the client; connect a second client
     (refused, the first keeps working); unplug while turning the proxy off.
  5. **No harm to the C6.** Zigbee and Thread on channels at least 5 apart; with the SLZB-06 on a USB extension
     cable, 30 minutes of sustained Zigbee traffic while the C6 border router is measured against a 30-minute
     baseline without the dongle: its role, `/device/diag` OpenThread lock timings, C6 TX retries and CCA failures,
     and ping loss to a Thread device. (Ping loss under REST load is already open from v0.4.0, so the baseline runs
     under the same load.)
  6. Optional: the SLZB-06 switched to its Thread RCP firmware, `otbr-agent` through `socat` reads the RCP version
     and forms a network (its baud rate found first with the dongle plugged straight into a Linux host).

## 5. Production direction: connect mode

**User's decision:** the PoC listens (section 3); production should **connect**: the board dials out to one
configured `host:port`, and the far side listens (for example
`socat pty,raw,echo=0,link=/dev/ttyRCP tcp-listen:6638,reuseaddr` in front of `otbr-agent` or a Zigbee stack).

Why it is the production shape:

- No listening radio port on the board: nothing on the LAN can connect in, so the first-client rule, `kick`, the
  per-VLAN access rule and the Thread-mesh refusal are no longer needed.
- The board talks to exactly one configured peer, and inbound firewall rules between VLANs are not needed.

What it adds later: a reconnect loop with back-off, hostname resolution (DNS, possibly `.local` over mDNS), the
source interface in split mode, and a `mode` setting (`"listen"` / `"connect"`) plus a `host` field in
`/device/usb_serial`.

What the PoC must keep possible *(my calls)*:

- The bridge, rings, owner task and shutdown order never depend on how the socket came to be: accepting it and
  connecting it are one small, replaceable piece of the owner task.
- The settings API leaves room for `mode` and `host` without renaming existing fields; a PoC build treats a missing
  `mode` as `"listen"`.
