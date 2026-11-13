---
template: flash.html
title: Flash in the browser
hide: [navigation, toc]
---

## How it works

This page writes the firmware of the [latest release](https://github.com/t3hk0d3/esp32-p4-otbr/releases/latest)
without installing anything: first the P4, then the radio firmware on the C6. It needs Chrome, Edge or another browser
with Web Serial, on a desktop.

1. Connect the board's P4 USB-C port to this computer.
2. Press **Detect board** and pick the board's serial port (a CH343, often named "USB Single Serial"). The page reads
   the [chip revision](glossary.md) and picks the firmware built for it.
3. Press **Install**. The page writes the P4, waits for the board's network address and links to it.
4. The page then asks the board about its C6 radio and offers the radio firmware of the same release. Press
   **Install radio firmware**.

If the page can't read the chip, put the board in download mode (hold BOOT, press and release RESET, release BOOT) and
try again, or use **Choose firmware by hand**. A wrong choice writes without an error, but then the board doesn't start;
install again with the other one to fix it.

It writes the same images as the release's `full_<board_id>_<chip_id>.zip` for your chip (bootloader, partition table,
OTA data, app, web UI) at the offsets of its `flash_args`. Tick **Erase the flash first** on a new board or one that ran
other firmware. It starts off: on a board in service leave it off, since erasing also deletes the stored Thread dataset and network
configuration.

## The radio firmware

The page sends the C6's files to the board over the same cable; the board installs them on the C6, the same way as an
upload on its Firmware page.

- **New board** (the C6 still runs its factory firmware): the page sends `c6_installer.bin`, waits until the C6 has
  set itself up (about 5 minutes), then sends `c6_rcp.bin`. **Don't unplug or power off the board meanwhile**: a power
  cut while the installer writes leaves a C6 that only the [maintenance jumper](maintenance-jumper.md) or a USB-UART
  adapter can repair (ways B and C of [First install](first-install.md#2-put-the-radio-firmware-on-the-c6)).
- **Board that had this firmware before**: the page sends `c6_rcp.bin` only, in about a minute. If the power fails, the
  C6 keeps its previous radio firmware; install again. When the C6 already runs this release's radio firmware, the page
  says so and offers **Install it anyway**.

When it can't:

- **This release has no installer for the C6**: a new board's C6 needs `c6_installer.bin`, and this release has none.
  Use way B or C of [First install](first-install.md#2-put-the-radio-firmware-on-the-c6).
- **The installer doesn't fit this firmware** (`wrong c6_installer.bin: <version>, need <version>`): the installer and
  the board's firmware come from different releases. Nothing was sent; reload the page and install again.
- **The C6 doesn't answer**: neither the factory firmware nor the updater answers. Use way B or C.
- **The radio firmware didn't install**, with the board's error code. `install_failed` after the installer means the
  C6 may still run its factory firmware or may already have taken the installer: wait a minute without powering off,
  then press **Check the radio**. The codes are those of the Firmware page (see [First install](first-install.md#a-no-wires)).
- **The serial port is gone** (cable pulled, computer asleep): while the page was sending, the C6 was not changed; the
  board drops a file it didn't get whole. Once the page has sent the last part of the file, the board may already be
  installing, and it carries on by itself as long as it has power. Plug the cable back in and press **Reconnect**; the
  page then shows how the install ended. If the board runs on this cable alone (no PoE), pulling it is a power cut:
  keep the cable in while the radio firmware installs.

How the page talks to the board: [Radio firmware over the console](reference/radio-console.md).
