# Changelog

All notable changes to this project are listed here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
release tags follow [Semantic Versioning](https://semver.org/). Firmware images carry their own version stamps
(`<build timestamp>-<git hash>` for the P4 app and `c6_rcp.bin`, `base-<hash>` for the C6 base image), listed in each
release's `versions.txt`.

## [Unreleased]

### Fixed

- Browser flasher on ESP32-P4 chips of revision v3.1 and later: the install stopped with "The install didn't finish"
  and wrote nothing. esptool-js resets the board when it switches to the faster baud rate and then skips the chip's
  flash power-on fix for these revisions; the flasher now runs it again
  ([esptool-js#275](https://github.com/espressif/esptool-js/issues/275)).

### Changed

- Tested on hardware: the firmware for P4 chips of revision v3.1 and later (a v3.2 chip, flashed from the browser),
  and the no-wires first install of the radio firmware (way A) on a C6 untouched since the factory.

## [0.6.0] - 2026-11-11

### Added

- Firmware targets, a board (`board_id`) and a chip (`chip_id`) each: chips of revision v3.1 and later are supported
  (`ESP32-P4-rev3.0`), next to those below v3 (`ESP32-P4-rev1.3`, the default). Each release carries
  `<board_id>_<chip_id>.ota` and `full_<board_id>_<chip_id>.zip` per target. `otbr.bundle` and `p4-flash.zip` stay for
  boards on older firmware, which update from them. A board on this firmware or later updates from the file of its own
  target. Build one with `make build BOARD_ID=<board_id> CHIP_ID=<chip_id>`. Chips of v3.0 get no firmware. Not yet
  tried on a v3 chip.
- USB/IP server: the device on the USB-A port is exported over the network (TCP 3240, bus id `1-1`, one client) and
  attaches on Linux (`usbip attach -r <board> -b 1-1`) or Windows (usbip-win2). Off by default; turning it on or off
  reboots the board and is refused while a USB channel runs `otbr` or `ziggurat` (409 `usb_channel_in_use`). While on,
  the USB channels are `unavailable` and their mode changes get 409 `usbip_enabled`. No isochronous transfers, no hubs,
  one configuration, no authentication or encryption. New `GET`/`PUT /device/usbip`, a USB/IP page and the `usbip`
  flag in `GET /device/channels`. Transfers of up to 120 KB, with USB transfer buffers in PSRAM
  (`CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM`, which also moves the USB radio channels' buffers), so USB flash
  drives work. See `docs/usbip.md`. Checked on a board with a USB serial radio dongle and a USB flash drive (read).
- Logging: one log stream (ESP-IDF components, OpenThread and Ziggurat) out on four channels with their own levels:
  UART (as before), USB-JTAG (header pins 27/28, needs a USB breakout), syslog over UDP (RFC 5424, `host[:port]`, a
  hostname is looked up again) and a live log in the browser (at most 2 viewers, each with its own level). Per-component
  levels combine with the channel level (a line must pass both). New `GET`/`PUT /device/logging`,
  `GET /device/logging/ws`, the **Device → Logs** page, the console command `log`, and the metrics
  `br_log_lines_sent_total`, `br_log_lines_dropped_total` and `br_log_viewers`. Debug and verbose levels exist in
  test builds only (`make build-test`, its own `p4/sdkconfig.test`); a release build stays at Info and reports it as
  `max_level`. `*` (everything else) goes up to Debug; a named component can go to Verbose. Levels default to what
  they were. The secondary console on USB-JTAG is off. The USB-JTAG channel is
  not checked on hardware (no breakout). See `docs/logging.md`. On-board tests pass (2026-11-10).
- Time sync: the board sets its clock from NTP, from the server the DHCP lease names, else `pool.ntp.org`, or a custom
  server. A Time section on the Ethernet page shows the board time and sync state; `GET`/`PUT /device/time`. Zigbee
  status times are real dates, also for events before the first sync.
- Thread and Zigbee together: one radio can run the Thread border router while the other runs the Zigbee coordinator,
  in either order. `PUT /device/channels/{id}` no longer answers 409 `hybrid_not_supported`. Starting or stopping Zigbee
  still reboots the board, and Thread with it; the Radios page says so before saving. Moving Thread onto the radio that
  runs Zigbee is a reboot that stores both radios. The Dashboard warns when both networks use the same 802.15.4 channel.
  Before downgrading to an older firmware, turn Zigbee (or Thread) off: older firmware never runs both. See
  `docs/zigbee.md`.

### Changed

- The browser flasher is a page of its own on esptool-js instead of ESP Web Tools: **Detect board** reads the chip
  revision and picks the firmware built for it, **Install** writes it with an MD5 check of each image, and a live board
  follows each step. **Choose firmware by hand** is the fallback when the chip can't be read. A chip no published
  firmware covers is told so and left unchanged. The site publishes `firmware/targets.json` and the images of each
  target.
- OpenThread's lines on the UART now have the format of every other tag: `I (123) OPENTHREAD: ...` instead of
  `I(123) OPENTHREAD:...`. Scripts that match the old form need the new one.
- lwIP UDP PCBs 16 → 32 (the sum of their users is in `p4/sdkconfig.defaults`), lwIP sockets 24 → 27 (syslog, USB/IP). An
  existing `p4/sdkconfig` keeps the old values and the build stops with an `#error`: delete `p4/sdkconfig` after
  pulling and build again (`make build` does it by itself).

## [0.5.0] - 2026-11-10

### Added

- Update from GitHub: the board checks the latest release (`versions.txt`) and installs it, the bundle first, then the
  radio firmware when it differs, then one reboot. New `GET`/`POST /device/update` (`check=true`, `force=1`; see
  `docs/api.md`) and an "Updates" card on the Firmware page that checks when the page opens, never in the background.
  A dev build newer than the release can go back to it with "Install anyway". `/device/ota/url` and
  `/device/radio/firmware?from_url=` now accept `https://` (verified against the built-in certificate bundle) and follow
  redirects. Bundles are not signed; trust is TLS to GitHub. Test builds add the console command `update_source`. See
  `docs/updates.md`. Not yet checked on a board.
- Chip temperature: `chip_temperature_c` in `GET /device/diag`, the gauge `br_chip_temperature_celsius` in `/metrics`
  and a "Chip temperature" row in the "Load and memory" card of the Status page. It is the die temperature, not the
  room temperature: it reads above ambient and rises with load. The sensor runs in its most accurate range, −10 to 80 °C (about ±1 °C; above 80 °C still shown, less accurately), read without a lock, and the value moves in 1 °C steps on a given board even though it is shown with one decimal. When it
  cannot start or a read fails, the value is `null` (`-` on the page, no series in `/metrics`). See `docs/diagnostics.md`.
- Radio channels: every radio is a channel with a stored mode. The ESP32-C6 (built-in) and one USB dongle (cp210x, ch34x or
  plain CDC-ACM driver) are listed; a channel runs `off`, `otbr` (Thread border router) or `rcp-proxy`; `ziggurat` is
  Zigbee mode (below). A dongle's firmware is detected at every plug-in (spinel, Ziggurat, Z-Stack ZNP, EZSP,
  or unknown, within 3 s). `GET /device/channels`, `PUT` and `DELETE /device/channels/{id}`; the Radios page. See
  `docs/radios.md`. Checked on a board with the C6 and a ZigStar OllyZig.
- Thread on a USB dongle: OpenThread runs on the C6 or on a spinel RCP dongle (design target: ZigStar OllyZig,
  CC2652P), and moves between them at runtime without a reboot and with the stored network (`PUT` `otbr` on the other
  channel answers 202; a start and a stop of Thread run the same way). Unplugging the OTBR dongle leaves the board up
  with the new mode `NO_RADIO` (the Thread REST API and the console answer 503 `no radio in OTBR mode`); Thread starts
  again on its return. `/device/status` gains `otbr_channel`.
- RCP proxy: a channel's bytes (the dongle's, or the C6's spinel frames as HDLC) served over TCP or UDP, listening or
  connecting out (back-off 1 to 30 s, `last_error` says why), with `hdlc` or `raw` framing on UDP. Two channels on one
  listen port are refused (409 `port_taken`). Replaces the experimental USB serial proxy.
- Zigbee mode (experimental): a radio set to `ziggurat` runs the Ziggurat Zigbee stack and serves Home Assistant's ZHA
  (radio type Ziggurat, `ws://<board>/zigbee`) to one client; a **Zigbee** section on the Dashboard, `GET /device/zigbee`,
  `zigbee_channel` in `/device/status` and `zigbee_*` metrics. Radio resets are survived and a lost radio shows as
  `no_radio` without a reboot. On-board tests (`p4/pytest/test_zigbee.py`) pass on the C6; on a USB dongle the
  pairing test still times out (pairing on the OllyZig CC2652P is slow, on a PC too). See `docs/zigbee.md`.

### Changed

- The experimental `/device/usb_serial` API (never released) is removed in favour of the channel API.
- The Dashboard has three sections: Device and network (board status with board-wide faults, Reboot, the two radio
  slots, Ethernet, firmware, load), Thread and Zigbee. A stack that is not running shows one line pointing at the
  Radios page. The "Thread runs on" chip and the Mode row are replaced by the Radios card and Thread's own status
  line.
- Navigation: Dashboard, Thread (Network, Topology, Routing, Commissioner, Addresses, Tools, Console), Device (Radios,
  Ethernet, Firmware, Diagnostics, Packet capture), About. The Thread menu is hidden when no channel is in `otbr`.
- The C6's channel id is `builtin-c6` (kind `builtin`; it was `c6`). Channels report `channel_state` (`absent`,
  `reinit`, `ready`, `fault`) and `mode_state` beside `state`, and `GET /device/channels` lists the modes and where
  they run (`modes`).
- A push of `c6_rcp.bin` (`POST /device/radio/firmware`) answers once the mode on the C6 (Thread, Zigbee or the RCP
  proxy) runs again on the new firmware: after the push it waits up to 180 s, and when the mode does not start, 502
  `restart_failed` says which mode and why. A push of `c6_installer.bin` still leaves the C6 without RCP firmware. The
  push and the ROM recovery moved out of the Thread stack into the built-in radio's own code.
- Moving Thread to another radio no longer rolls back or reboots: a failed start stays on the new radio with its
  reason, and the Radios page says where Thread is.
- OpenThread accepts any RCP with the ot-br-posix capability set (ack timeout, tx retries, CSMA backoff, tx security,
  tx timing), not the fixed set the C6 reports.

### Fixed

- Routing: "OMR prefix in use" (`status.omr_prefix` in `GET /device/routing`) is the OMR prefix OpenThread favors on the
  network, which can be another border router's; it was always the board's own. The board's own prefix moves to the new
  `status.own_omr_prefix`, and the Routing page shows it as "This router's prefix" when it is not the one in use. While
  the border router is not running, the prefix in use reads unknown (`null`).

## [0.4.0] - 2026-11-05

### Added

- Device info in the topology scan and on the Topology page: vendor name, model and software version, Thread stack
  version, EUI-64, app URL and OUI of every router and child (`info`, `progress.info` in `GET /device/topology`; see
  `docs/api.md`). The scan now waits for sleepy devices, up to 300 s from its start, and the page says so while it
  waits. This board now sends vendor name `ESP32-P4-WIFI6-POE-ETH`, model `OpenThread Border Router`, its software
  version (`YYYYMMDD-<hash>`) and app URL in network diagnostics, instead of `RD:`; OpenThread's reference-device
  (certification test harness) setting is turned off for that. Checked on ha-thread-bbc1 (2026-11-04), including a
  sleepy child polling every 120 s; the later 60 s re-send period and the stray-answer checks are host-tested only.
- The Topology page names devices registered with any border router's SRP server, not only this board's, and shows
  ESPHome, Matter and HomeKit details and border router records. The scan asks the other SRP servers over DNS-SD
  (`srp_servers`, `srp_hosts` in `GET /device/topology`; see `docs/api.md`). Checked on ha-thread-bbc1 (2026-11-04):
  records from the Home Assistant OTBR named an ESPHome device and two Matter devices; scans took about 6 s. HomeKit
  records were not seen on that network.
- Thread console: the board's OpenThread CLI over a WebSocket (`/device/console/ws`), on a new **Console** page under
  Management (desktop and phone) and from a shell with `websocat`. Off by default (`GET`/`PUT /device/console`); one
  session at a time, the board stays a border router, risky commands ask first on the page. OpenThread's CLI now
  runs in release images too (it was set up only in test builds). See `docs/api.md`, "Thread console".
- TREL visibility: the Ethernet page's TREL card lists border routers of other Thread networks apart (extended PAN ID,
  "other" chip, `peers_other`, `br_trel_peers_other`; never used) and shows each neighbour's 15.4 and TREL preference
  as a percentage, red when TREL is losing to the radio. The Topology map draws this board's TREL links as dashed
  curves through the home network box, and links of this board carry `transport` in `GET /device/topology`. See
  `docs/networking.md`.
- Documentation site on GitHub Pages (<https://t3hk0d3.github.io/esp32-p4-otbr/>), built from `docs/` and `README.md`
  with Material for MkDocs: a landing page, the docs, and a browser flasher page (ESP Web Tools) that writes the P4
  firmware of the latest release over USB from Chrome or Edge. `make docs`, `make docs-serve`, `make docs-firmware`; see
  `docs/development.md`.

### Changed

- The web UI is stored gzipped in the web image (`tools/pack_web.py`, run by the P4 build) and sent with
  `Content-Encoding: gzip`: the SPIFFS pages it fills drop from about 423 KiB to 131 KiB. Pages and scripts are
  unchanged at the source.
- CI skips the host tests and the build on pull requests that change only documentation.

## [0.3.0] - 2026-11-03

### Added

- TREL (Thread over Ethernet): Thread mesh links over the backbone interface to other border routers of the same Thread
  network, next to the 15.4 radio. On by default; switched live (no trial, no reboot) on the Ethernet page or with
  `"trel"` in `PUT /device/network`. State, peers and counters in `/device/status`, on the Ethernet page and as
  `br_trel_*` metrics. In split mode the TREL port is dropped on `mgmt`. The app image grows by about 19 KB. See
  `docs/networking.md`.
- Choosing the leader: a **Leader** card on the Network page and `GET`/`PUT /device/thread` set a stored leader weight
  adjustment (-16 to +16 on top of the base weight 65) and a **Become leader** button that makes this board take over
  from a leader with a strictly lower weight (never automatic; the role is not stored). See `docs/api.md`, "Choosing the
  leader". Tested on the board on a live network, taking over from another border router.
- First install without wires: on a factory C6 (ESP-Hosted firmware, no jumper) the board reports `RADIO_FAULT`
  `esp-hosted firmware`, and uploading the new release file `c6_installer.bin` sends it through ESP-Hosted's own update
  over the P4-C6 SDIO lines; it installs the C6 bootloader, partition table and updater, after which `c6_rcp.bin` is
  uploaded as usual. Nothing is written to the C6 without that upload. Tested on the board with an ESP-Hosted 0.0.27
  stand-in, not yet on an untouched factory C6. See `docs/first-install.md`, Path 1.
- `docs/home-assistant.md`: adding the board to Home Assistant, moving it onto the preferred Thread network, phones and
  Matter, several boards, troubleshooting.
- Network names: **mDNS** and **Hostname** sections on the Ethernet page and `GET`/`PUT /device/names` set the mDNS
  hostname, the name Home Assistant shows (the `_meshcop._udp` instance, by default the hostname), an optional
  `_http._tcp` advertisement of the web UI (off by default, not in split mode) and the DHCP client name. Applied live, stored
  across reboots and updates. The page shows when an mDNS conflict renamed the board. See `docs/networking.md`.
- Routing to the mesh for networks without DHCPv6 prefix delegation: a custom OMR prefix (one /64 you choose, optionally
  with the self-generated ULA kept beside it) and a route method, `pd` (default, as before), `ripng`, `bgp` or `off`.
  RIPng (RFC 2080) and an announce-only BGP speaker (up to 2 peers, iBGP or eBGP, IPv6 transport) announce the custom
  prefix and the ULA to the core router on the backbone; routes they receive are counted and dropped. Settings apply
  live on the new Routing page (Device menu) or with `GET/PUT /device/routing`; `/device/status` gains a `routing`
  object, `/metrics` the `br_routing_*`, `br_bgp_*` and `br_ripng_*` series, the console `routing show`. See
  `docs/networking.md`.

### Changed

- The board's DHCP client name is `esp-otbr` instead of lwIP's `espressif`.
- `POST /device/radio/firmware` refuses `c6_installer.bin` on a C6 that already runs the updater (400 `invalid_image`),
  before anything is erased.
- `docs/first-install.md`: the jumper path is now Path 2 and the USB-UART path Path 3.
- The "settings on trial" banner (Dashboard and device pages) shows the time left as a countdown tile, with the
  explanation next to it, **Keep new settings** on the right and a bar that drains over the 5 minutes. On a phone the
  button goes below at full width.
- DHCPv6-PD is on only with the route method `pd` (the default, so nothing changes without routing settings); with any
  other method the client is off and `pd.state` reads `disabled`.
- lwIP sockets 10 → 18 (web server 10, mesh routing up to 7, one HTTP client). An existing `p4/sdkconfig` keeps the old
  value and the build stops with an `#error` in `routing/task.c`: delete `p4/sdkconfig` after pulling and build again.

## [0.2.1] - 2026-11-02

### Fixed

- Web UI "Form Network" froze the board until it was power-cycled: the web server held the OpenThread lock twice,
  and bringing Thread up deadlocked with the lwIP thread (no Ethernet, no web UI, no Thread, no watchdog reset). Adding
  or removing an on-mesh prefix or a Thread address, and starting the commissioner, held the lock the same way and are
  fixed too.

## [0.2.0] - 2026-11-02

### Added

- `GET /metrics`: device health (CPU time per core and task, stacks, heap, OpenThread lock) and Thread figures (role,
  mesh size, MAC and IPv6 counters, border routing, SRP) in the Prometheus text format. Thread figures are collected
  every 10 s with a 10 ms lock timeout, so a scrape never waits for OpenThread. See `docs/metrics.md`.
- `Makefile` as the single entry point for building, flashing, network updates, packaging and tests (`make help`); CI
  uses the same targets.
- Web UI, Network page: "Join with Dataset" joins an existing Thread network from its active dataset TLVs (hex), with
  a confirm that names the network being replaced.

### Changed

- The device page is split up: its status becomes the Dashboard (`/`, merged with the upstream Dashboard's Thread network,
  identifiers and Thread addresses, plus the RLOC and RLOC16; the PSKc is hidden until "Show"), and a "Device" menu holds Ethernet, Firmware, Packet
  capture and Diagnostics. `/device.html` is gone. The Ethernet page replaces the network form with setup cards (single port or split), one card
  per interface with its live state and an inline editor (addresses as `192.168.1.10/24`), live validation, and a
  review of the changes before Apply and reboot; tagged VLANs need a tick that the switch port carries them. "Confirm" is
  now "Keep new settings" and shows on every device page while settings are on trial.
- The README is a short overview; the documentation moved to `docs/` (building, updates, API, networking, sniffer,
  maintenance jumper, diagnostics, development, troubleshooting).

### Fixed

- `PUT /node/dataset/active` answered 409 while Thread ran, which broke Home Assistant's Thread panel (moving the board
  onto the preferred network). For a complete dataset it now stops Thread, stores it and starts Thread on it; writing
  the stored dataset again changes nothing, and a partial change still answers 409 (use the pending dataset). Writing a
  different active dataset deletes the pending dataset of the old network, which otherwise took the board back to the
  old network when its delay expired.
- `c6-updater-flash.zip` put the C6 updater into the first RCP slot (`ota_0`, 0xa0000) and wrote `otadata`; it now
  writes bootloader, partition table and the updater into the `test` partition (0x20000), as the ROM recovery does. The
  v0.1.0 release asset was replaced.

## [0.1.0] - 2026-11-01

First release: an OpenThread Border Router on the Waveshare ESP32-P4-WIFI6-POE-ETH, with the ESP32-P4 as host and the
on-board ESP32-C6 as the radio co-processor over SPI.

### Added

- Border router on Ethernet: IPv4 and IPv6, DHCPv4, DHCPv6 prefix delegation, NAT64, mDNS/SRP advertising.
- Home Assistant compatible Thread REST API (`/node`, `/api`, `/.well-known/thread/br-rest`) and the upstream Web GUI,
  from esp-thread-br commit `1f5af46` with the patches listed in `p4/components/PATCHES.md`.
- Device page (`/device.html`) and `/device/*` API: status, packet capture, updates, radio firmware, network settings
  and diagnostics.
- Sniffer mode: an IEEE 802.15.4 capture streamed as pcap, straight into Wireshark.
- OTA update of the P4 app and the web UI as one bundle (`otbr.bundle`), by upload or from a URL, with rollback of an
  app that does not come up.
- C6 radio firmware update, independent of P4 updates (`POST /device/radio/firmware`, upload or `?from_url=`).
- Optional maintenance jumper (three wires to the C6 header H7): automatic and manual recovery of a C6 with no or broken
  firmware, and capture of the C6 console (`/device/radio/log`).
- CPU, memory, task and OpenThread-lock diagnostics (`/device/diag`).
- VLAN split mode (separate management and Thread backbone interfaces) with confirm-or-revert; mDNS answered only on the
  Thread backbone VLAN.
- CI: host tests and a release build on every push and pull request; a `v*` tag publishes a GitHub release with the OTA
  bundle, the radio firmware, USB flash sets, third-party licenses and checksums.
- License: GNU AGPL v3 or later, with an additional permission to link Espressif's binary-only libraries (`NOTICE`).

### Known limitations

- No authentication or TLS: put the board on a trusted network.
- Radio firmware updates interrupt the border router for a few seconds.
- Not verified on hardware: the no-jumper first install, recovery of a blank C6, overwriting the factory ESP-Hosted
  firmware, the give-up path after repeated radio failures and the fall-back to the default network config.

See `docs/troubleshooting.md` for the full list.

[Unreleased]: https://github.com/t3hk0d3/esp32-p4-otbr/compare/v0.6.0...HEAD
[0.6.0]: https://github.com/t3hk0d3/esp32-p4-otbr/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/t3hk0d3/esp32-p4-otbr/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/t3hk0d3/esp32-p4-otbr/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/t3hk0d3/esp32-p4-otbr/compare/v0.2.1...v0.3.0
[0.2.1]: https://github.com/t3hk0d3/esp32-p4-otbr/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/t3hk0d3/esp32-p4-otbr/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/t3hk0d3/esp32-p4-otbr/releases/tag/v0.1.0
