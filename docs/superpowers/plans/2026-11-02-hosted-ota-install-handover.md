# Handover: first install without wires (ESP-Hosted OTA) — SUPERSEDED

Status, 2026-11-02: the bundle design below was parked, then replaced by a simpler one, now implemented on branch
`feat/hosted-ota-install`: the P4 forwards a one-time `c6_installer.bin` (bootloader, partition table and updater embedded
in the app, so the C6's own image check covers them) through ESP-Hosted's OTA; the RCP is uploaded afterwards as usual.
Plan: [2026-11-02-hosted-ota-install.md](2026-11-02-hosted-ota-install.md). Dropped from the parked design: the appended
install bundle (`common/c6_bundle`), the P4 embedding of the migrator, buffering the RCP upload and the version-checked
wait. The research facts below still hold and are what `p4/components/c6_hosted` implements.

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

- Stock bootloader (IDF 5.x) starting the IDF 6.1 installer.
- SDIO bring-up and the slave's streaming mode against our minimal host.
- An untouched factory C6 (exact slave version, secure boot / flash encryption).
- Power cut during the bootloader (~22 KB) or partition table (4 KB) write bricks the C6 without the jumper.

