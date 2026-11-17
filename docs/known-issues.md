# Known issues

Problems whose cause lies outside this project (in ESP-IDF, a chip or another vendor's firmware), what they do, and
what to do about them. Each entry says which firmware versions it affects and when it can go.

## P4 v3.x boards: the first boot after an update panics (firmware 0.6.0 and 0.7.0)

**Who is affected:** boards with an ESP32-P4 of chip revision v3.x that run the `ESP32-P4-rev3.0` firmware of
releases 0.6.0 and 0.7.0. Boards with an older chip (`ESP32-P4-rev1.3` firmware) are not affected.

**What you see:** the first boot after the firmware is written stops with a panic (`Store access fault` on the USB
console, in FreeRTOS's start), and the board restarts by itself. It happens after every write: an update from the
Firmware page or from GitHub, a USB flash, even of the same version. Later restarts work.

- **After an update** the board comes back up on the version it had before: the panic happens before the new
  firmware can confirm itself, so the bootloader rolls the update back. The update seems to finish, but the version
  does not change.
- **After a USB flash** the board works after its automatic restart.

**Cause:** an ESP-IDF bug on P4 v3.x chips
([esp-idf#19369](https://github.com/espressif/esp-idf/issues/19369)): while the first core starts FreeRTOS, the
second core waits in a tight loop that starves the first core's memory accesses.

**Fixed in:** the next release after 0.7.0. The fix is part of the new firmware itself, so updating an affected board
to a fixed release works: the new firmware's own first boot no longer panics.

**On an affected version:** flash the board over USB ([Building and flashing](building.md), or the
[browser flasher](flash.md)), or try the update again; every attempt that panics is rolled back and leaves the board
working on its old version.

**When this entry can go:** when ESP-IDF fixes the loop. The firmware's patch is `tools/freertos_startup_patch.cmake`
(see `p4/components/PATCHES.md`, "Generated ESP-IDF copies").
