# Phase 0 results

Throwaway builds live outside the repo in `~/esp/phase0/` (and `phase0_rcp_spi` inside the ESP-IDF
tree). This file records what was run, what happened, and what it means for the spec.

## Board identification

- `esptool.py chip_id` / `flash_id` on `/dev/ttyACM0` (CH343 USB-serial, VID:PID 1a86:55d3):
  **ESP32-P4 revision v1.3**, dual core + LP core, 40 MHz crystal, MAC `80:f1:b2:d2:ca:fc`,
  32 MB flash (manufacturer 0xc8, device 0x4019).
- **Finding — chip revision:** ESP-IDF v6.1 targets ESP32-P4 rev ≥ v3.1 by default
  (`CONFIG_ESP32P4_REV_MIN_FULL=301`). Images built that way will not boot on this board. Every P4
  build must set `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` (gives `REV_MIN_FULL=0`). ESP-IDF's own CI
  has an `esp32p4v1` config for this. Spec impact: add to Global Constraints and
  `p4/sdkconfig.defaults`.
- Factory flash backed up before any flashing: `~/esp/phase0/backup/p4_factory_full.bin`
  (32 MiB, SHA-256 `2ddc1d799fb53d67905dccc681ce50fb24b7386c13bbf2fe8fed3c068dd6b4bc`).
  Restore with `esptool.py -p /dev/ttyACM0 write_flash 0 p4_factory_full.bin`.

## 0B — Ethernet (spec §10 item 5) — PASS (IPv4); IPv6 checked later with the BR

- ESP-IDF `examples/ethernet/basic` with `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`, MDC 31, MDIO 52,
  PHY reset 51, PHY address 1, and ESP-IDF's default P4 RMII pins (TX_EN 49, TXD0 34, TXD1 35,
  CRS_DV 28, RXD0 29, RXD1 30, REF_CLK input GPIO50) — the defaults match this board.
