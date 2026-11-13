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
- `tools/zigbee_test_device/`: the rig's Zigbee test device, an on/off light router for the aux C6 devkit
  (`make zigbee-test-device`; driven by `p4/pytest/zigbee_device.py`).
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

`p4/sdkconfig` (like the C6 projects' `sdkconfig`) is generated from `sdkconfig.defaults` and not tracked.
`make build-test` configures the P4 into its own `p4/sdkconfig.test`, from `sdkconfig.defaults` plus
`sdkconfig.defaults.test` (Debug and Verbose logging compiled in); switching between the two rebuilds everything. ESP-IDF
applies the defaults only to options the existing file does not set, so `make build` and `make build-test` delete
that file when one of the defaults files it is made from is newer and let ESP-IDF generate it again.
Local `menuconfig` changes are lost then; keep them in the defaults. A plain `idf.py build` does not do this: after
pulling a change to the defaults, delete `sdkconfig` yourself. `routing/task.c` stops the build with an `#error` when
the socket limit (`CONFIG_LWIP_MAX_SOCKETS`) is below what the image needs, and `logging/logging_capture.c` does the
same for a stale `sdkconfig` that still has the secondary console on USB-Serial-JTAG, log version 2 or fewer than 32
UDP PCBs, and for a log ceiling that doesn't match the build (`CONFIG_LOG_MAXIMUM_LEVEL` above Info in a release
build, below Verbose in a test build).

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

## On-board checks

Checks beyond the automated tests, and what has not been run on hardware yet. The rules they check are in
[Internals](reference/internals.md).

### Not verified on hardware

- The USB-UART first install ([First install](first-install.md), Path 3, esptool over H7), and recovery of a truly
  blank C6 (bootloader, partition table and otadata erased).
- The case where the no-wires first install (Path 1) lands the installer in the slot it must overwrite (a factory C6
  updated before) and moves itself first. Path 1 itself ran on an untouched factory C6 (2026-11-13, v0.6.0).
- The `br_core` give-up path after repeated radio failures, and the fall-back to the default network config when the
  stored config cannot be applied (host-tested only).
- Several boards on one Thread network.
- Zigbee mode: built and host-tested; its on-board tests (`p4/pytest/test_zigbee.py`) are written.
- The CH34x status-packet decoding (`uart_errors`) follows the ch34x driver's own defines
  (`CH34X_UART_STATE_TRANSIENT_MASK`, byte 0) and is not confirmed against a datasheet.

### Radio firmware from the browser flasher

Built and tested on the host (`radio_console` host tests, `tools/tests/test_flash_page.py` against a scripted board),
never run on a board. To check on the rev3 board with a factory C6 (from the docs preview with a published release, or
the `radio-fw` commands by hand in a serial terminal):

- Factory C6 end to end from the flasher: installer, the updater answering, then the RCP; `/device/status` afterwards.
- C6 with the updater: RCP only; the release's RCP already installed (**Install it anyway**).
- The cable tests need the board on PoE (or another supply): over USB-C alone, unplugging the cable is a power cut,
  and during the installer that leaves a C6 for the maintenance jumper or a USB-UART adapter.
- Unplug the cable while sending: `radio-fw load` ends with `error timeout` after 10 s, the C6 unchanged, the console
  usable again.
- Unplug the cable right after the last frame and while the installer runs: the push finishes; **Reconnect** shows
  the result.
- The console after each load: the frames are read from the REPL's own UART driver (256-byte RX ring); count CRC naks
  (overruns) in a transfer; log lines during the transfer don't break it; typing still works after.
- Transfer time at 115200 baud (about 35 s for `c6_installer.bin`, 25 s for `c6_rcp.bin` expected).

### Time sync

Spec `docs/superpowers/specs/2026-11-10-time-sync-design.md` section 8. Run on 2026-11-10 with `20261110T025922Z-06f7cb5`
in split mode (management untagged, Thread backbone on VLAN 49):

