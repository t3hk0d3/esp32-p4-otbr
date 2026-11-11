---
template: flash.html
title: Flash in the browser
hide: [navigation, toc]
---

## How it works

This page writes the P4 firmware of the [latest release](https://github.com/t3hk0d3/esp32-p4-otbr/releases/latest)
without installing anything. It needs Chrome, Edge or another browser with Web Serial, on a desktop.

1. Connect the board's P4 USB-C port to this computer.
2. Press **Detect board** and pick the board's serial port (a CH343, often named "USB Single Serial"). The page reads
   the chip revision and picks the firmware built for it.
3. Press **Install**. When it is done, continue with the radio firmware: [First install](first-install.md), from Path 1,
   2 or 3 after "Flash the P4".

If the page can't read the chip, put the board in download mode (hold BOOT, press and release RESET, release BOOT) and
try again, or use **Choose firmware by hand**. A wrong choice writes without an error, but then the board doesn't start;
install again with the other one to fix it.

It writes the same images as the release's `full_<board_id>_<chip_id>.zip` for your chip (bootloader, partition table,
OTA data, app, web UI) at the offsets of its `flash_args`. Tick **Erase the flash first** on a new board or one that ran
other firmware. It starts off: on a board in service leave it off, since erasing also deletes the stored Thread dataset and network
configuration. The C6 radio is not touched; upload `c6_installer.bin` (factory C6 only) and `c6_rcp.bin` from the same
release on the board's Firmware page.