- Result: `Ethernet Link Up`, **100 Mbps full duplex**, DHCPv4 lease `192.168.1.117/24`
  within ~9 s of start; host ping 3/3, rtt ≈ 0.4 ms (routed from the host's 192.168.10.0/24).
- An earlier run showed 10 Mbps half duplex and link loss: caused by a faulty cable, not the board.
- Observation: ~4 s between `esp_eth_driver_install` and `Ethernet Started` (PHY reset/startup);
  harmless.
- IPv6 not exercised by this example (it creates no IPv6 link-local); covered by the BR runs in 0D,
  which need a test host on the board's layer-2 segment.

## 0D — SPI spinel, BR, sniffer cycle (hardware) — PASS so far

Setup: board C6 runs `ot_rcp` with `CONFIG_OPENTHREAD_RCP_SPI=y` (pins §2.2), installed into its
`ota_0` over the jumper by `~/esp/phase0/c6_rcp_installer` (4.5 s, MD5-verified; bootloader
"No factory image, trying OTA 0" path works). P4 runs upstream `basic_thread_border_router` built
as `~/esp/phase0/br_p4_spi` (SPI spinel, rev < 3, `AUTO_UPDATE_RCP=n`, raw link enabled). Thread
peer: ESP32-C6 devkit (`/dev/ttyACM1` UART, `/dev/ttyACM2` USB-JTAG) running ESP-IDF `ot_cli`.
Console commands on both need the `ot` prefix.

- **SPI link:** upstream default clock 2.5 MHz (`clock_speed_hz = 2500 * 1000` in the example's
  `esp_ot_config.h`), mode 0, INT on GPIO16. RCP reset over spinel succeeded and the link came up
  on 5/5 P4-only resets with zero SPI timeouts. Higher clocks not yet tried.
- **Finding — first contact after flashing the P4:** the very first boot after `idf.py flash`
  timed out twice on SPI, logged "Failed to reset co-processor!", then **asserted**
  (`ot_spinel_coprocessor_reset_failure_callback` with no handler registered) and rebooted; the
  reboot worked. Likely cause: P4 SPI pins toggle/float during flashing and boot, leaving the C6
  SPI slave mid-frame. Spec impact: the P4 must hardware-reset the C6 (GPIO54) before spinel init
  (spec §5.3 step 4 already does), and `br_core` must register the RCP failure handler so a failed
  RCP reset leads to `RADIO_FAULT`, never an assert.
- **BR:** became leader; `br state` running; BR ULA `fddb:10d7:212f::/48`, OMR
  `fddb:10d7:212f:1::/64`; NAT64 ready; `rcp version` reports the RCP's OpenThread version.
- **Peer join:** peer attached as child with an OMR address; ping peer → BR mesh-local OK
  (20–42 ms).
- **NAT64:** peer `ping 192.168.1.1` → synthesized `fddb:10d7:212f:2::c0a8:101`, 3/3, avg 30 ms.
- **LAN → Thread routing:** Docker `macvlan` network `otbr-p0` on `mgmt0` (untagged = the board's
  VLAN 1 segment; IPv6 only, Docker address range `fdaa:1::d0c:0/112`), Alpine container with
  `accept_ra=2` and `accept_ra_rt_info_max_plen=64`. After a router solicitation the container
  learned `fddb:10d7:212f:1::/64 via fe80::82f1:b2ff:fed2:cafc` from the BR's RA (RIO). Ping to
  the peer's OMR address 5/5 (33–58 ms); traceroute: board (`fdaa:1::82f1:b2ff:fed2:cafc`) → peer.
  This also verifies the BR's IPv6 link-local and RA on the Ethernet backbone.
- **Sniffer cycles — PASS 20/20** (`~/esp/phase0/sniff_ch.py 20 11 <dataset>`): each cycle
  `thread stop` → `ifconfig down` → `channel 11` → `promiscuous enable` → 8 s capture →
  `promiscuous disable` → `dataset set active <saved>` → `ifconfig up` → `thread start`.
  Channel 11 carries a neighbouring Zigbee network: 21–122 frames per 8 s window in every cycle.
  After every cycle: role router, `br state` running, active dataset byte-identical, rejoin
  ≈ 1.4 s. On the idle Thread channel 24 the same loop captured 0–1 frames per window
  (14/20 non-empty) — an artefact of an idle channel, not of the sniffer; with the peer pinging
  ff02::1 during capture it was 17/20, the empty windows being peer-side timing.
- **Finding — changing channel invalidates the dataset:** `otLinkSetChannel()` (CLI `channel`)
  with Thread disabled **invalidates the Active and Pending Operational Datasets** unless the raw
  link is enabled (then it only sets the raw-link channel). Spec impact (§5.4): the sniffer must
  set its channel through the raw-link path (`otLinkRawSetEnable` + channel) or save and restore
  the active dataset; `br_core`'s "restore channel from the active dataset" step must not rely on
  the dataset surviving a plain `otLinkSetChannel`.
- **Sniffer cycle (single, via CLI):** `thread stop` → `ifconfig down` → `promiscuous enable`
  captured 10 frames in 15 s (peer MLE broadcasts) → `promiscuous disable` → `ifconfig up` →
  `thread start` → leader again within ~1 s. The 20-cycle run was interrupted part-way; not yet
  counted. Captured frames show `RX len=070` for a 68-byte dump plus 2 bytes, i.e. the length
  includes the 2-byte FCS (FCS validity still to be checked against Wireshark).
- **SPI clock (`~/esp/phase0/spi_speed.sh`):** BR rebuilt at 10, 20 and 40 MHz; each: 5/5 clean
  link starts (zero SPI timeouts, no asserts), 20/20 routed container→peer pings.
  Avg RTT 2.5 MHz ≈ 42 ms, 10 MHz 32 ms, 20 MHz 29 ms, 40 MHz 30 ms — radio-dominated above
  10 MHz. The first boot after each app-flash was clean at all speeds.
  **Decision: 10 MHz** (4× margin below the highest tested). Spinel-over-SPI frames carry no CRC,
  so margin matters more than the last few ms.
- **Flash-erase latency (`~/esp/phase0/erase_latency.sh`, throwaway console command `erasetest`
  erasing 4 MiB at 16 MiB; container pings board and Thread peer at 20 Hz for 40 s):**

  | Run | Erase time | Worst block | Board max | Peer loss | Peer p99 / max |
  |---|---|---|---|---|---|
  | baseline, no erase | — | — | 2 ms | 0/800 | ≤ 57 ms max |
  | 64 KB blocks, no yield | 3.4 s | 130 ms | 215 ms | 8/800 | max 1294 ms |
  | 4 KB sectors, 1-tick yield | 27.7 s | 31 ms | 17 ms | 1/800 | 297 ms / 572 ms |

- **Finding — flash auto-suspend unusable:** with `CONFIG_SPI_FLASH_AUTO_SUSPEND=y` ESP-IDF v6.1
  logs "Suspend and resume may not supported for this flash model yet" for the board's GigaDevice
  flash and asserts in flash init (boot loop). Spec §5.11 mitigation 1 is not available.
- **Finding — OTA latency target not met:** even with 4 KB sector erases and yields, Thread-path
  p99 during erase is ~300 ms with occasional loss, while the Ethernet-only path stays < 20 ms.
  The Thread path suffers more because OpenThread/spinel code runs from flash and stalls while
  the RCP keeps receiving. Spec impact: (a) erase OTA slots ahead of the transfer in throttled
  4 KB steps, then program in small chunks with yields; (b) relax the latency-isolation
  acceptance for the OTA case (no loss target kept for sniffer/topology loads; OTA allowed a
  bounded degradation), or move the OpenThread task's hot path to IRAM (to investigate).
- Still pending in 0D: httpd async check; page-program (write) stall measurement.

## 0F — `esp_rcp_update` slot pinning (spec §10 item 8) — spec sequence WRONG, fixed sequence PASS

> Superseded by R14: RCP firmware no longer stored on the P4.

Throwaway `~/esp/phase0/slotpin` (P4, `espressif/esp_rcp_update==1.7.0`, dummy UART config,
`firmware_dir="/rcp"`). For each stored `storage/rcp_seq` state (absent, 0, 1, 0|verified,
1|verified) × target slot (0, 1): pin, check `get_update_seq()` == target and
`get_next_update_seq()` == 1 − target, then `deinit` + `init` (reload from NVS, same as a reboot)
and check again.

- Semantics (from source): `get_update_seq()` returns `seq` if the verified flag (bit 5) is set,
  otherwise `1 − seq`; an absent key reads as seq 0, verified.
- **Spec §5.2 sequence** ("if `get` ≠ target: `mark_image_unusable()`, then
  `mark_image_verified(true)`"): **8/10** — fails for unverified states (stored `0` → target 0,
  stored `1` → target 1), because `mark_image_unusable()` does not flip an already-unverified
  state.
- **Fixed sequence**: `mark_image_verified(true)` (normalise to verified, keeping the current slot);
  if `get` ≠ target: `mark_image_unusable()`, then `mark_image_verified(true)`. **10/10**, all
  survive the reload. Spec §5.2 and plan Task 9 updated to this sequence.

## OpenThread in IRAM / XIP from PSRAM (follow-up to the flash-erase finding) — XIP from PSRAM PASS

Investigation report (sub-agent, build-only): `~/esp/phase0/iram_investigation.md`.
- **Linker fragments to put OpenThread in IRAM: rejected.** In normal XIP-from-flash mode every
  flash erase/write suspends the scheduler on *both* cores (`spi_flash_disable_interrupts_caches_and_other_cpu()`,
  other core spins in IRAM); only IRAM ISRs run, so task-level forwarding stops wherever the code
  lives. It also does not fit: on rev < 3 the only executable internal RAM is `sram_low`
  (179,152 B, shared with .data/.bss); all of OpenThread's text (404,912 B) overflows by ~352 KB,
  even a frame-path subset plus stock IRAM options overflows by 28.9 KB. (Fragments do work on the
  prebuilt `libopenthread_br.a`.)
- **`CONFIG_SPIRAM=y` + `CONFIG_SPIRAM_XIP_FROM_PSRAM=y`:** `.text`/`.rodata` are copied to PSRAM at
  boot; on P4 flash and PSRAM use separate MSPI buses, so flash erase/write only takes a mutex and
  both cores keep scheduling (unless some flash region is still mmapped without
  `SPI_FLASH_MMAP_FLAG_BLOCKS_WRITE`).
- **Hardware (current VLAN-20 BR build + XIP, `~/esp/phase0/lat.sh` from the VLAN 20 container,
  20 Hz pings, 40 s):** boot log `esp_psram: Found 32MB PSRAM device`, 200 MHz, `.rodata xip on
  psram`, `.text xip on psram`; app 0x1865f0 bytes; BR attached normally.

  | Run (XIP from PSRAM) | Board p99 / max | Thread peer p99 / max | Loss |
  |---|---|---|---|
  | baseline, no erase | 0.3 / 2.4 ms | 50.7 / 101.7 ms | 0/800 |
  | 4 KB sectors + 1-tick yield (29.6 s) | 0.3 / 0.3 ms | 42.5 / 60.4 ms | 0/800 |
  | 64 KB blocks, no yield (1.4 s) | 0.3 / 2.1 ms | 49.3 / 58.6 ms | 0/800 |

  **Flash erases no longer affect forwarding.** Decision: every P4 build uses XIP from PSRAM; the
  strict latency-isolation criterion applies to OTA too; chunked/yielding erases are not needed
  (keep a normal priority for the writer task). Our code must not keep flash regions mmapped
  without `SPI_FLASH_MMAP_FLAG_BLOCKS_WRITE`. Cost: ~1.5 MB PSRAM, +12.5 KB internal RAM,
  ~150 ms longer boot (agent estimate).

## Item 9 — async HTTP handlers — PASS

`/slow` on the test server (port 8080) calls `httpd_req_async_handler_begin()` and streams one
chunk per second for 20 s from a worker task pinned to core 1. During the stream: 5× `/whoami`
→ 200 in ~3 ms each, upstream `/node/state` (port 80) → 200 in 3 ms; the stream finished all 20
chunks in 20.0 s.

## Phase 0 status

All hardware items done except: DHCPv6-PD with the user's router (it does not delegate — PD
verified with our own test server), a real Home Assistant instance (python-otbr-api verified instead), and OpenThread
lock hold times (needs the `diag` instrumentation; moved to implementation).