1. Power-up on the home network: the clock is set within seconds; `source` matches what the router hands out. **Passed**:
   synced about 20 s after the web server came up, to the second; `pool.ntp.org` with no server in the lease, then
   `source: dhcp`, `dhcp_iface: mgmt` once the management DHCP named one. No "unreachable" right after a sync.
2. Zigbee `started` shows a real date although the stack started before the first sync. **Not run** (needs Zigbee
   mode).
3. Soft restart: the clock stays valid; Last synced reads "Before the last restart" until the next answer. **Passed**
   (with sync off).
4. A custom server that does not exist: **Server not answering** after a minute, the clock keeps running. Two quick
   saves in a row: the last one wins. **Passed**; a bad server gets a 400.
5. Off: no NTP packets on the wire (capture on the switch). **Passed at API level only**: a capture on another VLAN saw
   nothing either way, so the wire proof is still open.
6. Split mode with servers on both leases: which lease wins, and whether `dhcp_iface` names it. A DHCP renewal does not
   restart the sync (`synced` stays). **Not run.**

The timesync task's stack peaked at 1260 of 3072 bytes. Known: the reply to a `PUT` shows the old `servers` and
`source` until the task applies the change (the page reads the status again half a second after a save).

### Radios

The on-board tests are in `p4/pytest/test_channels.py` (the C6 alone) and `p4/pytest/test_dongle.py` (marker
`usb_dongle`, needs the OllyZig and a person at the board: run pytest with `-s -m usb_dongle`). Beyond the tests, the checks that need a logic analyser, a test build or long observation:

- **Gap under load** (C6): an RCP-heavy session for 10 minutes (proxy peer streaming, an energy scan) leaves `rx_errors`
  at or near 0; with a logic analyser on CS and INT, no CS fall comes less than 100 µs after the previous rise (if
  errors appear, raise `C6_GAP_US`).
- **Refused connects** (`test_proxy_connect_refused_soak`, 10 minutes by default): afterwards the board still serves three waves
  of 7 concurrent HTTP requests (every web slot) and the proxy still accepts a client. `/device/diag` has no socket count, so this sees
  exhaustion, not a slow leak below the socket budget; compare the log's back-off schedule (1 to 30 s) by eye.
- **Pool exhaustion**: a proxy peer that does not read: drops are counted and the RCP keeps answering.
- **Reset timing**: from a new peer's connect to the first valid header from the C6 takes 300-500 ms.
- **Quick replug** (under 1 s): one close, then a normal detection; Thread starts once.
- **Start failure after a probe** (wrong baud forced through the test build): Thread back on the C6, the dongle in `fault`;
  with a stuck init, a reboot onto the old radio.
- **Drain timeout**: hold a REST session for more than 45 s during a move: the target shows `fault` `drain_timeout`, Thread
  stays on the source.
- **Silent dongle**: the recovery ladder (reopen, spinel reset), and after three recoveries `RADIO_FAULT` without abort.
- **Dead dongle after a reset** (the CH340 OllyZig that stops answering after a reopen), OpenThread on it: one reopen,
  then `RADIO_FAULT` "RCP did not answer its reset", no more reopens in the log; a PUT of `otbr` on the channel starts it
  again, and an unplug and replug detects it again. The "no answer after 3 resets" give-up is checked only with a
  consumer that resets on its own timeouts (Zigbee mode, on the merged branch). `uart_errors` counts, and at most one
  CH34x warning per 10 s, with no "Unsupported notification type" lines.
