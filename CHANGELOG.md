# Changelog

All notable changes to this project are listed here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
release tags follow [Semantic Versioning](https://semver.org/). Firmware images carry their own version stamps
(`<build timestamp>-<git hash>` for the P4 app and `c6_rcp.bin`, `base-<hash>` for the C6 base image), listed in each
release's `versions.txt`.

## [Unreleased]

### Added

- 3D-printable case for the board (`case/`, `make case`): a one-piece base with keyholes for two wall screws and a
  honeycomb lid on snap-in corner tabs, checked against the vendor 3D model for fit and printability. See
  `docs/case.md`.
- Update from GitHub: the board checks the latest release (`versions.txt`) and installs it, the bundle first, then the
  radio firmware when it differs, then one reboot. New `GET`/`POST /device/update` (`check=true`, `force=1`; see
  `docs/api.md`) and an "Updates" card on the Firmware page that checks when the page opens, never in the background.
  A dev build newer than the release can go back to it with "Install anyway". `/device/ota/url` and
  `/device/radio/firmware?from_url=` now accept `https://` (verified against the built-in certificate bundle) and follow
  redirects. Bundles are not signed; trust is TLS to GitHub. Test builds add the console command `update_source`. See
  `docs/updates.md`. Not yet checked on a board.

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

[Unreleased]: https://github.com/t3hk0d3/esp32-p4-otbr/compare/v0.4.0...HEAD
[0.4.0]: https://github.com/t3hk0d3/esp32-p4-otbr/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/t3hk0d3/esp32-p4-otbr/compare/v0.2.1...v0.3.0
[0.2.1]: https://github.com/t3hk0d3/esp32-p4-otbr/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/t3hk0d3/esp32-p4-otbr/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/t3hk0d3/esp32-p4-otbr/releases/tag/v0.1.0