## Item 12 — HA REST compatibility (python-otbr-api 3.1.0) — PASS

Upstream example needs `CONFIG_OPENTHREAD_BR_START_WEB=y` (off by default) for the web server.
Then `GET /` → 200, `/node/state` → `"router"`, `/.well-known/thread/br-rest` → REST API 2.0.0.
`~/esp/phase0/tools/otbr_compat.py http://192.168.1.117` (host venv, routed IPv4):

| Call | Result |
|---|---|
| `get_api_version` | OK `2.0.0` |
| `get_active_dataset_tlvs` / `get_active_dataset` | OK (TLV hex and parsed JSON: channel 24, ext PAN ID, mesh-local prefix …) |
| `get_pending_dataset_tlvs` | OK (none) |
| `get_border_agent_id`, `get_extended_address` | OK |
| `get_coprocessor_version` | OK (RCP OpenThread version string) |
| ePSKc: `get/set_ephemeral_key_enabled`, `activate_ephemeral_key`, `get_ephemeral_key_status`, `deactivate_ephemeral_key` | OK (TAP issued, state started, then stopped) |
| `set_enabled(False)` / `set_enabled(True)` | OK |
| `set_channel(15, delay=20)` | OK once the BR is attached: BR and peer both moved 24 → 15. Issued 2 s after `set_enabled(True)` (BR still detached) it returns `PendingDatasetRejectedError` — expected, there is no leader to register the pending dataset with |
| `set_active_dataset_tlvs` while Thread is enabled | `ThreadNetworkActiveError` (409) — expected, same as ot-br-posix; HA only calls it with Thread disabled |
| `factory_reset` | not run (destructive) |

