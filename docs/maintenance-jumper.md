# Maintenance jumper

Three wires between the P4 header and the C6 header H7 (wiring table in [First install](first-install.md#maintenance-jumper-wiring)).
With it fitted the P4 can write the C6's bootloader, partition table and updater by itself (ROM recovery): automatically at
boot when the updater does not answer or its version differs from the one embedded in the P4 app (once per boot), or on
demand with `make recover BOARD=<board>` (`POST /device/radio/recover`, also the "Recover C6" button on the device page).

The request answers 200 once the base image was rewritten, with `rcp` telling what is on the C6 afterwards
(`no rcp firmware`, or the installed version); only a failed recovery answers 502 `recovery_failed`. Recovery restores the
C6 base image only; the RCP firmware is left alone if its slot is intact, otherwise upload it again (`make radio`).

Each flash region is written and verified up to three times; every retry reconnects to the ROM loader and drops the link
speed (1.5 Mbaud, then 460800, then 115200 baud), so a noisy wire costs time, not the recovery; the worst case is about two
minutes.

The jumper also lets the P4 log the C6 console (`c6:` lines, at most 20 per second; a C6 boot loop shows one
`c6: <n> lines suppressed` line per second instead) and serve all of it at `GET /device/radio/log`. `jumper` in
`/device/status` shows presence and base image versions. Without the jumper both endpoints answer 412 `no_jumper`.

## Base image version

The embedded base image version is content-addressed (`base-<8 hex>`, `tools/base_version.cmake`: a hash of the updater
sources, the shared protocol and board headers, the updater `sdkconfig.defaults`, the C6 partition table and the IDF
version), so an ordinary P4 update does not touch the C6. Only a P4 release whose base image really changed runs the
automatic ROM recovery once at boot. The recovery erases the C6's `otadata`; the updater then follows the C6 bootloader,
which boots the first valid slot (`ota_0`, then `ota_1`), so an RCP installed by a push into `ota_1` survives. If
`rcp.installed` is empty afterwards, no valid RCP image was left in either slot (new board, erased slots): upload
`c6_rcp.bin`.
