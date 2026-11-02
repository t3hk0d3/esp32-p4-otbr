# Changelog

All notable changes to this project are listed here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
release tags follow [Semantic Versioning](https://semver.org/). Firmware images carry their own version stamps
(`<build timestamp>-<git hash>` for the P4 app and `c6_rcp.bin`, `base-<hash>` for the C6 base image), listed in each
release's `versions.txt`.

## [Unreleased]

### Added

- `GET /metrics`: device health (CPU time per core and task, stacks, heap, OpenThread lock) and Thread figures (role,
  mesh size, MAC and IPv6 counters, border routing, SRP) in the Prometheus text format. Thread figures are collected
  every 10 s with a 10 ms lock timeout, so a scrape never waits for OpenThread. See `docs/metrics.md`.
- `Makefile` as the single entry point for building, flashing, network updates, packaging and tests (`make help`); CI
  uses the same targets.

### Changed

- The device page is split up: its status becomes the Dashboard (`/`, merged with the upstream Dashboard's Thread network,
  identifiers and Thread addresses; the PSKc is hidden until "Show"), and a "Device" menu holds Ethernet, Firmware, Packet
  capture and Diagnostics. `/device.html` is gone. The Ethernet page replaces the network form with setup cards (single port or split), one card
  per interface with its live state and an inline editor (addresses as `192.168.1.10/24`), live validation, and a
  review of the changes before Apply and reboot; tagged VLANs need a tick that the switch port carries them. "Confirm" is
  now "Keep new settings" and shows on every device page while settings are on trial.
- The README is a short overview; the documentation moved to `docs/` (building, updates, API, networking, sniffer,
  maintenance jumper, diagnostics, development, troubleshooting).

### Fixed

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

[Unreleased]: https://github.com/t3hk0d3/esp32-p4-otbr/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/t3hk0d3/esp32-p4-otbr/releases/tag/v0.1.0