`DELETE /node/dataset/*` was never needed by these calls. Conclusion: the vendored server is
HA-compatible for every call the integration uses that we could exercise; a real HA instance is
still a Task 21 check.

### Task 21 re-verification against the final firmware (2026-11-01)

`tools/otbr_compat.py --url http://192.168.1.117` (python-otbr-api 3.1.0, aiohttp 3.14.5, single network mode, board in
BR_RUNNING) runs every call above plus `create_active_dataset`, `create_pending_dataset` and `factory_reset`; all PASS
(pytest `p4/pytest/test_ha_compat.py`: 2 passed). The run restores the board: Thread is disabled, the dataset is replaced by
`create_active_dataset` and written back with `set_active_dataset_tlvs`, the channel is moved by `set_channel` and back by
`create_pending_dataset`, and `factory_reset` (DELETE /node, the board restarts with an empty dataset) runs last followed by
re-provisioning the same TLVs and `set_enabled(True)`. `DELETE /node/dataset/*` is never requested by python-otbr-api.
`set_active_dataset_tlvs` while enabled answers 409 (`ThreadNetworkActiveError`) as before. Split mode: the policy
(REST writes only on thread_bb) is covered by `test_vlan.py::test_home_assistant_rest_works_on_thread_bb_and_is_refused_on_mgmt`
and `tools/otbr_compat.py --expect-writes-refused`; see the Task 21 report for what was run.
**Pending the user (item 17, real Home Assistant):** add the board in the `otbr` integration by URL, then check the HA log
for `DELETE /node/dataset/*` (instructions in the Task 21 report). Nothing was done on a real HA instance.

## Item 11 — DHCPv6 prefix delegation — PASS (with our own delegating server); LAN router does not delegate

- **LAN router:** answers a SOLICIT with IA_PD on VLAN 1 and VLAN 20 with ADVERTISE but **no IA_PD**
  (status 2 "no addresses available") — before and after the user's firewall fix. Its DHCPv6
  server is not configured to delegate prefixes (`tools/phase0/pdprobe.py` reproduces this).
