# Handover: first install without wires (ESP-Hosted OTA) — PARKED

Status, 2026-11-02: **parked by decision of the project owner.** The jumper wiring for the first install is a one-time step,
and this feature's complexity does not pay for removing it. Nothing here is merged; branch `feat/hosted-ota-install`
(worktree `.claude/worktrees/feat-hosted-ota-install`) keeps the work.

Plan: [2026-11-02-hosted-ota-install.md](2026-11-02-hosted-ota-install.md) (approved, BDD skeleton included).

## Idea in one paragraph

A factory board's C6 runs Espressif ESP-Hosted, which updates itself over SDIO from the P4 (RPCs OTABegin / OTAWrite /
OTAEnd). On the first radio firmware upload, the P4 would send one transfer through that OTA: a small C6 app
(`c6_migrator`) with an install bundle appended (our bootloader, partition table, updater, the uploaded RCP). The stock
bootloader starts the migrator, which writes everything into our layout and restarts into the RCP. At boot the P4 would
only detect ESP-Hosted and report `RADIO_FAULT "esp-hosted firmware"`; nothing is written without an explicit upload.

## Done (host-tested where pure)

| Step | What | State |
| --- | --- | --- |
| 1 | `common/c6_bundle/`: bundle header (magic `OTBRMIG1`, 4 entries kind/length/SHA-256, header SHA-256), target offsets and room checks, PSA SHA-256 helpers; stock ESP-Hosted layout constants | done, 11 host tests green, committed |
| 2 | `c6_migrator/` project: `migrator_core.c/.h` (decide INSTALL / RELOCATE / ALREADY_OURS / STUCK, blob check, ordered idempotent install RCP → updater → bootloader → partition table → erase otadata, relocate), `migrator_main.c` (device wiring: running partition, own image length via `esp_image_verify`, fallback to the stock slot), `sdkconfig.defaults` (dangerous flash writes allowed, TWDT off) | done, 17 host tests green; app builds (141 KB) |
| 2 | Build wiring: `tools/build_all.sh` and `make clean` include `c6_migrator`; `p4/CMakeLists.txt` + `p4/main/CMakeLists.txt` embed `c6_migrator.bin`; `rcp_sync_base_image_t` gained optional `migrator_start/end` (`p4/main/app_main.c` passes it) | done; `make build` passes |
| 3 | `p4/components/c6_hosted/`: public API `c6_hosted.h` (`c6_hosted_detect`, `c6_hosted_install` over segments); `hosted_frame.c/.h` (12-byte payload header + checksum, stream framing, serial TLV `RPCRsp`/`RPCEvt`, PRIV init chip-ID TLV); `hosted_rpc.c/.h` (hand-written protobuf for requests 272/273/274, response/event decode); `hosted_session.h` (link ops + timeouts) | code written, **not compiled, no tests, no CMakeLists, no `hosted_session.c`** |

## Not started

- `hosted_session.c` (detect/install sequences), `hosted_link_sdio.c` (SDMMC slot 1 on P4 pins CLK 18, CMD 19, D0–D3
  14–17 + `espressif/esp_serial_slave_link` 1.1.2), component `CMakeLists.txt` / `idf_component.yml`, host tests.
- `rcp_sync` wiring (link ops `hosted_detect` / `hosted_install` / `wait_updater`, probe field `esp_hosted`, push branch that
  buffers the source, builds the bundle and waits for the updater without resetting the C6), `br_core` fault
  `"esp-hosted firmware"`, `device_api` 502 `install_failed`, `firmware.js` hint.
- Test-build commands `c6_jumper_ignore`, `c6_stock_restore` (as-is flash of a stock build's `flash_args` over the jumper),
  `c6_hosted_ota`; `p4/pytest/test_hosted_install.py`; docs; review; hardware pass.

## Facts worth keeping (research 2026-11-02)

- Stock P4 dump `~/esp/phase0/backup/p4_factory_full.bin`: IDF v5.4.2, built 2025-08-05, ESP-Hosted host over SDIO with
  OTABegin/Write/End but no GetCoprocessorFwVersion → esp_hosted **0.0.6–0.0.27**. No C6 image stored on the P4.
- Wire format identical in 0.0.27, 1.4.7, 2.13.1: header `if_type:4|if_num:4, flags, len, offset(12), checksum (u16 byte sum,
  field zeroed), seq, throttle, pkt_type`; SERIAL if 3, PRIV if 5; serial TLV `01 len16 "RPCRsp" 02 len16 <proto>`;
  `Rpc{msg_type=1, msg_id=2, uid=3, oneof=msg id}`, responses = request + 256, empty `{int32 resp=1}` means 0.
  Startup: host sets host-interrupt bit 0 (open data path), slave sends PRIV event 0x33/0x22 with chip-ID TLV 0x12 (C6 =
  0x0D), then RPC event ESPInit (769). SDIO: function 1, 512-byte blocks, CMD53 address `0x1F800 - len`, slave RX
  buffers 1536 bytes — matches IDF `essl_sdio`.
- Slave OTA: `esp_ota_begin(next, OTA_SIZE_UNKNOWN)` erases the whole 1.5 MB slot (~15 s) before answering; `esp_ota_end`
  verifies only the app image, bytes after it are kept unchecked (this carries the bundle); restart 5 s after OTAEnd.
  Stock C6 table: nvs 0x9000, otadata 0xd000, phy 0xf000, ota_0 0x10000, ota_1 0x190000 (1.5 MB each).
- Do not touch `common/c6_board/` for this: it is an input of the updater's content-addressed base version, and a change
  there triggers a ROM recovery on every jumper board.
- Linux-target host tests that use mbedtls/PSA need `-u esp_mbedtls_mem_calloc -u esp_mbedtls_mem_free` (link order).
- Upstream esp-hosted-mcu `main` (unreleased, after 3.0.10) carries Spinel over the ESP-Hosted transport
  (`ESP_AUX_IF`, `coprocessor/features/eh_cp_feat_openthread`). It needs an ESP-IDF with `CONFIG_OPENTHREAD_RCP_CUSTOM`
  (C6) and `CONFIG_OPENTHREAD_RADIO_SPINEL_CUSTOM` (P4); IDF v6.1 has neither. Worth a fresh look once both ship: it would
  keep ESP-Hosted (and Wi-Fi) on the C6 instead of replacing it.

## Risks still unproven (would need the hardware pass)

- Stock bootloader (IDF 5.x) starting the IDF 6.1 migrator.
- SDIO bring-up and the slave's streaming mode against our minimal host.
- An untouched factory C6 (exact slave version, secure boot / flash encryption).
- Power cut during the bootloader (~22 KB) or partition table (4 KB) write bricks the C6 without the jumper.

## Resuming

Rebase the branch on master, then continue with plan step 3 (`hosted_session.c` + host tests). `make test-host` and
`make build` are green at the parking commit.
