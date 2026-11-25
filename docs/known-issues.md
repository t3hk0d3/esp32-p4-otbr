# Known issues

Problems whose cause lies outside this project (in ESP-IDF, a chip or another vendor's firmware), what they do, and
what to do about them. Each entry says which firmware versions it affects and when it can go.

## P4 v3.1 and later: the first boot after an update panics (firmware 0.6.0 and 0.7.0)

**Who is affected:** boards with an ESP32-P4 of chip revision v3.1 and later (seen on v3.1 and v3.2) that run the
`ESP32-P4-rev3.0` firmware of releases 0.6.0 and 0.7.0. Boards with an older chip (`ESP32-P4-rev1.3` firmware) are
not affected.

**What you see:** the first boot after the firmware is written stops with a panic (`Store access fault` on the USB
console, in FreeRTOS's start), and the board restarts by itself. It happens after every write: an update from the
Firmware page or from GitHub, a USB flash, even of the same version. Later restarts work.

- **After an update** the board comes back up on the version it had before: the panic happens before the new
  firmware can confirm itself, so the bootloader rolls the update back. The update seems to finish, but the version
  does not change.
- **After a USB flash** the board works after its automatic restart.

**Cause:** a flaw of the P4 chip that Espressif confirmed
([esp-idf#19369](https://github.com/espressif/esp-idf/issues/19369)). ESP-IDF v6.1 sets the chip's data cache to
choose at random which core it serves first. On these chips, while one core waits in a tight loop, the other core's
memory access can then wait forever. On the first boot after a write the second core waits like that while the first
starts FreeRTOS, and the first core's stalled access ends in the panic.

**Also while running:** other users report that the same thing rarely crashes a running board, which then restarts.
We have not seen this on our boards.

**Fixed in:** the next release after 0.7.0. The firmware sets the data cache to serve the cores in turn, as Espressif
recommends. The fix is part of the new firmware itself, so updating an affected board to a fixed release works: the
new firmware's own first boot no longer panics.

**On an affected version:** update to a fixed release; that works from an affected version. Or flash the board over
USB ([Building and flashing](building.md), or the [browser flasher](flash.md)): it works after its automatic restart.
Trying an update to 0.6.0 or 0.7.0 again does not help: it panics and is rolled back each time, and the board keeps
working on its old version.

**When this entry can go:** when ESP-IDF sets the data cache this way itself. The firmware's patch is
`tools/cache_arbitration_patch.cmake` (see `p4/components/PATCHES.md`, "Generated ESP-IDF copies").
