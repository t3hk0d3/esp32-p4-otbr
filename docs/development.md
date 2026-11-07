# Development

Building and flashing: [building.md](building.md). Conventions (naming, formatting, errors, tests): `STYLEGUIDE.md`. Design:
`docs/superpowers/specs/2026-10-30-esp32-p4-otbr-design.md` (the whole firmware), plus the later feature specs in
`docs/superpowers/specs/` (device pages, Prometheus metrics, network names, TREL) and the plans in `docs/superpowers/plans/` (among them the
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
[`p4/components/PATCHES.md`](../p4/components/PATCHES.md) (patches 1 to 21; 4 and 6 were not applied, 13 is the mDNS
filter), together with the REST guard table (which endpoint answers 409 or 503 in which mode). They stay under Apache-2.0.

`usb_host_cdc_acm` is `espressif/usb_host_cdc_acm` 2.4.1 from the component registry, vendored so the radio mux can
patch it; `override_path` in `p4/components/radio_mux/idf_component.yml` makes every user of the component (the CH34x
and CP210x drivers, esp-serial-flasher) build this copy. Its changes are in the same `PATCHES.md`.

## sdkconfig

`p4/sdkconfig` (like the C6 projects' `sdkconfig`) is generated from `sdkconfig.defaults` and not tracked. ESP-IDF
applies the defaults only to options the existing `sdkconfig` does not set, so `make build` and `make build-test`
delete a project's `sdkconfig` when one of its `sdkconfig.defaults*` files is newer and let ESP-IDF generate it again.
Local `menuconfig` changes are lost then; keep them in the defaults. A plain `idf.py build` does not do this: after
pulling a change to the defaults, delete `sdkconfig` yourself. `routing/task.c` stops the build with an `#error` when
the socket limit (`CONFIG_LWIP_MAX_SOCKETS`) is below what the image needs.

## Testing

```
make test            # all host-side tests: test-host, test-tools, test-vendored (no board)
make lint            # formatting and lint (pre-commit)
```

- `make test-host`: the component host tests on the ESP-IDF linux target. Projects build and run `HOST_TEST_JOBS` at
  a time (default 4); a failing project's output is printed at the end, after all projects have run.
- `make test-tools`: tests of the tools and of the device page scripts (needs `node`).
- `make test-vendored`: the upstream esp_ot_br_server REST tests (needs `clang++` and a `make build` for the managed
  components).
- `python3 tools/topology_preview.py --port 8765`: serves the frontend on localhost with canned `/device/topology`
  (the test fixture's result), `/ping` and `/device/status` answers, to look at the Topology page without a board
  (needs `node`).

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

**Latency tests (`test_latency.py`)** ping the Thread peer's OMR address through the board, so the pings must come from a
host that has a route to the Thread network. Don't change the host's own networking. Use a disposable container on a
Docker macvlan network on the board's Ethernet segment instead. It accepts the board's router advertisements, and their
route information option gives it the OMR prefix via the board:

```
make lan-ping-up
tools/lan_ping.sh status
PING_PREFIX="docker exec otbr-lan-ping" make test-target TIMEOUT=3000 TEST=p4/pytest/test_latency.py
make lan-ping-down
```

- **Status check:** `tools/lan_ping.sh status` must list the OMR prefix `via fe80::… proto ra`. Every test forms a fresh
  network with a new prefix, and the container follows the board's next RA.
- **Interface:** `LAN_IF` (default `mgmt0`) is the host interface on the board's segment, untagged in single mode.
- **Network:** the script reuses the macvlan network `LAN_NET` (default `otbr-p0`) or creates it.
- **IPv4:** the container has no IPv4, so the `pcap_stream` load, which pings the board's IPv4 address, needs the host.
  Select the loads with `-k`, e.g. `TARGET_PYTEST=".venv/bin/pytest -k 'topology_requests or node_requests'"`.
- **Settings are erased:** the fixtures erase the board's NVS and form a new network, so the board leaves any network it
  was on.
- `pd_router`: delegating server in Docker, env `PD_HOST_IF`.
- `routing_rig`: FRR and BIRD routers in Docker for the mesh routing tests, env `ROUTING_HOST_IF` (and `TIMEOUT=1800`;
  `ROUTING_FEED_ROUTES` sets the size of the received-route feed, default 10k). `ROUTING_NETWORK` uses an existing docker
  network instead of creating a macvlan one (for a VLAN whose host interface already carries ipvlan), and
  `ROUTING_BOARD_URL` (for example `http://192.168.10.104`) points the tests at the board's management address in split
  mode. Flash once (`make build-test && make flash`) and pass
  `TARGET_PYTEST=".venv/bin/pytest --skip-autoflash y"`: without it every test would flash the board again, so
  `test_routing.py` refuses to run. The PUT validation tests (`test_routing_api.py`) need no rig and run unmarked.

`test_hosted_install.py` (the no-wires first install) skips unless `OTBR_C6_STOCK_URL`, `OTBR_C6_INSTALLER` and
`OTBR_RCP_IMAGE` are set. `OTBR_C6_STOCK_URL` is an HTTP server on the build directory of an ESP-Hosted C6 slave (e.g.
esp-hosted-mcu 0.0.27 `slave/`, `idf.py set-target esp32c6 build` with IDF v5.4); the test writes it to the C6 over the
jumper (console `c6_stock_restore`, with the C6 held in reset) and hides the jumper from the probe (`c6_jumper_ignore`).
The board ends up in `BR_RUNNING` with the given RCP.

`test_trel.py` links the board with a second TREL border router when the TREL test peer runs (`tools/trel_peer`):
ot-br-posix from the official `openthread/border-router` image (TREL built in) on the board's backbone link through Docker
macvlan, with the Thread peer devkit as its radio. Its tests skip otherwise.

```
tools/trel_peer/flash_c6.sh rcp                  # the C6 devkit on $PEER_PORT (/dev/ttyACM1) becomes a UART RCP
LAN_IF=enp3s0 LAN_SUBNET=192.168.1.0/24 LAN_GATEWAY=192.168.1.1 PEER_IP=192.168.1.240 tools/trel_peer/up.sh
make test-target TEST=p4/pytest/test_trel.py     # the tests join the peer to the board's network themselves
tools/trel_peer/join.sh <board>; tools/trel_peer/status.sh   # by hand: join, then TREL peers, counters, radios
tools/trel_peer/down.sh; tools/trel_peer/flash_c6.sh cli     # back to the ot_cli Thread peer
```

`LAN_IF` is a wired interface on the backbone link (in split mode the host's VLAN interface of `thread_bb`); see
`up.sh` for the variables. While the devkit is the RCP, run no other on-board tests: the `peer` fixture would write CLI
commands into the RCP's UART.

A test that kills the radio (`rcp_kill`, `c6_hold`) leaves a flag: after an aborted run use the console `c6_hold 0` and
reboot (and `c6_jumper_ignore 0` after an aborted `test_hosted_install.py`). A killed `test_routing.py` or
`test_routing_api.py` run (for example by the `timeout`) leaves the board's routing settings changed and the ones it found in
`p4/pytest/.routing-restore.json`; put them back and delete the file (the next run refuses to start while it exists):
`curl -X PUT http://<board>/device/routing -H 'Content-Type: application/json' -d @p4/pytest/.routing-restore.json && rm p4/pytest/.routing-restore.json`. A Thread peer that stops answering is fixed by an RTS reset. Test builds add console commands (`p4/main/test_cmds.c`)
that the on-board tests use; never ship them.

## CI and releases

`.github/workflows/build.yml` runs on every push to `master` and every pull request, in the `espressif/idf` container of
`IDF_VERSION`: `make test-host`, `make test-tools`, `make build`, `make test-vendored` and `make package`. The workflow keeps
the packaged files as the `firmware` artifact of the run. No board is involved; the on-board tests stay on the bench.
A pull request that changes only documentation (Markdown files, `docs/`, `mkdocs.yml`, `tools/docs_hooks.py`, the docs
workflow) skips the tests and the build; the skipped jobs still pass as status checks. Pushes always build.

`make sizes` (also run by the workflow, into the job summary) lists each image's size, its flash region and the space left
there. The run keeps the numbers as the `sizes` artifact; on a pull request a follow-up job compares them with the
`sizes` artifact of the master build of the PR's base commit (or the latest successful master build) and posts the table
with the deltas as a PR comment, updated on every push. Pull requests from forks only get the job summary.

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

## Documentation site

`mkdocs.yml` builds `docs/` with Material for MkDocs into a GitHub Pages site. Its home page is the landing page
`docs/overrides/home.html` (styles in `docs/assets/home.css`, web UI screenshots in `docs/images/home/`), `README.md`
becomes the Overview page, and `docs/superpowers/` and `docs/phase0/` stay out. `tools/docs_hooks.py` keeps the links written for GitHub working: links
to pages and images stay relative, links to anything else in the repository become GitHub URLs. A new page goes into the
`nav` of `mkdocs.yml` as well as `docs/README.md`. `make docs` builds it with `--strict` (a broken link fails), `make
docs-serve` previews it.

`.github/workflows/docs.yml` builds the site on pull requests that touch the docs and publishes it from `master` and
after every successful `v*` build. Before publishing, `make docs-firmware` puts the P4 images of the latest release and
an [ESP Web Tools](https://esphome.github.io/esp-web-tools/) manifest (`tools/flash_manifest.py`, from `flash_args`)
into `site/firmware/`, which [Flash in the browser](flash.md) uses. Repository settings: Pages, source "GitHub
Actions".
