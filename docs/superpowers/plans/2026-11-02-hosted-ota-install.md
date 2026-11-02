# First install without wires, simplified: ESP-Hosted slave OTA + one-time `c6_installer`

## Context

A factory Waveshare ESP32-P4-WIFI6-POE-ETH ships ESP-Hosted on its ESP32-C6. Today the C6 can only be replaced over the
maintenance jumper. The first attempt (branch `feat/hosted-ota-install`, parked at 55fd399) packed everything into one
upload: installer app + appended bundle (boot loader, partition table, updater, RCP), a bundle format, P4-side bundle
assembly, buffering of the RCP upload and a version-checked wait. Too much machinery for a one-time step.

The user's simplification:
- The P4 learns ESP-Hosted slave OTA: it forwards an uploaded image to a stock C6 unchanged.
- A **one-time firmware** `c6_installer.bin` installs only our boot loader, partition table and updater, then restarts.
- After that the C6 is exactly what ROM recovery leaves behind: the updater with a blank RCP slot. The existing
  "no rcp firmware" flow takes over; the user uploads `c6_rcp.bin` as usual.

New first install: flash the P4 over USB-C → upload `c6_installer.bin` → upload `c6_rcp.bin`. No wires.

## What changes against the parked branch

Kept (reused as is or trimmed):
- `p4/components/c6_hosted/` `hosted_frame.c/.h`, `hosted_rpc.c/.h`, `hosted_session.h`, `c6_hosted.h` (written, never
  compiled): the ESP-Hosted wire format, already researched (facts in the handover doc).
- `c6_migrator/` → renamed **`c6_installer/`**; `migrator_core` keeps decide / relocate / install, minus the RCP step.

Dropped:
- `common/c6_bundle/` (whole component and its tests). The installer **embeds** its three images with `EMBED_FILES`, so
  `esp_ota_end` on the C6 and the boot loader verify everything through the app image's own SHA-256. No appended data.
- The P4 embedding of the migrator (`p4/CMakeLists.txt`, `p4/main/CMakeLists.txt`, `c6_base_image.h`, `app_main.c`,
  `rcp_sync_base_image_t.migrator_*`, `base_image.c` check). The installer is a release file the user uploads.
- PSRAM buffering of the source, segment lists, bundle header building, RCP version check after install.
- Test-build command `c6_hosted_ota` (dropped). `c6_stock_restore` was first dropped here in favour of a host-side esptool
  step, then brought back during the hardware pass: no host serial line reaches the board's C6, so the stand-in is
  written through the P4 over the jumper. It stays (`p4/main/test_cmds.c`, used by `p4/pytest/test_hosted_install.py`).
  The stand-in build itself stays out of the repository. `c6_jumper_ignore` stays too (needed to test without removing
  wires).

## Approach

### C6: `c6_installer/` (IDF 6.1 project, built by `tools/build_all.sh` after `c6_updater`)
- `main/CMakeLists.txt` embeds `c6_updater/build/{bootloader/bootloader.bin, partition_table/partition-table.bin,
  c6_updater.bin}`. App version = the C6 base version (`tools/base_version.cmake`), so the P4 can tell a matching
  installer from one of another release.
- `sdkconfig.defaults` as today (dangerous flash writes on, TWDT off, size-optimised, single-app table).
- **Decision** (`installer_decide`, pure): running slot clear of 0x0–0xA0000 → INSTALL; running in stock `ota_0`
  (0x10000, overlaps the updater target) → RELOCATE into the other stock app slot; our partition table already at
  0x8000 → ALREADY_OURS (idle); else STUCK → select the other (ESP-Hosted) slot and restart.
- **Install order**: updater → `test` (0x20000, image-verified) → boot loader 0x0 → partition table 0x8000 → erase
  first sector of `ota_0` (0xA0000, leftover ESP-Hosted bytes) → erase otadata (0xf000, 8 KB) → restart. Each region
  skipped when it already matches, verified after writing, one retry. Power cut before the boot loader write: stock
  boot still runs the installer, which resumes. During boot loader / partition table write (~26 KB): needs the jumper
  (documented).
- After restart our boot loader finds no valid `ota_0`; the P4 holds GPIO6 low (below) so it boots the updater.

### P4: `c6_hosted` component (prefix `c6_hosted_` / `hosted_`)
- Pure, host-tested: frame, rpc (exist), `hosted_session.c` — **detect** (reset C6 with GPIO6 high, open data path,
  ≤ 3 s for PRIV with chip ID C6) and **ota(source)** (detect, drain ESPInit, OTABegin 30 s, stream the source in
  1400-byte OTAWrite chunks 5 s each, OTAEnd 30 s). The source is the existing `rcp_fw_source_t` (sequential read),
  so a body upload streams straight through, no buffering.
