# Radio firmware over the console

The `radio-fw` command on the P4's console (the USB-C port's CH343 bridge, UART0, 115200 baud) reports the C6's state
and takes one C6 image over that serial line. [Flash in the browser](../flash.md) uses it to put the radio firmware on
the C6 right after it flashed the P4: an https page can't call the board's http API, but it already holds the serial
port. Anything else that can write to the port can use it too.

The image goes through the same push as `POST /device/radio/firmware` ([HTTP API](../api.md)): `c6_installer.bin`
through the factory ESP-Hosted firmware's OTA, `c6_rcp.bin` to the updater, with the same checks and error codes.

## Lines and bytes on one port

The console keeps logging while the command runs. The board's replies are lines that contain `@radio-fw ` followed by a
kind and its fields, each written in one go so that no log line splits it; a client ignores every other line, the
echo of what it typed included. Replies end with `\n` (the console may add `\r`).

The host types commands as text lines ended by `\r`. After `@radio-fw ready` it sends binary frames; the command reads
them straight from the UART, so they never reach the line editor.

## Commands

### `radio-fw status`

```
@radio-fw status state=<state> installed=<version|-> base=<version|-> jumper=<0|1>
```

| `state` | Meaning |
| --- | --- |
| `probing` | the boot steps have not probed the C6 yet; ask again |
| `busy` | a radio maintenance runs (a push, a ROM recovery, the boot steps); ask again |
| `esp_hosted` | the C6 runs its factory ESP-Hosted firmware: it needs `c6_installer.bin` first |
| `updater` | the updater answers; `installed` is its RCP firmware, `-` for none |
| `fault` | nothing answers |

`base` is the version of the C6 base image built into the P4 firmware. A `c6_installer.bin` fits only when the version
in its app description is this one.

### `radio-fw load <size> <crc32>`

`<size>` in bytes (decimal, 1 to 1572864, the C6's RCP slot), `<crc32>` the CRC-32 (IEEE, as zlib's `crc32`) of the
whole image in hex. Replies `@radio-fw ready max=1024`, or `@radio-fw error <code> <detail>` without a transfer:

| `code` | When |
| --- | --- |
| `invalid_request` | the arguments are not two numbers |
| `image_too_large` | size 0 or above the slot |
| `busy` | a radio maintenance runs |
| `no_memory` | no buffer for the image (it is held in PSRAM) |

## Frames

After `ready`, the host sends the image in frames, one at a time, each answered before the next:

| Bytes | Field |
| --- | --- |
| 2 | magic `R` `F` |
| 4 | offset, little endian |
| 2 | length, little endian: 1 to 1024, or 0 to abort |
| length | payload |
| 4 | CRC-32 of offset, length and payload, little endian |

Each frame gets `@radio-fw ack <next offset>` or `@radio-fw nak <expected offset> <reason>` (`crc`, `length`,
`timeout`, `offset`).

- The board takes a frame only at the offset it expects. A frame before it (a repeat after a lost ack) is acked again
  without being written; a frame after it, or past the image, gets `nak … offset`.
- Bytes before a magic are skipped. A frame that stalls for 500 ms between two bytes is dropped (`nak … timeout`).
- The host sends from the offset of the last `ack` or `nak`. With no reply in 2 s it sends the same frame again; the
  browser flasher gives up after 5 sends of one frame and sends an abort frame.
- There is no resume across commands: a new `load` starts at offset 0.

The command ends with `@radio-fw error <code> <detail>` and back at the prompt, the C6 untouched, on: an abort frame
(`aborted`), 10 s without a frame (`timeout`), 9 naks in a row (`transfer`).

## After the last frame

The board checks the whole image against `<crc32>` (`error invalid_request image checksum mismatch` otherwise), replies
`@radio-fw received`, and only then touches the C6. While the push runs it prints `@radio-fw busy <seconds>` every 5 s;
the installer takes several minutes (the C6 is never reset while it may run the installer), an RCP image seconds. The
end is one line:

- `@radio-fw done installed=<version|->`: `-` after `c6_installer.bin` (the updater answers, no RCP firmware yet);
  the RCP's version after `c6_rcp.bin`.
- `@radio-fw error <code> <detail>`: the codes of `POST /device/radio/firmware` (`invalid_image`, `install_failed`,
  `c6_busy`, `busy`, `restart_failed`, `push_failed`, …) with the push's text, e.g.
  `invalid_image wrong c6_installer.bin: base-11111111, need base-22222222`.

A push that has started does not depend on the port: the image is in the P4's memory. If the cable goes, the push runs
to its end; reconnect and read the `busy` lines and the result, or ask `radio-fw status`.

## Order for a factory C6

1. `radio-fw status`: `esp_hosted`. Compare `base` with the version in `c6_installer.bin`'s app description.
2. `radio-fw load` of `c6_installer.bin`, frames, wait for `done installed=-`. Power must not fail meanwhile: a power
   cut while the installer writes leaves a C6 that only the [maintenance jumper](../maintenance-jumper.md) or a
   USB-UART adapter can repair ([First install](../first-install.md#2-put-the-radio-firmware-on-the-c6)).
3. `radio-fw status`: `updater`.
4. `radio-fw load` of `c6_rcp.bin`, frames, `done installed=<version>`.

A C6 with the updater needs steps 3 and 4 only.