- **Own server:** `tools/phase0/pdserver.py` (minimal DHCPv6 server, PD only, Preference 255; never
  answers clients that don't request IA_PD) in a container on the VLAN 20 backbone.
  - BR: SOLICIT → ADVERTISE → REQUEST → REPLY; received `fd5d:a5d0:100::/56`; OMR favoured
    `fd5d:a5d0:100:0::/64` (prf medium, in Network Data as `paros med`) over the local ULA (prf low).
  - Thread peer SLAAC'd `fd5d:a5d0:100:0:…` ; VLAN 20 container learned
    `fd5d:a5d0:100::/64 via fe80::82f1:…` from the BR's RA (RIO) and pinged the peer 5/5;
    traceroute board → peer.
  - `br pd disable` sends RELEASE; a Preference-255 server wins over the router's empty ADVERTISE.
  - **Lifetimes:** OpenThread raises T1/T2 to ≥ 300 s (server T1 30 → adjusted 300). With valid
    lifetimes below that, the prefix expires first; the client then stops, releases and
    re-solicits by itself (observed). With realistic values (T1 310, T2 400, preferred 600, valid
    720) the client extended the lease at T1 — via **REBIND**, because OpenThread's adjustment set
    T2 to 240 s (< T1); the REPLY refreshed the lease and the prefix stayed. Harmless quirk
    (REBIND is multicast to any server).
  - Server gone → after the valid lifetime the OMR falls back to the local ULA.
- Conclusion: the PD client, OMR selection, RA/RIO advertisement and routing all work. Testing
  against a real delegating router remains a Task 21 item; the test server is the fallback.

### Task 21 re-verification against the final firmware (2026-11-01)

`p4/pytest/test_pd.py -m pd_router` runs `tools/phase0/pdserver.py` in a Docker macvlan container on the untagged LAN
(host interface `mgmt0`, single mode) and removes it afterwards. With the delegating server: `/device/status` shows
`pd.state "running"` and `pd.prefix` = the OMR /64 inside the delegated /56; `ot br omrprefix` favours that prefix
(`prf:med`) over the local ULA (`prf:low`). Without a delegation (the LAN router): `pd.prefix` null and the local ULA OMR
prefix is used, but `pd.state` is `"running"` (OpenThread keeps soliciting), not `"idle"` as the BDD says.
**Resolved (rig defect, not firmware):** the unanswered echo came from the test rig. Docker gave the container a static
address `fd5a:7e57:c::2` that is off-link for the board; for a destination in `fd5d:a5d0:100::/48` the kernel's source
selection (RFC 6724 rule 8, longest common prefix) preferred it over the SLAAC address `fdaa:1::...` of the LAN, while for
the ULA OMR destination (`fd99:...`) it picked the SLAAC address. The peer did answer (the board's OpenThread log shows the
echo reply from RLOC 0x0000 arriving), but the board never put a reply for the off-link source onto the LAN
(tcpdump on mgmt0: requests, no replies). With `ping -I <SLAAC address>` the same delegated-prefix address answers 3/3, and
the ULA OMR address does not answer from the `fd5a` source. `test_pd.py` now pings from the SLAAC address and asserts the
effect (no xfail). Details: `.superpowers/sdd/2026-10-30-esp32-p4-otbr/pd-investigation-report.md`.

## 0C — C6 bootloader and flash (spec §10 items 1, 2, 3) — PASS, done entirely over the jumper

Throwaway projects: `~/esp/phase0/c6_probe` (C6: spec §4.1 partition table with explicit offsets —
`nvs` 0x9000, `otadata` 0xf000, `test` 0x20000/512K, `ota_0` 0xa0000/1.5M, `ota_1` 0x220000/1.5M;
`CONFIG_BOOTLOADER_APP_TEST=y`, pin 2, active low, `CONFIG_BOOTLOADER_HOLD_TIME_GPIO=0`) and
`~/esp/phase0/c6_installer` (P4: embeds the C6 bootloader, partition table and app; installs them
over the jumper with `esp-serial-flasher`, then drives GPIO6/GPIO54 and reads the C6 console on
GPIO4). No USB-UART adapter used.

- **Install over the jumper:** ROM at 1.5 Mbaud; bootloader, partition table, app → `test` and
  app → `ota_0` all written and MD5-verified; `otadata` and `ota_1` erased. Total 7.5 s for ~420 KB
  plus erases (ROM loader, no stub).
- **Item 1:** C6 flash is 8 MiB (see 0E).
- **Item 2 — APP_TEST on IO2: PASS.** GPIO6 high at reset → `running=ota_0`; GPIO6 held low →
  bootloader logs `Detect a boot condition of the test firmware` and runs `test`. Works with only
  the C6's internal pull-up (no external pull) and `HOLD_TIME_GPIO=0`.
  **Timing:** the bootloader samples IO2 at ~88 ms after reset release. Holding GPIO6 low for 5 ms
  or 50 ms after reset is **not** enough (normal boot). Decision: the P4 keeps GPIO6 low for the
  whole updater session (as the spec already says) and releases it only before booting the RCP.
- **Item 3 — writing an OTA slot from the `test` app: PASS.** Running from `test`, the app copied
  `ota_0` → `ota_1` with `esp_ota_begin/write/end` and `esp_ota_set_boot_partition(ota_1)`; the next
  normal boot ran `ota_1`.

## 0E — Maintenance jumper (spec §10 items 13, 14) — PASS for detection and ROM connect

Throwaway probe `~/esp/phase0/jumper_probe` (P4 app, `espressif/esp-serial-flasher` 1.11.0, UART1,
RX GPIO4, TX GPIO5, BOOT GPIO22, RESET GPIO54; GPIO6 held high; BOOT/TX high-impedance outside the
ROM session).

- **Passive detection:** while pulsing GPIO54 and listening on GPIO4 at 115200 for 1.5 s, the P4
  received 4337 bytes: the C6 ROM banner (`ESP-ROM:esp32c6-20220919`) followed by the factory
  ESP-Hosted 2nd-stage bootloader log (`ESP-IDF v5.4-dev-3602-ga97a7b0962`). Reliable.
- **ROM download mode via the jumper:** `esp_loader_connect()` with GPIO22 as BOOT and GPIO54 as
  reset succeeded on the first attempt; target = ESP32-C6, MAC `d8:85:ac:f9:cb:4c`,
  **C6 chip revision v0.2**.
- **C6 flash size: 8 MiB** (`esp_loader_flash_detect_size` = 8388608). The schematic's
  "ESP32-C6-MINI-1-N4" label is wrong; the product page (C6-MINI-1U-H8) is right. The spec's
  4 MB-safe C6 layout still applies; 8 MiB leaves room to grow the slots later.
- **Baud rate:** switching ROM and host to 921600 succeeded. Higher rates and write/verify are
  exercised in 0C, which installs the C6 test images through the jumper.
- Factory backups: not needed (factory images are downloadable from Waveshare/Espressif).

## 0A — Toolchain and upstream build (spec §10 items 6, 10) — PASS

- **ESP-IDF:** v6.1 (released 2026-08-27), installed at `~/esp/esp-idf-v6.1` for esp32p4 + esp32c6,
  Python 3.14. esp-thread-br CI builds `basic_thread_border_router` with `espressif/idf:latest`
  (= v6.1); a separate job uses release/v5.5. **Decision: pin ESP-IDF v6.1.**
- **esp-thread-br:** pinned to `1f5af46d0ccb22fe01465ba6d16a33b276c20c1b` (main, 2026-10-28). It
  contains `aa59994` "feat(br_server): align Thread REST APIs with ot-br-posix 0.6.0" (REST 2.0.0).
- **Upstream example for esp32p4** (`idf.py set-target esp32p4 && idf.py build`): builds after
  building `ot_rcp` for esp32c6 first. App 0x160950 bytes.
- **No Wi-Fi needed:** with `CONFIG_OPENTHREAD_BR_SOFTAP_SETUP` off and no `esp_wifi_remote`/
  `esp_hosted`, `esp_ot_br_server` links; `esp_wifi` contributes only empty objects.
  **Decision: patch 4 (drop Wi-Fi config source) is not needed.**
- **Packaging tool:** `managed_components/espressif__esp_rcp_update/create_ota_image.py
  --rcp-build-dir <ot_rcp build dir> --target-file <.../ot_rcp_0/rcp_image>`, run automatically by
  the `esp_rcp_update` component's CMake.
- **Finding — version file:** the `rcp_version` file that the packager puts into
  `FILETAG_RCP_VERSION` is generated by the `ot_rcp` build and contains the OpenThread version
  string (`openthread-esp32/fff9895c-b678a4f; esp32c6;  2026-10-30 22:58:50 UTC`), **not**
  `PROJECT_VER`. Spec impact: our `c6_rcp` build must write `rcp_version` itself with its
  `PROJECT_VER` (global version format) so the stored expected version and the C6 updater's
  reported `esp_app_desc_t.version` compare equal.
- **Finding — board defaults:** upstream's P4 defaults assume the Function EV board
  (`CONFIG_EXAMPLE_ETH_PHY_RST_GPIO=40`); this board needs 51.

## 0D (build-only part) — PASS

- **C6 `ot_rcp` with SPI on our pins** (MOSI 18, MISO 20, SCLK 19, CS 21, INT 22;
  `CONFIG_OPENTHREAD_RCP_SPI=y`): builds; app 0x43d30 bytes (≈272 KB, fits 1.5 MB slots). Pins are
  hard-coded in the example's `main/esp_ot_config.h`.
- **P4 BR with SPI spinel on our pins** (`CONFIG_OPENTHREAD_RADIO_SPINEL_SPI=y`, CS 15, SCLK 18,
  MISO 14, MOSI 19, INT via `CONFIG_DEFAULT_PIN_TO_RCP_BOOT=16`, reset via
  `CONFIG_DEFAULT_PIN_TO_RCP_RESET=54`, PHY reset 51): builds; app 0x167240 bytes.
  Note: upstream reuses the RCP **BOOT** pin symbol as the SPI interrupt pin.
- **Raw link alongside the BR:** OpenThread core is compiled from source in ESP-IDF (only
  `libopenthread_br.a` is prebuilt). `CONFIG_OPENTHREAD_HEADER_CUSTOM=y` with a header defining
  `OPENTHREAD_CONFIG_LINK_RAW_ENABLE 1` builds; `otLinkRawSetReceiveDone`,
  `otLinkRawSetPromiscuous` and `otBorderRoutingDhcp6PdSetEnabled` are present in
  `libopenthread.a`.
- **httpd async:** `httpd_req_async_handler_begin()` exists; `httpd_config_t.core_id` exists.
- **Affinity knobs (item 15):** `CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0`,
  `CONFIG_MDNS_TASK_AFFINITY_CPU0`, `ETH_MAC_FLAG_PIN_TO_CORE`, `httpd_config_t.core_id` all exist;
  the default event loop task is already pinned to core 0.
- **Flash auto-suspend (item 16, build side):** `SOC_SPI_MEM_SUPPORT_AUTO_SUSPEND` is 1 on esp32p4,
  so `CONFIG_SPI_FLASH_AUTO_SUSPEND` is selectable. Latency effect still to be measured.
- `CONFIG_ETH_IRAM_OPTIMIZATION` exists.
- Pending on hardware: link bring-up, speed, BR↔raw cycling, FCS validity, latency during erase.

## 0G — VLANs (spec §10 items 18–21) — PASS with driver-level tagging; lwIP-hook tagging REJECTED

- ESP-IDF v6.1 has **no** esp_eth VLAN sub-layer (it exists only on `master`). **Decision: use the
  `examples/network/vlan_support` pattern** (custom Ethernet input demux + lwIP VLAN hooks).
- The example builds for esp32p4 (app 0x60bc0 bytes).
- **On hardware — PASS.** Throwaway copy of the example (`phase0_vlan` in the IDF tree): untagged
  netif + VLAN 10 + VLAN 20, DHCPv4 on all three, IPv6 link-local created on link-up,
  `CONFIG_LWIP_IPV6_AUTOCONFIG=y`. The board's switch port already carries VLANs 10 and 20 tagged
  plus an untagged network. Console within ~12 s of boot:

  | Interface | IPv4 (DHCP) | IPv6 (SLAAC) |
  |---|---|---|
  | untagged | 192.168.1.117 | `2a06:9801:c7:aa01::/64`, `fdaa:1::/64` + link-local |
  | VLAN 10 | 192.168.10.104 | `2a06:9801:c7:aa10::/64`, `fdaa:10::/64` + link-local |
  | VLAN 20 | 192.168.2.34 | `2a06:9801:c7:aa20::/64`, `fdaa:20::/64` + link-local |

  Each interface received only its own VLAN's DHCP lease and router advertisements, so tagging on
  output and demultiplexing on input both work, for IPv4 and IPv6. All three interfaces share the
  board MAC and therefore the same EUI-64 interface ID; that is fine because they are separate
  broadcast domains.

### BR on a tagged backbone (VLAN 20) — what failed with the example's lwIP-hook approach

Throwaway `~/esp/phase0/br_p4_spi` with `main/p0_vlan.c` (untagged + VLAN 20 netifs, VLAN 20 as
`esp_openthread_set_backbone_netif`), upstream `border_router_launch.c` patched in the local
esp-thread-br clone to call `p0_backbone_netif()`.

1. **NULL client data:** the example's `lwip_vlan_set/check` dereference `netif_get_client_data()`
   unconditionally → load access fault in `ethernet_output` on a netif without VLAN data.
2. **Hard-coded client-data slot:** the example uses `LWIP_NETIF_CLIENT_DATA_INDEX_MAX + 1` instead of
   `netif_alloc_client_data_id()`; upstream BR enables `CONFIG_LWIP_PPP_SUPPORT`, which makes
   esp_netif use a client-data slot too.
3. **ABI hazard — `struct netif` layout:** raising `CONFIG_LWIP_NUM_NETIF_CLIENT_DATA` to 2 changed
   `struct netif`; the **prebuilt `libopenthread_br.a`** then read a wrong interface index (179
   instead of 4): `otPlatGetInfraIfLinkLayerAddress` → "Invalid If index". Rule: never change lwIP
   options that alter `struct netif` (client-data count etc.) — the prebuilt library is compiled
   against IDF's defaults.
4. **Fatal for the approach — `PBUF_LINK_HLEN`:** `lwip/opt.h` defines `PBUF_LINK_HLEN` as 18 only
   where `LWIP_HOOK_VLAN_SET` is defined, else 14. Header room is reserved by the *caller's*
   compile-time `PBUF_IP`/`PBUF_LINK` value, so packets built by OpenThread / the prebuilt library
   have 14 bytes of link headroom and `ethernet_output` cannot add the 802.1Q tag → "Failed to send
   ND6 message", "Failed to send RS". Control: same build with the untagged netif as backbone → no
   errors. The prebuilt library cannot be recompiled, so **VLAN tagging via lwIP hooks is unusable
   for the OpenThread backbone.**

### Driver-level tagging — PASS

`p0_vlan.c` rewritten without any lwIP hook or lwIP compile flag:
- Untagged netif: stock `esp_eth_new_netif_glue()`.
- VLAN netif: own esp_netif driver glue — TX inserts the 4-byte tag with
  `esp_eth_transmit_vargs(eth, 3, hdr, 12, tag, 4, payload, len-12)` (no copy); RX demux installed
  with `esp_eth_update_input_path_info()` **after** both attaches (the stock glue installs its own
  in `post_attach`): matching VID → `memmove` the MAC header 4 bytes forward, `esp_netif_receive`
  with `buf+4`; the VLAN glue's `driver_free_rx_buffer` frees `buf-4`; unknown VID → drop; untagged
  → untagged netif. ETH events drive the VLAN netif (`esp_netif_action_start/connected/...`).
- Results: DHCPv4 on both (192.168.1.117 untagged, 192.168.2.34 VLAN 20); BR attached (router), **no
  ND6/RS errors**; a container on VLAN 20 learned `fddb:10d7:212f:1::/64 via fe80::82f1:…` and
  pinged the Thread peer 4/4; a container on VLAN 1 received no Thread route (RA correctly
  scoped). Counters: 74 tagged TX, 0 TX errors; 1164 frames of other VLANs on the trunk dropped.
- **Item 19 — hook chaining: PASS.** The prebuilt `libopenthread_br.a` defines both
  `lwip_hook_ip6_input` and `lwip_hook_ip6_route` (strong; they replace IDF's weak defaults), so
  the spec's "switch `LWIP_HOOK_IP6_ROUTE` to a custom implementation" would collide. Chaining
  works with `-Wl,--wrap=lwip_hook_ip6_input -Wl,--wrap=lwip_hook_ip6_route` added as INTERFACE
  link options of a component (`idf_build_set_property(LINK_OPTIONS …)` after `project()` has no
  effect). Wrapper counters increment per interface and the BR keeps working.
- **IPv4 source routing:** ESP-IDF already defines `LWIP_HOOK_IP4_ROUTE_SRC` (`ip4_route_src_hook`
  in `lwip_default_hooks.c`: route via the netif owning the source address). No custom hook needed.
- **Item 21 — interface of a request: PASS.** `getsockname(httpd_req_to_sockfd(req))` returns the
  local address (IPv4 appears as `::FFFF:a.b.c.d` on the dual-stack socket); matching it against
  each netif's IPv4/IPv6 addresses identified untagged vs VLAN 20 correctly for host IPv4 to both
  addresses and container IPv6 on both VLANs.
- Upstream web UI (200) and REST (`/node/state` → "router") reachable on the VLAN 20 address.

## Maintenance jumper pins (0E step 1) — chosen, wiring done by user

From the schematic (40-pin header P6, Raspberry-Pi-style power/ground positions):

| H7 pin | Signal | P4 header pin | P4 GPIO |
|---|---|---|---|
| 1 `C6_U0TXD` | C6 TX → P4 RX | 18 | GPIO4 (`JUMPER_RX`) |
| 2 `C6_U0RXD` | P4 TX → C6 RX | 16 | GPIO5 (`JUMPER_TX`) |
| 3 GND | ground | 14 | GND |
| 4 `C6_IO9` | C6 BOOT strap | 12 | GPIO22 (`JUMPER_BOOT`) |

Header pins to leave free: 15 (GPIO6, C6 update select) and 32 (GPIO54, C6 reset) — already wired
to the C6 on the board; 3/5 (GPIO7/8, codec I2C); 8/10 (GPIO37/38, P4 console); 22/23
(GPIO1/0, 32 kHz crystal); 24 (GPIO36, strap); 27/28 (GPIO24/25, USB); 35 (GPIO53, speaker amp);
40 (GPIO45, likely SD power).

## Pending (need the board on USB)

0D hardware part, 0F slot pinning,
0G hardware part.