- Device: `hosted_link_sdio.c` — SDMMC slot 1 (CLK 18, CMD 19, D0–D3 14–17, 4-bit, 20 MHz) + `essl_sdio`
  (`espressif/esp_serial_slave_link`, pinned); release frees the pins for the SPI link. Pins in `p4/main/board.h`.

### P4: `rcp_sync`
- New link ops: `hosted_detect(bool *)`, `hosted_ota(source, detail, cap)`, `wait_updater(timeout_ms)` (SPI up and
  GPIO6 low **without** resetting the C6, HELLO every 500 ms — never reset a C6 that may be mid-install).
- Probe: updater silent, no jumper → `hosted_detect`; `esp_hosted = true`, `last_error "esp-hosted firmware"`. Nothing
  written.
- Push (`rcp_sync_push_firmware_with`): HELLO fails, no jumper, `hosted_detect` true → read the image header from the
  source (first read, project name and version from `esp_app_desc_t`):
  - project `c6_installer` and version == `link->base_version()` → `hosted_ota`, then `wait_updater(90 s)`; ESP_OK when
    HELLO answers. br_core's re-probe then lands in the existing "no rcp firmware" fault.
  - project `c6_installer`, other version → `RCP_SYNC_ERR_IMAGE_REJECTED` "installer from another release (x, need y)".
  - anything else (e.g. `c6_rcp.bin`) → `RCP_SYNC_ERR_IMAGE_REJECTED` "the C6 runs ESP-Hosted; upload c6_installer.bin
    first". Nothing sent.
- On a C6 that already runs our updater, an uploaded `c6_installer.bin` is rejected ("not an RCP image") so it never
  lands in `ota_0`.
- The jumper keeps priority: with the jumper present nothing ESP-Hosted runs.

### br_core / device_api / web
- `boot_radio`: `probe.esp_hosted` → `RADIO_FAULT "esp-hosted firmware"`, OpenThread not started.
- Push result: hosted path returns `{"installed": "c6_installer <ver>"}`; mode stays RADIO_FAULT "no rcp firmware".
- `RCP_SYNC_ERR_HOSTED_OTA` → 502 `install_failed` in `device_api_map_push_error`.
- `firmware.js` `faultHint`: "esp-hosted firmware" → "The C6 still runs its factory ESP-Hosted firmware. Upload
  c6_installer.bin first, then c6_rcp.bin."

