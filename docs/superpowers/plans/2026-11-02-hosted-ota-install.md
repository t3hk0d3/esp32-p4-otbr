# First install without wires: detect the stock ESP-Hosted C6, replace it in one upload of the radio firmware

## Context

A new Waveshare ESP32-P4-WIFI6-POE-ETH comes with ESP-Hosted on its ESP32-C6. Today our P4 firmware can replace it only
with the maintenance jumper (three wires) or a USB-UART adapter on H7 (Path 2, untested). The stock ESP-Hosted firmware
updates itself over the air from the host, over the P4↔C6 SDIO lines. The first install becomes:
1. flash the P4 over USB-C;
2. the device page reports "ESP-Hosted firmware on the C6";
3. upload `c6_rcp.bin` as usual. That single upload replaces ESP-Hosted with our boot loader, partition table, updater
   and the RCP.

Decided with the user:
- **Nothing happens automatically.** At boot the P4 only detects ESP-Hosted and reports it. The install runs only inside
  an explicit radio firmware update (`POST /device/radio/firmware`, body or URL).
- **One ESP-Hosted transfer carries everything.** It is a small C6 installer app (`c6_migrator`, the "tricky boot
  loader") with a bundle appended:
  - our boot loader;
  - our partition table;
  - the updater;
  - the uploaded RCP.

  The stock boot loader starts the installer; the installer writes our images into place.
- The jumper path is unchanged.

Facts this rests on (research of 2026-11-02, esp-hosted-mcu source, the stock P4 dump `~/esp/phase0/backup/p4_factory_full.bin`):
- **Stock pair.**
  - The stock P4 (IDF v5.4.2, built 2025-08-05) is an ESP-Hosted host over SDIO. It has the OTABegin/OTAWrite/OTAEnd RPCs,
    but neither GetCoprocessorFwVersion nor OTAActivate: esp_hosted 0.0.6–0.0.27.
  - Every slave from 0.0.6 on has OTA over SDIO.
  - The wire format is identical in 0.0.27, 1.4.7 and 2.13.1.
- **What the slave does with the image.**
  - OTABegin: `esp_ota_begin(next slot, OTA_SIZE_UNKNOWN)`, which erases the 1.5 MB slot before it answers (≤ ~15 s).
  - OTAWrite: `esp_ota_write`.
  - OTAEnd: `esp_ota_end`, which runs `esp_image_verify` over the app image only (its length comes from the image header).
    Bytes written after the image stay in the slot unchecked. That is what carries the bundle.
    `esp_ota_set_boot_partition` follows, and the slave restarts after 5 s.
  - Stock C6 layout (`slave/partitions.esp32c6.csv`, 4 MB):

    | Partition | Offset | Size |
    | --- | --- | --- |
    | nvs | 0x9000 | |
    | otadata | 0xd000 | |
    | phy | 0xf000 | |
    | ota_0 | 0x10000 | 1.5 MB |
    | ota_1 | 0x190000 | 1.5 MB |

  - A stock C6 that was never updated runs `ota_0`, so the transfer lands in `ota_1` at 0x190000.
- **Sizes.**
  - Transfer: installer (~150 KB) + boot loader (22 KB) + partition table (3 KB) + updater (228 KB) + RCP (278 KB),
    about 0.7 MB. That is well inside 1.5 MB.
  - Our target regions:

    | Region | Range |
    | --- | --- |
    | boot loader | 0x0 |
    | partition table | 0x8000 |
    | otadata | 0xf000 |
    | `test` (updater) | 0x20000–0xA0000 |
    | `ota_0` (RCP) | 0xA0000 |

    With an RCP below 960 KB, all of these lie below 0x190000, so they never overlap a transfer in stock `ota_1`.
- **Wire protocol** (small enough to implement ourselves; no esp_hosted host or esp_wifi_remote component).
  - Transport: SDIO function 1, 512-byte blocks.
  - SLCHOST registers: exactly IDF's `esp_serial_slave_link` (essl_sdio).
  - Payload header: 12 bytes, packed little-endian.
    - Fields: `if_type:4|if_num:4`, `flags`, `len u16`, `offset u16 (=12)`, `checksum u16`, `seq u16`, `throttle`, `pkt_type`.
    - Checksum: u16 sum with the field zeroed.
  - Serial interface (`if_type 3`) carries TLV `[0x01][len16]"RPCRsp"[0x02][len16]<protobuf>`.
  - Protobuf message `Rpc{msg_type=1, msg_id=2, uid=3, payload oneof field = msg id}`.
    - Requests: OTABegin 272, OTAWrite 273 (`bytes ota_data=1`), OTAEnd 274.
    - Responses: 528/529/530, each `{int32 resp=1}`.
  - Startup sequence:
    1. The slave waits for host interrupt bit 0 (open data path).
    2. It sends a PRIV packet (`if_type 5`, chip ID TLV 0x12) and an RPC event `ESPInit`.
  - Packets are at most 1536 bytes; data chunks are about 1400 bytes.
- **Pins.** esp_hosted's P4 defaults match `p4/main/board.h`:
  - SDIO slot 1: CLK 18, CMD 19, D0–D3 14/15/16/17.
  - Reset: GPIO54, pulled low to reset.
- **Where it plugs in.**
  - Boot: `rcp_sync_probe_with` (`p4/components/rcp_sync/sync.c`) ends with "updater not responding" for a stock C6;
    br_core then tries OpenThread on it anyway.
  - Upload: `rcp_sync_push_firmware_with` fails with "updater not responding".
  - Body uploads are a one-shot source read from the socket (`device_api.c` `body_source_read`, 60 s receive timeout).
- **The C6 images the P4 already embeds.** Boot loader, partition table, updater and base version
  (`p4/CMakeLists.txt`, `p4/main/c6_base_image.h`, set into rcp_sync by `rcp_sync_set_base_image`). The installer is
  embedded the same way.

Considered and not taken: keeping ESP-Hosted with the OpenThread RCP inside it (esp-hosted-mcu ≥ 2.12.7).
- Spinel only runs over a dedicated UART (`slave/main/slave_openthread.c@v2.13.1`: `#error … not yet supported`).
- On this board that UART is H7, i.e. the jumper.
- Wi-Fi/Thread coexistence on one radio, with `OPENTHREAD_RX_ON_WHEN_IDLE=n` on the host.
- Revisit if upstream ships Spinel over the ESP-Hosted transport.

## Approach

**Bundle format: new shared component `common/c6_bundle/`** (prefix `c6_bundle_`). It is pure and used by both sides.
- **Placement.** The bundle starts at the first 4 KB boundary after the installer image, padded with 0xFF.
- **Header.**
  - magic `OTBRMIG1`;
  - format version 1;
  - entry count;
  - entries `{kind (BOOTLOADER, PTABLE, UPDATER, RCP), length, sha256[32]}`;
  - a SHA-256 over the header.
- **Data.** Blobs follow back to back, in kind order.
- **Destinations** come from `common/c6_board/c6_board.h`, not from the bundle.
- **API:**
  - `c6_bundle_header_encode(entries, out)`;
  - `c6_bundle_header_decode(buf, len, out)`, which checks magic, version, header hash, sizes against the room each
    destination has (`C6_FLASH_*`), and that the RCP fits below 0x190000.
  - Host tests in `common/c6_bundle/test_host/`.

**C6: new project `c6_migrator/`** (IDF 6.1, built by `tools/build_all.sh` before p4; `CONFIG_SPI_FLASH_DANGEROUS_WRITE_ALLOWED=y`).
- No SPI and no radio; it logs on its UART (H7).
- It must not depend on its own partition table: it runs under the stock table.
- **Pure logic in `migrator_core.c/.h`**, host-tested over a fake flash ops struct:
  - the decision;
  - the step list;
  - the skip-if-equal rule;
  - the retry rule.
- **Decision**, from the running partition's offset and size and the foreign table:
  - **already ours** (the partition table at 0x8000 equals the bundle's): do nothing and idle (misuse guard).
  - **RELOCATE**: the running slot overlaps 0x0–0x190000, i.e. stock `ota_0` at 0x10000. Copy image + bundle into the
    other stock app slot with `esp_ota_*`, select it, restart.
  - **INSTALL**: the running slot is clear of all targets.
  - **STUCK**: neither applies. Log it, select the other stock slot (ESP-Hosted) again, restart.
- **Install steps, in order.** Each region is skipped when its content already matches, and verified after writing (one
  retry). Power cuts:
  - steps 2–3: the stock table and otadata still boot the installer, which carries on;
  - step 4 or 5 (boot loader ~22 KB, partition table 4 KB): unrecoverable without the jumper (documented);
  - after step 5: our boot loader boots the RCP in `ota_0`.

  1. Verify the bundle: header and SHA-256 of every blob read from flash. On failure, select stock `ota_0` (ESP-Hosted)
     again and restart: nothing was touched.
  2. RCP → `ota_0` (0xA0000), then `esp_image_verify` there. This overwrites ESP-Hosted; from here on there is no way
     back to stock.
  3. Updater → `test` (0x20000), then `esp_image_verify`.
  4. Boot loader → 0x0.
  5. Partition table → 0x8000.
  6. Erase our otadata (0xf000, 8 KB). The boot loader then picks the first valid slot, `ota_0`, or `test` while P4
     GPIO6 is low.
  7. `esp_restart`.

**P4: new component `p4/components/c6_hosted/`** (prefix `c6_hosted_`).
- **Pure, host-tested:**
  - `hosted_frame.c`: payload header encode/decode and checksum, serial TLV, splitting a streamed read buffer into packets.
  - `hosted_rpc.c`: protobuf encode of the three requests, decode of the three responses; events are skipped.
  - `hosted_session.c`: sequences over an ops struct.
    - **detect:**
      1. reset the C6 into its normal boot (GPIO6 high);
      2. bus up and open the data path;
      3. wait ≤ 3 s for PRIV with chip ID ESP32-C6;
      4. release.
    - **install(segments):** detect's first three steps, then:
      1. drain `ESPInit`;
      2. OTABegin (30 s);
      3. OTAWrite: the segments streamed back to back in 1400-byte chunks (5 s each);
      4. OTAEnd (30 s);
      5. release.

      A non-zero `resp` or a timeout is an error naming the step.
- **Device-only:** `hosted_link_sdio.c`.
  - Bus: `sdmmc_host` slot 1 on the `board.h` pins, 4-bit, 20 MHz; `sdmmc_card_init` + `essl_sdio`
    (`espressif/esp_serial_slave_link`, exact version pinned).
  - Release returns the pins to the SPI link.
- **API:**
  - `c6_hosted_detect(bool *present)`
  - `c6_hosted_install(const c6_hosted_segment_t *segs, size_t n, char *err_detail, size_t cap)`

**P4: `rcp_sync`** (`sync.c`, `link.h`, `link_spi.c`, `base_image.c`, `rcp_sync.h`; `p4/CMakeLists.txt` and
`p4/main/c6_base_image.h` embed `c6_migrator.bin`).
- **New link ops:**
  - `hosted_detect`.
  - `hosted_install`.
  - `wait_updater(timeout_ms)`.
    - Brings up the SPI link without resetting the C6, holds GPIO6 low, and sends HELLO every 500 ms.
    - The P4 never resets the C6 while the installer runs.
- **Boot probe.** When the updater is silent and no jumper is present (or the jumper is ignored in a test build), it runs
  `hosted_detect`.
  - Result field `esp_hosted`; `last_error` "esp-hosted firmware".
  - Nothing is written.
- **Push: the ESP-Hosted branch** in `rcp_sync_push_firmware_with`. It runs when HELLO fails, there is no jumper, and
  `hosted_detect` is true.
  1. Read the whole source into a PSRAM buffer (≤ `C6_OTA_SLOT_SIZE`). The body is read at once, inside its timeout.
  2. Build the bundle header (SHA-256 of each blob) and the segment list: installer, 0xFF padding, header, boot loader,
     partition table, updater, RCP buffer.
  3. `hosted_install`.
  4. `wait_updater(120 s)`. The updater must answer HELLO, and its `boot_slot_version` must equal the RCP version read
     from the uploaded image's app description.
  5. Return ESP_OK. br_core then resets the C6 into the RCP and starts OpenThread, as after any push.

  Failures:
  - install fails: `RCP_SYNC_ERR_HOSTED_INSTALL`, detail with the step. The C6 is still ESP-Hosted, or still runs the
    installer.
  - wait times out, or the version differs: `ESP_ERR_TIMEOUT` / `RCP_SYNC_ERR_IMAGE_REJECTED` with a detail.
  - source fails while buffering: its error, nothing sent.
- **The jumper keeps priority.** With the jumper present, nothing ESP-Hosted is tried; boot ROM recovery and
  `POST /device/radio/recover` are unchanged.

**br_core, device_api, web.**
- `boot_radio`: on `probe.esp_hosted`, `RADIO_FAULT "esp-hosted firmware"`; OpenThread is not started.
- `device_api_map_push_error`: `RCP_SYNC_ERR_HOSTED_INSTALL` → 502 `install_failed`.
- `firmware.js` `faultHint` for "esp-hosted firmware": "The C6 still runs its factory ESP-Hosted firmware. Upload the radio
  firmware to replace it; the first upload takes about a minute."

**Test build only.**
- Console `c6_jumper_ignore <0|1>`: NVS-persisted like `c6_hold`. The probe and push treat the jumper as absent.
- Console `c6_stock_restore <http base url>`: flashes the stand-in as-is over the jumper's ROM loader, exactly as
  `idf.py flash` / esptool would, never through OTA. Reuses `rom_flash.c`; `ROM_PLAN_MAX_REGIONS` is raised to fit.
  1. Fetch `flash_args` from the URL, then every file it lists, into PSRAM.
  2. Erase the whole C6 flash.
  3. Write each file at its `flash_args` offset, MD5-verified. That is the slave build's boot loader, partition table,
     `ota_data_initial.bin` and app in stock `ota_0`.

  The C6 then boots the stand-in from `ota_0`, like a factory board.
- Console `c6_hosted_ota <url>`: one real ESP-Hosted OTA of an arbitrary image, through `c6_hosted_install`. Used only to
  set up the relocation case: OTA the stock app onto itself, so the stand-in runs from stock `ota_1` like a C6 that was
  updated before.

**Stock stand-in images: session files only, nothing in the repo.**
- A scratch script in the job directory (`$CLAUDE_JOB_DIR/tmp/c6_stock/`) builds the esp-hosted-mcu C6 slave in the
  `espressif/idf:v5.4.2` Docker image (esp_hosted 0.0.27 commit). Its output is the build's own `flash_args` and the
  files it lists, served unchanged.
- The host serves them over HTTP during the hardware pass.
- `p4/pytest/test_hosted_install.py` takes the base URL from `--c6-stock-url` (or `C6_STOCK_URL`) and skips without it.
- The two test-build console commands are generic and stay in the repo.

**Release.** `tools/package_release.sh`: no new artifact (the installer is inside the P4 image); `versions.txt` gets a
`c6 migrator` line.

## Styleguide

`STYLEGUIDE.md`:
- astyle C;
- the prefixes `c6_bundle_`, `c6_hosted_`, `migrator_`;
- `esp_err_t` + `ESP_RETURN_ON_ERROR`;
- the C6 flash layout only in `c6_board.h`; pins in `board.h` (add SDIO D3 = 17);
- the new project's `CMakeLists.txt` in the order project.cmake → `tools/version.cmake` → `tools/test_build.cmake` →
  `project()`;
- host tests in `<component>/test_host/` named `"<scenario>: <effect>"`;
- target tests in `p4/pytest/test_<feature>.py`;
- a make target with `## ` help;
- conventional commits.

Ops structs only where host tests need them.

## BDD skeleton

```
c6_bundle (common, pure)
    c6_bundle_header_encode / decode
        On four entries round-tripped
            - decode returns the same kinds, lengths and hashes
        On a wrong magic, version or header hash
            - ESP_ERR_INVALID_CRC / ESP_ERR_NOT_SUPPORTED, nothing returned
        On a blob larger than its destination's room (bootloader > 0x8000, ptable > 0x1000, updater > test, RCP past 0x190000)
            - ESP_ERR_INVALID_SIZE

migrator_core (c6_migrator, pure)
    migrator_decide(running, foreign_table, ptable_at_0x8000, bundle)
        On running in stock ota_1 (0x190000), stock table
            - INSTALL
        On running in stock ota_0 (0x10000)
            - RELOCATE to stock ota_1
        On our partition table already at 0x8000
            - ALREADY_OURS
        On no clear slot to relocate into
            - STUCK
    migrator_install(ops, bundle)
        On a valid bundle on a stock C6
            - writes RCP 0xA0000, updater 0x20000, bootloader 0x0, partition table 0x8000, erases otadata 0xf000, in that order
            - verifies each region (image verify for RCP and updater, read-back compare for the others)
            - asks for a restart last
        On a blob whose SHA-256 does not match
            - writes nothing, selects the stock ESP-Hosted slot, asks for a restart
        On regions already holding the right content (repeat after a power cut)
            - skips them
        On a region failing verification twice
            - stops, later regions untouched, returns the error
    migrator_relocate(ops)
        On running in stock ota_0
            - copies image + bundle (length from the header) into stock ota_1, selects it, asks for a restart
    app_main
        On STUCK
            - logs, selects the stock ESP-Hosted slot, restarts
        On ALREADY_OURS
            - logs and idles, writes nothing

c6_hosted (P4, pure parts)
    hosted_frame_encode / decode
        On a serial RPC payload
            - if_type 3, offset 12, len = payload length, checksum = u16 sum with the field zeroed
        On a streamed buffer holding PRIV + event + response
            - yields the three packets in order
        On a truncated or bad-offset header
            - reports malformed, consumes nothing past it
    hosted_rpc_encode_req
        On OTABegin / OTAWrite(1400 bytes) / OTAEnd
            - Rpc msg_type 1, msg_id 272/273/274, uid increments, oneof field number = msg id
    hosted_rpc_decode_resp
        On Resp 528/529/530 with resp 0
            - ok for the matching request
        On resp != 0 (negative varint)
            - returns the slave error code
        On an Event or another msg id
            - skipped
    hosted_session_detect
        On PRIV with chip ID ESP32-C6
            - present = true; bus released
        On no PRIV within 3 s, or another chip ID
            - present = false, ESP_OK; bus released
    hosted_session_install(segments)
        On a slave answering everything
            - resets the C6 with GPIO6 high, opens the data path, drains PRIV + ESPInit
            - BEGIN, then WRITEs carrying the segments back to back in ≤ 1400-byte chunks (chunks may span segments), then END
        On BEGIN/WRITE/END answering resp != 0
            - stops there, ESP_FAIL naming the step; no END after a failed WRITE
        On a response timeout
            - ESP_ERR_TIMEOUT naming the step; bus released on every path

rcp_sync (P4)
    rcp_sync_probe_with
        On updater silent, no jumper, ESP-Hosted answering
            - esp_hosted = true, last_error "esp-hosted firmware"; nothing written
        On updater silent, no jumper, nothing on SDIO
            - esp_hosted = false, last_error "updater not responding"
        On jumper present
            - hosted_detect never called
    rcp_sync_push_firmware_with
        On a stock C6 (HELLO fails, hosted_detect true, no jumper)
            - reads the whole source into a buffer first
            - hosted_install segments: migrator, 0xFF pad to 4 KB, bundle header, bootloader, ptable, updater, RCP
            - wait_updater without a reset; ESP_OK when HELLO's boot_slot_version equals the uploaded RCP version
        On hosted_install failing
            - RCP_SYNC_ERR_HOSTED_INSTALL with the step; no wait
        On no HELLO within the wait
            - ESP_ERR_TIMEOUT, detail "the C6 did not come back after the install"
        On a boot slot version other than the uploaded one
            - RCP_SYNC_ERR_IMAGE_REJECTED, detail names both versions
        On the source failing while buffering
            - its error; hosted_install never called
        On HELLO failing and no ESP-Hosted
            - "updater not responding" as today
        On jumper present
            - hosted_detect never called

br_core / device_api / web
    boot_radio
        On probe.esp_hosted
            - RADIO_FAULT "esp-hosted firmware", OpenThread not started
    POST /device/radio/firmware
        On a stock C6
            - 200 {"installed": "<version>"}, BR_RUNNING afterwards
        On a failed install
            - 502 install_failed with the detail
    firmware.js faultHint
        On "esp-hosted firmware"
            - tells the user to upload the radio firmware to replace it

p4/pytest/test_hosted_install.py (target, deferred to the hardware pass)
    On a C6 restored to the stock stand-in (ota_0), jumper ignored, after a reboot
        - /device/status RADIO_FAULT "esp-hosted firmware"; the C6 still answers as ESP-Hosted (nothing written at boot)
    On uploading c6_rcp.bin
        - 200 with the uploaded version; BR_RUNNING; base_installed == base_expected
    On the relocation case (stock restored with ota1)
        - same outcome
```

## Steps

0. **Pin the dependencies.**
   - Registry: the `esp_serial_slave_link` version for IDF v6.1.
   - esp-hosted-mcu: the 0.0.27 commit.
1. **`common/c6_bundle`.** Host tests, then code. Commit.
2. **`c6_migrator` project.**
   - Tests: `migrator_core` host tests, then code.
   - Wiring: `tools/build_all.sh`, `make clean`, embedded into p4 (`p4/CMakeLists.txt`, `c6_base_image.h`).
   - Commit.
3. **`c6_hosted` pure parts.** Host tests, then code; register in `tools/run_host_tests.sh`. Commit.
4. **`c6_hosted` SDIO link.** `idf_component.yml`, `board.h`. Build only. Commit.
5. **rcp_sync, br_core, device_api, firmware.js wiring.**
   - Host tests: `rcp_sync/test_host` (scripted link), the device_api error map, `tools/tests` for `faultHint`.
   - Then the code. Commit.
6. **Test build commands.** `c6_jumper_ignore`, `c6_stock_restore` (as-is flash from `flash_args`), `c6_hosted_ota`. Write `p4/pytest/test_hosted_install.py`, not run.
   Commit. The stock stand-in build script stays a session file.
7. **Docs.**
   - `docs/first-install.md`: new Path 1 "no wires": flash the P4, upload the radio firmware. The jumper becomes the
     repair path, Path 2 stays for a broken C6.
   - `docs/api.md`, the spec (sections 5.3 and 5.5), `CHANGELOG.md`, `docs/building.md` (the new `c6_migrator` project).
   - Commit.
8. `/code-review` and fixes, before any hardware run.

## Verification

- `logrun make test-host`, `logrun make test-tools`, `logrun make lint`, `logrun make build`, `logrun make build-test`.
- **Lean hardware pass after review.** Budget: one USB flash of the P4 test build, two stock restores of the C6, two
  uploads.
  1. Build the stand-in with the session script; serve its output over HTTP from the host.
  2. Console: `c6_stock_restore http://<host>/`, then `c6_jumper_ignore 1`, then reboot.
  3. `make test-target TEST="p4/pytest/test_hosted_install.py --c6-stock-url http://<host>/"`.
     - This checks stock-bootloader (IDF 5.4) ↔ IDF 6.1 installer and updater compatibility, SDIO bring-up, erase timing,
       and the C6 log over the jumper.
  4. Relocation: `c6_stock_restore http://<host>/`, then `c6_hosted_ota http://<host>/network_adapter.bin` (the stand-in
     now runs from stock `ota_1`); same test.
  5. Finish: `c6_jumper_ignore 0`; the board is in `BR_RUNNING`.
- **Still unproven after this pass:** an untouched factory C6 (its exact slave version, any secure boot or flash
  encryption). `first-install.md` says so until someone runs it on a new board.
