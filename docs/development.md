# Development

Building and flashing: [building.md](building.md). Conventions (naming, formatting, errors, tests): `STYLEGUIDE.md`. Design:
`docs/superpowers/specs/2026-10-30-esp32-p4-otbr-design.md` (the whole firmware), plus the later feature specs in
`docs/superpowers/specs/` (device pages, Prometheus metrics, network names) and the plans in `docs/superpowers/plans/` (among them the
no-wires first install through ESP-Hosted).

## Repository layout

- `p4/`: the ESP32-P4 firmware (components `br_core`, `device_api`, `netcfg`, `netnames` (mDNS and DHCP names),
  `sniffer`, `diag`, `metrics`, `rcp_sync`, `c6_hosted` (just enough of the ESP-Hosted protocol to send
  `c6_installer.bin` to a factory C6), vendored `esp_ot_br_server` and `esp_br_http_ota`); `p4/pytest/` holds the on-board tests.
- `c6_updater/`: the small, frozen C6 program that receives the RCP firmware from the P4 and writes it to flash.
- `c6_installer/`: the one-time C6 app sent through ESP-Hosted on a factory C6; it writes the `c6_updater` bootloader,
  partition table and updater, then restarts into the updater (`c6_installer.bin`).
- `c6_rcp/`: the C6 radio firmware (`c6_rcp.bin`), an OpenThread RCP with SPI spinel.
- `common/`: code shared by the projects (`c6_board` pins and C6 flash layout, `rcp_sync_proto`, `board_cores`).
- `tools/`: the scripts behind the Makefile (`build_all.sh`, `run_host_tests.sh`, `run_vendored_rest_tests.sh`,
  `package_release.sh`, `make_bundle.py`), `version.cmake`, `base_version.cmake`, `test_build.cmake`,
  `c6_updater_images.cmake`, `otbr_compat.py` (python-otbr-api driver) and `tools/tests/` (tool tests).
- `docs/`: this documentation, the design spec and plan, Phase 0 results (hardware facts).
- `IDF_VERSION`: the pinned ESP-IDF version.

## Vendored code

`esp_ot_br_server` and `esp_br_http_ota` come from esp-thread-br commit `1f5af46d0ccb22fe01465ba6d16a33b276c20c1b`. Every
change to them and the link-time mDNS filter for `espressif/mdns` is listed in
[`p4/components/PATCHES.md`](../p4/components/PATCHES.md) (patches 1 to 15; 4 and 6 were not applied, 13 is the mDNS
filter), together with the REST guard table (which endpoint answers 409 or 503 in which mode). They stay under Apache-2.0.

## Testing

```
make test            # all host-side tests: test-host, test-tools, test-vendored (no board)
make lint            # formatting and lint (pre-commit)
```

- `make test-host`: the component host tests on the ESP-IDF linux target.
- `make test-tools`: tests of the tools and of the device page scripts (needs `node`).
- `make test-vendored`: the upstream esp_ot_br_server REST tests (needs `clang++` and a `make build` for the managed
  components).

On-board tests need the board with the Thread peer (an ESP32-C6 or H2 devkit running `ot_cli` on a second serial port) and
run against a test build, which the fixtures flash themselves:

```
make build-test
make test-target TEST=p4/pytest/test_br_boot.py
make test-target TEST=p4/pytest/test_jumper.py MARK=jumper
```

`make test-target` always passes `--port $(PORT)` (without it pytest-embedded scans every serial port, including the Thread
peer, and hangs) and wraps the run in `timeout` (`TIMEOUT=900` seconds). Check `fuser -v /dev/ttyACM0` for stale processes
first. The suites are slow (reboot-heavy; the whole of `p4/pytest` takes about 40 minutes), so run the files you need.
Tests that need a person or a special rig are deselected by marker and run with `MARK=<marker>`:

- `manual_cable`: unplug and replug Ethernet (add `-s` by running pytest directly).
- `no_jumper`: remove the jumper wires.
- `vlan_switch`: managed switch and Docker macvlan hosts, env `VLAN_MGMT`, `VLAN_THREAD`, `HOST_IF_MGMT`, `HOST_IF_THREAD`.
- `pd_router`: delegating server in Docker, env `PD_HOST_IF`.

`test_hosted_install.py` (the no-wires first install) skips unless `OTBR_C6_STOCK_URL`, `OTBR_C6_INSTALLER` and
`OTBR_RCP_IMAGE` are set. `OTBR_C6_STOCK_URL` is an HTTP server on the build directory of an ESP-Hosted C6 slave (e.g.
esp-hosted-mcu 0.0.27 `slave/`, `idf.py set-target esp32c6 build` with IDF v5.4); the test writes it to the C6 over the
jumper (console `c6_stock_restore`, with the C6 held in reset) and hides the jumper from the probe (`c6_jumper_ignore`).
The board ends up in `BR_RUNNING` with the given RCP.

A test that kills the radio (`rcp_kill`, `c6_hold`) leaves a flag: after an aborted run use the console `c6_hold 0` and
reboot (and `c6_jumper_ignore 0` after an aborted `test_hosted_install.py`). A Thread peer that stops answering is fixed by an RTS reset. Test builds add console commands (`p4/main/test_cmds.c`)
that the on-board tests use; never ship them.

## CI and releases

`.github/workflows/build.yml` runs on every push to `master` and every pull request, in the `espressif/idf` container of
`IDF_VERSION`: `make test-host`, `make test-tools`, `make build`, `make test-vendored` and `make package`. The workflow keeps
the packaged files as the `firmware` artifact of the run. No board is involved; the on-board tests stay on the bench.

Pushing a `v*` tag also publishes a GitHub release with those files. Its notes start with the tag's section of
`CHANGELOG.md`, so move the "Unreleased" entries under the new version first:

```
git tag v0.2.0
git push origin v0.2.0
```

| File | What |
| --- | --- |
| `otbr.bundle` | P4 app and web UI, for an OTA update |
| `c6_rcp.bin` | C6 radio firmware from the same build |
| `p4-flash.zip` | full P4 flash over USB: `esptool --chip esp32p4 write-flash @flash_args` in the unpacked directory |
| `c6_installer.bin` | one-time C6 installer for the no-wires first install on a factory C6 (ESP-Hosted) |
| `c6-updater-flash.zip` | C6 base images for the USB-UART first install |
| `third-party-licenses.zip` | license texts of the code linked into the images |
| `versions.txt`, `SHA256SUMS` | build versions and checksums |

The firmware versions stay `<build timestamp>-<git hash>`; the tag only names the release.