### Release
- `tools/package_release.sh` ships `c6_installer.bin` (header comment: "one-time, POST /device/radio/firmware on a
  factory C6"); `versions.txt` line.

## Styleguide

Existing `STYLEGUIDE.md` / repo conventions: astyle C (`make lint`), prefixes `c6_hosted_`/`hosted_`, `installer_`;
`esp_err_t` + `ESP_RETURN_ON_ERROR`; C6 layout only in `common/c6_board/c6_board.h` (read-only for this work — base
version input), stock layout constants inside `c6_installer`; pins in `board.h`; host tests in `<component>/test_host/`
named `"<scenario>: <effect>"`; target tests `p4/pytest/test_<feature>.py`; conventional commits.

## BDD skeleton

```
installer_core (c6_installer, pure)
    installer_decide
        On running in stock ota_1 (0x190000), stock table
            - INSTALL
        On running in stock ota_0 (0x10000)
            - RELOCATE to stock ota_1
        On our partition table already at 0x8000
            - ALREADY_OURS
        On no clear slot to relocate into
            - STUCK
    installer_install(flash ops, embedded images)
        On a stock C6
            - writes updater 0x20000, bootloader 0x0, ptable 0x8000, erases 0xA0000 sector and otadata 0xf000, in that order
            - image-verifies the updater, read-back compares the others
        On regions already matching (resume after a power cut)
            - skips them
        On a region failing verification twice
            - returns the error, later regions untouched
    installer_relocate
        On running in stock ota_0
            - copies the own image into stock ota_1 and selects it
    app_main
        On STUCK
            - selects the ESP-Hosted slot, restarts
        On ALREADY_OURS
            - idles, writes nothing

c6_hosted (P4, pure)
    hosted_frame_encode / hosted_frame_next
        On a serial RPC payload
            - if_type 3, offset 12, len, checksum = u16 sum with the field zeroed
        On a stream holding PRIV + event + response
            - yields the three packets in order
        On a truncated or bad-offset header
            - waits for more / reports malformed
    hosted_rpc_encode_req / hosted_rpc_decode
        On OTABegin / OTAWrite(1400 B) / OTAEnd
            - msg_type 1, msg_id 272/273/274, uid, oneof field = msg id
        On Resp 528/529/530
            - resp value decoded, negative varints included
        On an event or unknown field
            - skipped
    hosted_session_detect
        On PRIV with chip ID C6
            - present = true; link released
        On silence for 3 s or another chip
            - present = false, ESP_OK; link released
    hosted_session_ota(source)
        On a slave answering everything
            - BEGIN, WRITEs carrying the source in order in ≤ 1400-byte chunks, END
        On resp != 0 at BEGIN/WRITE/END
            - stops, ESP_FAIL with the step named; no END after a failed WRITE
        On a timeout / source read error
            - error with the step named; link released on every path

rcp_sync
    rcp_sync_probe_with
        On updater silent, no jumper, ESP-Hosted present
            - esp_hosted = true, last_error "esp-hosted firmware"; no writes
        On updater silent, nothing on SDIO
            - "updater not responding" as today
        On jumper present
            - hosted_detect never called
    rcp_sync_push_firmware_with
        On ESP-Hosted and a matching c6_installer image
            - hosted_ota gets the whole source unchanged
            - wait_updater without reset; ESP_OK when HELLO answers
        On ESP-Hosted and a c6_installer of another base version
            - RCP_SYNC_ERR_IMAGE_REJECTED naming both versions; hosted_ota never called
        On ESP-Hosted and any other image
            - RCP_SYNC_ERR_IMAGE_REJECTED "upload c6_installer.bin first"; nothing sent
        On hosted_ota failing
            - RCP_SYNC_ERR_HOSTED_OTA with the step; no wait
        On no HELLO within the wait
            - ESP_ERR_TIMEOUT "the C6 did not come back after the installer"
        On our updater answering and a c6_installer image
            - RCP_SYNC_ERR_IMAGE_REJECTED "not an RCP image"; nothing written
        On jumper present
            - hosted_detect never called

br_core / device_api / web
    boot_radio
        On probe.esp_hosted
            - RADIO_FAULT "esp-hosted firmware"; OpenThread not started
    POST /device/radio/firmware
        On a stock C6 + c6_installer.bin
            - 200 installed c6_installer; status then RADIO_FAULT "no rcp firmware"
        On a stock C6 + c6_rcp.bin
            - 4xx with the "upload c6_installer.bin first" detail
        On a failed OTA
            - 502 install_failed
    firmware.js faultHint
        On "esp-hosted firmware"
            - tells to upload c6_installer.bin, then c6_rcp.bin

p4/pytest/test_hosted_install.py (target; skips without the stock stand-in)
    On a stock-restored C6, jumper ignored
        - RADIO_FAULT "esp-hosted firmware"; upload c6_rcp.bin rejected
        - upload c6_installer.bin → 200; RADIO_FAULT "no rcp firmware"
        - upload c6_rcp.bin → 200; BR_RUNNING; base_installed == base_expected
```

## Steps

1. Rebase `feat/hosted-ota-install` on master. Remove `common/c6_bundle/` and the P4 migrator embedding; rename
   `c6_migrator` → `c6_installer`; embed the three images; trim `installer_core` + tests. `make test-host`, `make build`.
   Commit.
2. `c6_hosted` pure parts: CMakeLists, `hosted_session.c` (detect, ota over `rcp_fw_source_t`), host tests for frame /
   rpc / session; register in `tools/run_host_tests.sh`. Commit.
3. `hosted_link_sdio.c`, `idf_component.yml` (pinned `esp_serial_slave_link`), `board.h` pins. Build only. Commit.
4. rcp_sync link ops + probe/push branches (scripted-link host tests first), br_core fault, device_api error map,
   `firmware.js` hint (+ `tools/tests`). Commit.
5. Test build: `c6_jumper_ignore`; `p4/pytest/test_hosted_install.py`. Release script. Commit.
6. Docs: `docs/first-install.md` (Path 1 "no wires": installer, then RCP; jumper becomes the repair path), `docs/api.md`,
   `CHANGELOG.md`, `docs/building.md`; update the handover doc to "superseded". Commit.
7. `/code-review`, fixes; then the hardware pass.

## Verification

- `logrun make test-host`, `logrun make test-tools`, `logrun make lint`, `logrun make build`, `logrun make build-test`.
- Lean hardware pass after review (one P4 flash, one stock restore, three uploads):
  1. Build the ESP-Hosted 0.0.27 stand-in with the session script; flash it as-is over the jumper with esptool from the
     host (its own `flash_args`).
  2. Console `c6_jumper_ignore 1`, reboot.
  3. `make test-target TEST="p4/pytest/test_hosted_install.py"`.
  4. `c6_jumper_ignore 0`; board in BR_RUNNING.
- Still unproven after the pass: an untouched factory C6 (exact slave version, secure boot / flash encryption) and the
  RELOCATE case (stock C6 updated before) — the latter is host-tested only.