- **USB reopen under traffic** (`test_usb_reopen_under_traffic_survives`, test build: the `usb_reopen` console command):
  120 reopens while the dongle streams answers: no abort (PATCHES.md patch 33) and the radio answers again (the first
  request after the last reopen may be lost, a cut outgoing frame). Checked 2026-11-07. The CH340 never stayed silent
  after a reopen on the bench, also after forced overruns (`usb_stall <ms>` masks a core's interrupts) and with spinel
  resets in between (about 400 reopens); `ch34x diag|init|clear` (test build) read the chip's registers and send the
  open-time init that ESP-IDF's ch34x driver skips, if it ever does. `usb_reopen` runs the reset path of a consumer's
  reset (only on an attached or leased dongle; a failed reopen detaches it, and three in a row with nothing received
  give up with "no answer after 3 resets"), so the test sends a burst the dongle answers between two reopens.
- **C6 maintenance**: a firmware push with OpenThread on the dongle leaves OpenThread alone, also while the dongle
  captures or starts (the board mode is the dongle's); unplugging the dongle during it gives `NO_RADIO` at once; with a
  proxy peer on the C6 it gives 409 `c6_busy`.
- **Boot**: a stored dongle that is unplugged gives `NO_RADIO` without a fault; plugging it in gives `BR_RUNNING`.
- **Heap**: about 21 KB of internal DMA RAM is taken at the first C6 open, about 39 KB (pool, buffers, 16 KB ring) at
  the first dongle open (never on a board without a dongle); a proxy session takes an 8 KB ring (TCP) plus codec
  buffers. Compare the free and the minimum free internal heap (`GET /device/diag`) after boot with no dongle against
  v0.4.0 on the same board: at most 72 KB less (the seven new tasks, below, the queues, the C6 buffers and the
  proxy's resolver). Measured 2026-11-07 (build 2f3b159, 60 s after boot): 284 KB free with no dongle against 350 KB
  on the v0.4.0-era master test build, 65 KB less; 238 KB with a dongle plugged in. The budget was 40 KB before the
  first measurement and was raised to the measured figure plus headroom.
- **Task stacks** (not shrunk: no high-water marks measured yet): `radio_mux` 6144, `usb_lib` 4096, `USB-CDC` 4096,
  `c6` transport 4096, `rcp_proxy` 6144, `rcp_resolve` 4096, `br_radio` 6144 bytes. Read their high-water marks after
  the checks below (a C6 move, a dongle detection, a proxy with a `.local` connect host) before shrinking any.
- **C6 drops under REST load**: with Thread on the C6 and the REST load of `test_unplug_during_rest_load` (without the
  unplug) for 5 minutes, the C6 channel's `drops` and `rx_errors` stay at or near 0 and pings to a Thread device do not
  get lost more than without the load (compare v0.4.0's open ping-loss note).
- **C6 otbr → rcp-proxy → otbr with an otbr-agent peer** (handles of earlier attaches): `RADIO_MUX_AGENT_TEST=1
  pytest -s -m usb_dongle -k otbr_agent` (on the host network by default: it creates `wpan0` and routes on the host;
  with `RADIO_MUX_AGENT_NET=otbr-p0 RADIO_MUX_AGENT_BOARD=<the board's fdaa:1:: address>` the container runs on that
  macvlan network instead and keeps them to itself; either way it advertises a Border Agent on the LAN while it runs.
  Passed that way 2026-11-07). While the old OpenThread drains, the agent gets its RCP's answers and resets, never the board's,
  and the board's OpenThread does not log frames it did not ask for; afterwards Thread is back on the C6 with the same
  network.
- **Two channels in `rcp-proxy` on the same port**: refused with 409 `port_taken` (`test_proxy_port_taken`); a
  listener held by another service shows `backoff` with `last_error` "Address already in use" on the page.
- **Unplug with the console open** running `channel supported` (and `state`) in a loop while the OTBR dongle is
  unplugged: `NO_RADIO`, never `RADIO_FAULT`, no reboot (uptime keeps growing); the open console's commands get
  answers or errors, never a hang, until the session closes with the teardown.
- **Sustained radio -> host stream** (one-MPS IN transfers, PATCHES.md patches 31 and 32: eight transfers in flight;
  checked 2026-11-07: 44808 of 44808 pipelined spinel replies over the proxy, 0 errors; with one, the CH340 at 460800 under Zigbee load counted `uart_errors` 9 and `rx_errors` 5 in
  6 minutes): `rcp-proxy` raw, or `otbr`, on the CH340 at 460800 baud with a sustained radio -> host stream (a spinel
  `PROP_VALUE_GET` flood, or the dongle's own TX burst test) while the REST load of `test_unplug_during_rest_load`
  (without the unplug) runs for 5 minutes: the channel's `uart_errors`, `drops` and `rx_errors` stay 0. Repeat the
  Zigbee load that showed the overrun. Repeat on the CP2102N if one is at hand.
- **HTTP refusal in TCP listen** (port 6638): `curl -X POST http://<board>:6638/ -d x` is refused (`refused` + 1, an RST
  or FIN for curl) with `to_radio` unchanged; `otbr-agent` through `socat` on the C6 still comes up (the reset replay
  once its first bytes pass); a kick during the hold (a client connected with `nc` that has not sent yet) drops it and
  the next client is served; the preconnect case: a client that connects and stays silent stays `listening` (no peer,
  no C6 reset), a second connection replaces it (the first is closed) and becomes the peer with its first spinel
  bytes, and a silent client that then sends `POST / HTTP/1.1` is dropped (`refused` + 1) with `to_radio` unchanged.
- **Stop with an open session**: hold a REST session longer than 45 s while setting the OTBR channel `off`: the PUT
  answers 202, the channel stays `otbr` running with `error` `drain_timeout`, Thread keeps running.
### Logging

The on-board tests are in `p4/pytest/test_logging.py` (the board alone, as a border router with a stored dataset; the
syslog test needs the test host reachable from the board over UDP, the `403` test the VLAN rig, `-m vlan_switch`). They
put the logging settings and the Thread console's setting back as they found them and never erase NVS. The host tests
are in `p4/components/logging/test_host/` and `p4/components/device_api/test_host/` (`test_logging_json.c`); the
Logs page's in `tools/tests/test_logs_page.py`. The rules they check are in
[Internals](reference/internals.md#logging). Not run on hardware yet; beyond the tests, check on the board:

- `stack_free_min_bytes` of `log_out` (5120) and of the short-lived `log_dns` (4096) in `/device/diag` after a syslog
  name lookup and a debug-level session; trim both.
- A syslog name whose DNS server does not answer: USB-JTAG and the live log keep flowing while `log_dns` waits.
- `*` at `debug` with a debug live viewer open and syslog at `debug` to a closed port: the line rate settles (no
  `esp.emac` or `httpd*` debug lines on the viewer or in syslog), and the UART still shows them.
- With a USB breakout: lines on USB-JTAG with a host, none counted as dropped without one, `jtag.host` following the
  cable.
- `log tag OPENTHREAD debug` takes effect at once, and again after the OpenThread lock was busy (the retry timer).
- Ziggurat logging under load does not stall (its ring is in PSRAM), and `log tag ziggurat <level>` is followed.

### Topology scan latency

Mesh topology requests slow down pings through the border router while they run, because of the Diagnostic Get
traffic. Measured with `p4/pytest/test_latency.py` (one Thread peer, a ping every 100 ms, the requests repeated back to
back; 2026-11-03):

- **The page's scan** (`POST /device/topology`) adds about 105–117 ms to the 99th-percentile round trip, and no ping
  is lost. A scan is one discover, then two unicast mesh-diagnostics queries per router (its child table and its
  children's addresses), then one query per full-device child that reported no address.
- **The old `/topology`** (a multicast diagnostic get, no longer used by the page) adds about 77 ms, and 13 of 1000
  pings were lost.
- **For comparison,** `/node` requests add about 2 ms.

### Test-build commands

- `update_source <base>` points the update check and install at another releases base:
  `<base>/latest/download/versions.txt` and `<base>/download/<tag>/<file>`, for example a local HTTP server
  (`p4/pytest/test_self_update.py` does this). An empty argument restores GitHub. The release link on the Firmware
  page then points to the same base, and the page shows it only for an `https://` base.
- `ot <command>` runs an OpenThread CLI line through the same CLI as the Thread console WebSocket (`ot trel` prints
  TREL state and peers). It answers `console in use from <address>` while a WebSocket session is attached, refuses lines
  outside `BR_RUNNING`, and gives up waiting for the prompt after 10 s.
- `tools/make_bundle.py --help` shows how to inspect an `otbr.bundle`.

### Known upstream behaviour

- `GET /api/node` and `/api/devices` show `state` `disabled` (name `OpenThread`, rloc16 `0xfffe`) although `GET /node`
  says `leader`: upstream esp-thread-br builds that collection entry once and never refreshes it (see
  `p4/components/PATCHES.md`, "Known upstream behaviour"). `/node`, `/node/state` and `/node/dataset/active` (what Home
  Assistant uses) are live.

### Core dumps

After a supervisor reboot (`supervisor: the OpenThread lock is stuck` or `... the HTTP server task is stuck`), read the
core dump over USB with `idf.py -C p4 coredump-info`.

## CI and releases

`.github/workflows/build.yml` runs on every push to `master` and every pull request, in the `espressif/idf` container of
`IDF_VERSION`: `make test-host`, `make test-tools`, `make build-release`, `make test-vendored` and `make package`. The workflow keeps
the packaged files as the `firmware` artifact of the run. No board is involved; the on-board tests stay on the bench.
A pull request that changes only documentation (Markdown files, `docs/` except `docs/assets/flasher.js` and
`docs/overrides/flash.html`, `mkdocs.yml`, `tools/docs_hooks.py`, the docs workflow) skips the tests and the build; the
skipped jobs still pass as status checks. Pushes always build.

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
| `<board_id>_<chip_id>.ota` | P4 app and web UI of a firmware target, for an OTA update |
| `otbr.bundle` | copy of the default target's `.ota`; released firmware downloads it by this name |
| `c6_rcp.bin` | C6 radio firmware from the same build |
| `full_<board_id>_<chip_id>.zip` | full P4 flash of a firmware target over USB: `esptool --chip esp32p4 write-flash @flash_args` in the unpacked directory |
| `p4-flash.zip` | copy of the default target's `full_*.zip` |
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

Diagrams and screenshots are made by scripts, so they can be retaken after a UI change:

- `python3 tools/docs_diagrams.py docs/images` writes the SVG diagrams.
- `python3 tools/docs_preview.py` serves this checkout's web UI on `http://localhost:8766/` with the board answers
  recorded in `tools/docs_preview/recording.json` (`--record http://<board>` records new ones: only GETs reach the
  board, and network keys and global IPv6 addresses are scrubbed). With it running,
  `uv run --with playwright tools/docs_screenshots.py [name ...]` takes the screenshots into `docs/images/`, with the
  board shown as `esp-otbr-a1b2c3.local`. Screenshots that need state the recording doesn't have (a radio fault, a
  network trial, Home Assistant's own dialogs) are taken by hand.

`.github/workflows/docs.yml` builds the site on pull requests that touch the docs and publishes it from `master` and
after every successful `v*` build. Before publishing, `make docs-firmware` unpacks the P4 images of each target of the
latest release into `site/firmware/<board_id>/<chip_id>/`, each with a `manifest.json` of parts and offsets from its
own `flash_args`, and lists what it published with the revision ranges in `site/firmware/targets.json`
(`tools/flash_manifest.py`). A release from before targets has only `p4-flash.zip`, published as the default target.
The release's `c6_rcp.bin` and `c6_installer.bin` go to `site/firmware/c6/`, with their versions (from their app
descriptions) in `site/firmware/radio.json`; the flasher sends them through the `radio-fw` console command
([Radio firmware over the console](reference/radio-console.md)).
[Flash in the browser](flash.md) (`docs/overrides/flash.html`, `docs/assets/flasher.js`, on
[esptool-js](https://github.com/espressif/esptool-js)) reads that index, matches the chip revision against it and
writes the images itself. Repository settings: Pages, source "GitHub
Actions".
