# ESP32-P4 OpenThread Border Router

Firmware for a standalone OpenThread Border Router (OTBR) on the Waveshare ESP32-P4-WIFI6-POE-ETH:
the ESP32-P4 is the host, the on-board ESP32-C6 is the radio co-processor (RCP) behind an SPI link,
and the board connects to your network over Ethernet.

Features:

- Border router on Ethernet (IPv4 and IPv6, DHCPv4, DHCPv6 prefix delegation, NAT64, mDNS/SRP advertising).
- Home Assistant compatible Thread REST API (`/node`, `/api`, `/.well-known/thread/br-rest`) and the
  upstream Web GUI, both from the pinned [esp-thread-br](https://github.com/espressif/esp-thread-br)
  commit (see `p4/components/PATCHES.md`).
- Sniffer mode: stream an IEEE 802.15.4 capture as pcap straight into Wireshark.
- Paired OTA of the P4 app and its web UI as one bundle, with rollback.
- Separate update of the C6 radio firmware, independent of P4 updates.
- Optional maintenance jumper (three wires to the C6 header H7): recovers a C6 with no or broken
  firmware and captures the C6 console.
- CPU, memory, task and OpenThread-lock diagnostics (`/device/diag`, device page).
- VLAN split mode (separate management and Thread backbone interfaces) with confirm-or-revert.

Design: `docs/superpowers/specs/2026-10-30-esp32-p4-otbr-design.md`. First install of a new board:
[`docs/first-install.md`](docs/first-install.md).

## Hardware

- Waveshare ESP32-P4-WIFI6-POE-ETH (ESP32-P4 chip revision v1.3, 32 MB flash, 32 MB PSRAM, IP101 Ethernet PHY,
  ESP32-C6 module with 8 MiB flash).
- A 2.4 GHz antenna on the C6 IPEX connector. Without it the radio does not work.
- USB-C to the P4 port (flashing and console, appears as a CH343 serial port, `/dev/ttyACM0` on Linux).
- Ethernet cable (PoE or a separate USB supply).
- Optional: the maintenance jumper (see `docs/first-install.md`), a 3.3 V USB-UART adapter for the no-jumper
  first install.

## Repository layout

- `p4/`: the ESP32-P4 firmware (components `br_core`, `device_api`, `netcfg`, `sniffer`, `diag`,
  `rcp_sync`, vendored `esp_ot_br_server` and `esp_br_http_ota`); `p4/pytest/` holds the target tests.
- `c6_updater/`: the small, frozen C6 program that receives the RCP firmware from the P4 and writes it to flash.
- `c6_rcp/`: the C6 radio firmware (`c6_rcp.bin`), an OpenThread RCP with SPI spinel.
- `common/`: code shared by the projects (`c6_board` pins and C6 flash layout, `rcp_sync_proto`, `board_cores`).
- `tools/`: `build_all.sh`, `run_host_tests.sh`, `run_vendored_rest_tests.sh`, `make_bundle.py`,
  `version.cmake`, `otbr_compat.py` (python-otbr-api driver), `tools/tests/` (tool tests).
- `docs/`: design spec, plan, first-install guide, Phase 0 results (hardware facts), hardware schematic.
- `IDF_VERSION`, `STYLEGUIDE.md`: pinned ESP-IDF version, formatting, naming and test conventions.

## Prerequisites

- ESP-IDF at the version in `IDF_VERSION` (v6.1), activated in your shell: `. ~/esp/esp-idf-v6.1/export.sh`.
- `uv` for the tools that are not installed globally: `uvx pre-commit run --all-files` (lint and format).
- `node` (optional): runs the device page tests in `tools/tests`.
- For the target tests: a Python virtualenv with `p4/pytest/requirements.txt` (`uv venv .venv` and
  `uv pip install -r p4/pytest/requirements.txt`).

## Build

```
. ~/esp/esp-idf-v6.1/export.sh
tools/build_all.sh           # c6_updater, c6_rcp, p4, in that order; stops at the first failure
tools/build_all.sh --test    # the same with the test-only console commands (OTBR_TEST_BUILD=1)
```

The P4 build embeds the C6 updater, bootloader and partition table (used by the maintenance jumper
recovery). Outputs:

| File | What |
| --- | --- |
| `p4/build/p4.bin` | P4 application image |
| `p4/build/web_0.bin` | web UI image (the vendored `frontend/` directory) |
| `c6_rcp/build/c6_rcp.bin` | C6 radio firmware, a separate release artifact |
| `c6_updater/build/` | C6 bootloader, partition table and updater (only needed for the no-jumper first install) |

Version strings of the P4 app and of `c6_rcp.bin` are `<UTC build timestamp>-<git hash>[-dirty]`. They are taken at CMake configure time, and a new
commit, checkout or reset triggers a reconfigure by itself, so the next build carries a fresh stamp. Only `-dirty`
is as of the last configure: uncommitted edits made after it do not change the stamp (run `idf.py -C p4 reconfigure`
to refresh it). Releases consist of two independent artifacts with independent
versions: the P4 OTA bundle `otbr.bundle` (web UI and P4 app) and `c6_rcp.bin`. The frozen C6 updater (the base
image the P4 embeds) is the exception: its version is content-addressed, `base-<8 hex>` from a hash of the sources that
define it (`tools/base_version.cmake`), so it does not change with the build time or with commits that leave those
sources alone (an IDF patch-level upgrade or a local menuconfig edit of the generated `sdkconfig` does not change it, deliberately). Do not use `--test` builds for
anything but testing.

## Flash

```
idf.py -C p4 -p /dev/ttyACM0 flash
idf.py -C p4 -p /dev/ttyACM0 monitor
```

This writes bootloader, partition table, application and the web UI image. A board flashed with an older partition layout
(historical: early builds had `rcp_0`/`rcp_1` partitions) needs
`idf.py -C p4 -p /dev/ttyACM0 erase-flash` and one full flash over USB; an OTA cannot move the partition
table. Erasing the flash also deletes the stored Thread dataset and network configuration. The C6 is not touched by
a P4 flash: see `docs/first-install.md` for a new board.

## Updates

### P4 and web UI (OTA)

```
tools/make_bundle.py --web p4/build/web_0.bin --app p4/build/p4.bin --out otbr.bundle
curl --data-binary @otbr.bundle http://<board>/device/ota
```

or use "Firmware update" on the device page (`http://<board>/device.html`). The bundle holds the web image and the
app image; both are written to the inactive pair, the web image is read back and checked against its SHA-256, and only
then the boot partition is switched. The reply is 200 and the board reboots about a second later. A new app that does not
run for 60 s with the HTTP server listening is rolled back by the bootloader. `tools/make_bundle.py --help`
shows how to inspect a bundle.

To let the board fetch the bundle itself: `curl -X POST -d '{"url": "http://host/otbr.bundle"}' http://<board>/device/ota/url`.
Only `http://` URLs work (no certificate bundle is built in; `https://` gives 400 `https_not_supported`).

Updates are accepted in every mode (also in `RADIO_FAULT` and while sniffing). One update or radio push at a time.

### C6 radio firmware

The RCP firmware is not part of the P4 image and never stored on the P4. Update it independently:

```
curl --data-binary @c6_rcp.bin http://<board>/device/radio/firmware
curl -X POST "http://<board>/device/radio/firmware?from_url=http://host/c6_rcp.bin"
```

or "Radio firmware" on the device page. The reply is `{"installed": "<version>"}`.

What happens: the border router stops (Thread and OpenThread are shut down), the P4 resets the C6 into its updater,
streams the image to it frame by frame (nothing is buffered on the P4), the C6 checks it and switches its boot slot
only when the transfer is complete, then the C6 resets and OpenThread starts again. Expect a few seconds of outage;
the Thread network keeps running on the other routers meanwhile, and an end device whose parent is the border router
loses its parent for that time. The Thread dataset stays stored and is used again. A failed upload (cable pulled, C6 reset
mid-transfer) leaves the previous firmware in place and the border router restarts with it; upload again. With `from_url`
the P4 retries the whole transfer up to three times.

Versions and compatibility. `GET /device/status` shows `rcp.installed` (version string reported by the C6 updater: a
non-empty string, `""` when the C6 has no valid RCP firmware, `null` if the probe failed) and `rcp.compatible`. The
version is only informative: nothing is compared or ordered. Compatibility is decided by OpenThread itself when it
starts: it rejects an RCP that does not match the OpenThread build of the P4 (spinel/RCP API version), and the board goes
to `RADIO_FAULT` with the reason `rcp incompatible` (`rcp.compatible` false). The cure is the same as for a missing
firmware: upload a matching `c6_rcp.bin` (from the same release as the P4 bundle, or one the release notes say is
compatible). A C6 without any RCP firmware gives `RADIO_FAULT` with `no rcp firmware`.

A P4 update runs the automatic ROM recovery only when the content-addressed base image version changed (see Maintenance
jumper); the RCP firmware survives it.

## Sniffer

```
curl -N "http://<board>/device/sniffer.pcap?channel=15" | wireshark -k -i -
```

`channel` is 11..26 and defaults to the channel of the active dataset (400 `no_channel` if there is no dataset).
`duration` (seconds) defaults to 600, maximum 3600. Only in `BR_RUNNING` (else 409 `busy`). The capture ends on
the duration, when the client disconnects, or on `curl -X DELETE http://<board>/device/sniffer`, and the border router
resumes by itself. The device page can start, stop and download a capture too.

- While sniffing, the Thread stack is stopped: the Thread REST API answers 503 for reads and 409 for changes, and the
  Thread network sees the border router leave. Ethernet (IPv4/IPv6, the web UI and `/device/*`) keeps working.
- The format is pcap with link type 283 (IEEE 802.15.4 TAP). The radio delivers frames without a valid FCS
  (RSSI and LQI replace the last two bytes), so the capture carries no FCS: the TAP header says "no FCS" and has
  RSSI, LQI and channel fields. Wireshark's FCS-related checks do not apply.
- Frames come through a 256 KB ring; when the client reads too slowly frames are dropped and counted
  (`sniffer.drops` in `/device/status`).
- A sniffer session is never persisted: a reboot returns to the border router.

## HTTP API

Everything is on port 80, no authentication. Errors on `/device/*` are JSON `{"error": "<code>", "detail": "<text>"}`.

| Method and path | Purpose | Notable errors |
| --- | --- | --- |
| `GET /device/status` | mode, app version and slot, `rcp`, `jumper`, `eth` addresses, `pd`, `sniffer` stats, `fault_reason`, `storage_ok` | |
| `GET /device/sniffer.pcap?channel=&duration=` | stream a pcap capture | 400 `invalid_channel`, `invalid_duration`, `no_channel`; 409 `busy` (not `BR_RUNNING`); 503 `busy` (no channel given and OpenThread's state could not be read within 100 ms: retry) |
| `DELETE /device/sniffer` | stop the running capture | 404 `not_found` |
| `POST /device/ota` | upload an `otbr.bundle`, then reboot | 400 `length_required`, `invalid_bundle`; 413 `payload_too_large`; 409 `busy` |
| `POST /device/ota/url` | body `{"url": "http://..."}`, same via download | 400 `invalid_request`, `https_not_supported`; 502 `transfer_failed` |
| `POST /device/radio/firmware[?from_url=]` | push `c6_rcp.bin` to the C6, restart the BR | 400 `length_required`, `invalid_image`, `https_not_supported`; 413 `payload_too_large`; 409 `busy` (sniffing, starting, update running); 502 `fetch_failed`, `push_failed` |
| `POST /device/radio/recover` | maintenance jumper: rewrite the C6 base image (no RCP firmware) and restart | 200 `{"recovered":true,"rcp":"<installed version>"\|"no rcp firmware"\|"unknown","mode":...}` (also when the C6 has no RCP firmware afterwards); 412 `no_jumper`; 409 `busy`; 502 `recovery_failed` (only when the recovery itself failed) |
| `GET /device/radio/log` | jumper: the newest 16 KB of the C6 console (`text/plain`) | 412 `no_jumper` |
| `GET /device/diag` | per-core load, per-task CPU/stack/affinity, memory, OpenThread lock stats, uptime, reset reason | |
| `GET /device/network` | active config, pending trial with remaining time, interface state | |
| `PUT /device/network` | store a trial network config and reboot into it | 400 `invalid_config` (`detail` names the field); 409 `busy`; 413 |
| `POST /device/network/confirm` | commit the trial config | 409 `no_trial`; 403 `wrong_interface` |
| `POST /device/reboot` | reboot | 409 `busy` while an update or push runs |

Thread REST API (upstream esp-thread-br, ot-br-posix 0.6.0 compatible): `/node`, `/node/state`, `/node/dataset/active|pending`,
`/node/ba-id`, `/node/ext-address`, `/node/coprocessor/version`, `/node/ba-epskc/*`, `/node/commissioner/*`, `/api/actions`,
`/api/devices`, `/api/diagnostics`, `/api/node`, `/.well-known/thread/br-rest`. In `SNIFFER` these answer 503 (reads) or 409
(changes); in `STARTING` and `RADIO_FAULT` they answer 503. `DELETE /node/dataset/...` is not supported. The mode, fault reason
and version information are in `/device/status`, which works in every mode. Home Assistant's OpenThread Border Router integration
connects to `http://<board>` (verified with `python-otbr-api` by `tools/otbr_compat.py`; a run against a real Home Assistant is
still open, see `docs/phase0/RESULTS.md`).

`/device/status` modes: `STARTING`, `BR_RUNNING`, `SNIFFER`, `RADIO_FAULT`.

### Web UI

`http://<board>/` serves the upstream pages (Dashboard, Network to form or join, Commissioner, Addresses, Tools, Topology,
About) plus `/device.html` (status, sniffer, OTA and radio firmware upload, jumper actions, network configuration with the
confirm banner, diagnostics). `device.html` uses only `/device/*`, so it also works in `RADIO_FAULT` and while sniffing.

### Creating a Thread network

The board never forms or joins a network by itself. Use the Web UI (Network page: form or join), or the REST API / Home
Assistant / `python-otbr-api`:

```
import asyncio, aiohttp, python_otbr_api as api

async def main():
    async with aiohttp.ClientSession() as session:
        otbr = api.OTBR("http://<board>", session)
        await otbr.create_active_dataset(api.models.ActiveDataSet(network_name="MyThread", channel=15))
        await otbr.set_enabled(True)

asyncio.run(main())
```

The dataset is stored on the board and used again at every boot. `GET /node/state` returns `leader` (alone) or
`router`/`child`/`detached`.

## Network modes and VLANs

The default is `single`: one untagged interface with DHCPv4 for everything. `split` separates two interfaces, `mgmt`
(web UI, `/device/*`, read-only `GET /node/*`) and `thread_bb` (the OpenThread backbone and the full Thread REST API
for Home Assistant), each untagged or on a VLAN ID (1..4094, distinct, at most one untagged). In split mode:

- Nothing is forwarded to or from `mgmt`. Replies leave by the interface the request came from.
- A request on the wrong interface is refused with 403 and the connection is closed.
- mDNS (hostname and every service, including `_meshcop._udp`) runs only on `thread_bb`; `mgmt` is never announced. A
  gateway that reflects mDNS between the VLANs may still show the `thread_bb` records on the `mgmt` VLAN; that is the reflector.

Change it with the device page or `PUT /device/network`, for example:

```
curl -X PUT http://<board>/device/network -d '{"mode":"split","mgmt":{"vlan":10,"ipv4":{"dhcp":true}},"thread_bb":{"vlan":20,"ipv4":{"dhcp":true}}}'
```

Shape: `{"mode": "single"|"split", "single"|"mgmt"|"thread_bb": {"vlan": 0|1..4094, "ipv4": {"dhcp": true} | {"dhcp": false, "addr", "mask", "gw", "dns"}}}`.
`vlan: 0` is untagged. The switch port must carry the VLANs before you apply.

Confirm-or-revert: the change is stored as a trial and the board reboots into it. You then have 5 minutes to call
`POST /device/network/confirm` (or press Confirm on the device page) from the management side, that is the `single` interface or
`mgmt` of the trial config. If you do not (or the board resets first), the trial is dropped and the board reboots into the previous
config. If the board is unreachable after a network change: wait 5 minutes, or use the console, below.

Console (USB-C port, 115200 baud, any build): `net show` prints the active config, trial and interfaces; `net reset` restores
`single`, untagged, DHCPv4 and reboots.

## DHCPv6 prefix delegation

Always on. `pd.state` and `pd.prefix` in `/device/status` are `null` when OpenThread was busy and its state could not be read (ask again). `pd.state` mirrors OpenThread's DHCPv6-PD client (`disabled`, `stopped`, `running`). The client
is `running` while it solicits, so with no delegating router `pd.state` is `running` and `pd.prefix` is `null`; the local ULA
prefix is then the OMR prefix. With a delegation, Thread devices get global addresses from it: this makes them reachable from
the Internet unless your router's IPv6 firewall blocks it. A delegating router must offer lifetimes above 300 s (OpenThread
raises T1/T2 to at least 300 s).

## Maintenance jumper

Three wires between the P4 header and the C6 header H7 (table in `docs/first-install.md`). With it fitted the P4 can write the
C6's bootloader, partition table and updater by itself (ROM recovery): automatically at boot when the updater does not answer
or its version differs from the one embedded in the P4 app (once per boot), or on demand with `POST /device/radio/recover`
(also the "Recover C6" button). The request answers 200 once the base image was rewritten, with `rcp` telling what is on the C6 afterwards (`no rcp firmware`, or the installed version); only a failed recovery answers 502 `recovery_failed`. Recovery restores the C6 base image only; the RCP firmware is left alone if its slot is
intact, otherwise upload it again. Each flash region is written and verified up to three times; every retry reconnects to the
ROM loader and drops the link speed (1.5 Mbaud, then 460800, then 115200 baud), so a noisy wire costs time, not the recovery;
the worst case is about two minutes. The jumper also lets the P4 log the C6 console (`c6:` lines, at most 20 per second; a C6 boot loop shows one
`c6: <n> lines suppressed` line per second instead) and serve all of it at
`GET /device/radio/log`. `jumper` in `/device/status` shows presence and base image versions. Without the jumper both
endpoints answer 412 `no_jumper`.

The embedded base image version is content-addressed (`base-<8 hex>`, `tools/base_version.cmake`: a hash of the updater
sources, the shared protocol and board headers, the updater `sdkconfig.defaults`, the C6 partition table and the IDF
version), so an ordinary P4 update does not touch the C6. Only a P4 release whose base image really changed runs the automatic
ROM recovery once at boot. The recovery erases the C6's `otadata`; the updater then follows the C6 bootloader, which boots the
first valid slot (`ota_0`, then `ota_1`), so an RCP installed by a push into `ota_1` survives. If `rcp.installed` is empty
afterwards, no valid RCP image was left in either slot (new board, erased slots): upload `c6_rcp.bin`.

## Diagnostics

`GET /device/diag` returns the precomputed snapshot of a sampler that runs once per second: per-core load over 1/10/60 s,
per-task CPU share, core, priority, minimum free stack, internal RAM and PSRAM figures, OpenThread lock statistics (count,
longest hold and wait, holder), uptime and last reset reason. The lock statistics cover everything that takes the OpenThread
lock through `esp_openthread_lock_acquire/release` (libopenthread, `br_core`, the Thread REST server); the border router
internals in the prebuilt `libopenthread_br.a` only use the task-switching lock, which is not counted: their holds show up
only as wait time of the others. Diagnostics are optional: if the sampler cannot start, the boot goes on and `/device/diag`
answers 503 `not_started`. Tasks beyond the 48 the sampler tracks are left out of the snapshot (one log line). The device page shows it refreshed every 2 s. Tasks are pinned:
core 0 for the network path (lwIP, Ethernet, OpenThread, mDNS), core 1 for management (HTTP, workers, `rcp_sync`, console).

## Vendored code

`esp_ot_br_server` and `esp_br_http_ota` come from esp-thread-br commit `1f5af46d0ccb22fe01465ba6d16a33b276c20c1b`. Every
change to them (patches 1 to 12) and the link-time mDNS filter for `espressif/mdns` (patch 13) is listed in
[`p4/components/PATCHES.md`](p4/components/PATCHES.md), together with the REST guard table (which endpoint answers 409 or
503 in which mode).

## Testing

- Host tests (Linux target, no board): `tools/run_host_tests.sh`, `tools/run_vendored_rest_tests.sh`, and
  `uvx --with requests pytest tools/tests` (the device page tests need `node`).
- Lint and format: `uvx pre-commit run --all-files`.
- Target tests need the board with the Thread peer (an ESP32-C6 or H2 devkit running `ot_cli` on a second serial port) and
  run against a `--test` build, which the fixtures flash themselves:

  ```
  . ~/esp/esp-idf-v6.1/export.sh
  timeout 900 .venv/bin/pytest p4/pytest/test_br_boot.py --port /dev/ttyACM0 -p no:cacheprovider
  ```

  Always pass `--port` (without it pytest-embedded scans every serial port, including the Thread peer, and hangs),
  activate the IDF environment in the same shell, wrap runs in `timeout`, and check `fuser -v /dev/ttyACM0` for stale
  processes first. The suites are slow (reboot-heavy; the whole of `p4/pytest` takes about 40 minutes), so run the
  files you need. Tests that need a person or special rig are deselected by marker and run with `-m <marker>`:
  `manual_cable` (unplug and replug Ethernet, with `-s`), `no_jumper` (remove the jumper wires), `vlan_switch`
  (managed switch and Docker macvlan hosts, env `VLAN_MGMT`, `VLAN_THREAD`, `HOST_IF_MGMT`, `HOST_IF_THREAD`),
  `pd_router` (delegating server in Docker, env `PD_HOST_IF`). A test that kills the radio (`rcp_kill`, `c6_hold`) leaves
  a flag: after an aborted run use the console `c6_hold 0` and reboot. A Thread peer that stops answering is fixed by an
  RTS reset.
- Test builds add console commands (`p4/main/test_cmds.c`) that the target tests use; never ship them.

## Known limitations and non-goals

- No authentication or TLS anywhere: anybody on the network can reconfigure the board, read the Thread dataset with
  `GET /node/dataset/active` (also from the `mgmt` VLAN: read-only `GET /node/*` is served there by design) or update its firmware. Put it on a
  trusted network. `https://` URLs are not supported for downloads.
- DHCPv6-PD makes Thread devices globally addressable; rely on the upstream router's firewall.
- Requests for the mesh topology (`/topology`, `/api/diagnostics`) add roughly 100 ms to the latency of pings through the border router
  while they run (Diagnostic Get traffic).
- Flash writes (OTA) use XIP from PSRAM so traffic keeps flowing, but the P4 reboots after an update.
- The `-dirty` flag of the app version stamp is as of the last configure (see Build); a new commit refreshes the stamp.
- Untested on hardware: the no-jumper first install (`docs/first-install.md`, Path 2, esptool over H7).
- Untested on hardware: recovery of a truly blank C6 (bootloader, partition table and otadata erased) and overwriting the factory
  ESP-Hosted firmware.
- A mDNS reflector between the two VLANs is tolerated, but a host that claims the board's mDNS name with different addresses can
  rename it (standard mDNS behaviour).
- Radio firmware updates interrupt the border router for a few seconds; seamless updates are not implemented.
- Not verified on hardware: the `br_core` give-up path after repeated radio failures, and the fall-back to default network config
  when the stored config cannot be applied (host-tested only).

## Troubleshooting

`GET /device/status` shows `mode` and `fault_reason`. In `RADIO_FAULT` the Thread API answers 503; `/device/*`, the device page and
OTA keep working.

| `fault_reason` | Meaning and fix |
| --- | --- |
| `no rcp firmware` | The C6 has no valid RCP image (new board, erased slots). Upload `c6_rcp.bin`. Not retried automatically. |
| `rcp incompatible` | OpenThread rejected the RCP, or it did not answer its reset. Upload the `c6_rcp.bin` of the same release. |
| `storage unavailable` | NVS did not initialise (for example no free pages), `storage_ok` is `false`. The board booted degraded instead of boot-looping: HTTP, OTA and `/device/*` work, OpenThread does not start. NVS is never erased automatically because it holds the Thread credentials. Update to a firmware that fixes it, or erase NVS by hand (`esptool.py erase-region 0x9000 0x10000`; the Thread network has to be set up again) and reboot. |
| `rom recovery failed` | The updater was silent and the jumper recovery failed. Check the jumper wires and `GET /device/radio/log`; try `POST /device/radio/recover`; without a jumper use the no-jumper path. |
| `resetting the RCP failed`, `radio boot timed out`, `radio restart failed`, `OpenThread main loop ended` | Radio did not come up. The board retries (at most three automatic recoveries per 10 minutes), then waits: reboot, or upload firmware. Check the antenna and the C6 console log. |

Other problems:

- Board unreachable after a network change: wait 5 minutes for the revert, or use the console `net reset`.
- No IP address: check the cable and the switch port (VLAN setup), `net show` on the console.
- `GET /api/node` / `/api/devices` show `state` `disabled` (name `OpenThread`, rloc16 `0xfffe`) although `GET /node` says `leader`:
  upstream esp-thread-br builds that collection entry once and never refreshes it (see `p4/components/PATCHES.md`, "Known upstream
  behaviour"). Use `/node`, `/node/state` or `/node/dataset/active` (what Home Assistant uses) for the live state.
- Thread REST answers 503 `radio not available`: see `mode` above; 409 `sniffer active`: a capture is running (`DELETE /device/sniffer`).
- The board boots in `RADIO_FAULT` after a test: see Testing (`c6_hold 0`).
- Recovery from a bad P4 flash: reflash over USB (`idf.py -C p4 -p /dev/ttyACM0 erase-flash flash`). See `docs/first-install.md`
  for C6 recovery.

## License

Copyright (C) 2026 Igor Yamolov. Licensed under the [GNU Affero General Public License v3.0 or later](LICENSE), with an
additional permission to link Espressif's binary-only radio libraries (see [`NOTICE`](NOTICE)). If you run a modified
version that other people use over the network (the web UI or the REST API), you must offer them its source code.

The vendored esp-thread-br components stay under Apache-2.0 (`p4/components/esp_ot_br_server/LICENSE`,
`p4/components/esp_br_http_ota/LICENSE`). Files derived from ESP-IDF examples keep their CC0-1.0 / Unlicense headers.
