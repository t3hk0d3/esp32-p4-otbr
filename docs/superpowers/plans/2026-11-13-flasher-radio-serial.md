# Radio firmware from the browser flasher Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** After the browser flasher has written and booted the P4, it puts the radio firmware on the C6 through the
serial port it already holds: `c6_installer.bin` first when the C6 still runs its factory ESP-Hosted firmware, then
`c6_rcp.bin`, both from the same release as the P4 images. No http call from the https docs page to the board.

**Architecture:** A new P4 console command `radio-fw` on the UART0 REPL (the CH343 port, 115200 baud) reports the
radio's state and takes one image over the serial line into a PSRAM buffer, checks it, and hands it to the existing
`builtin_rcp_push_firmware` (which already does both the installer path through ESP-Hosted OTA and the updater path).
The page talks to it with text commands, binary frames host → P4, and marked text reply lines P4 → host that it picks
out of the log output. The docs site publishes the release's two C6 images next to the P4 images.

**Tech Stack:** ESP-IDF v6.1 (`esp_console`, UART driver, `esp_rom_crc32_le`), Unity host tests on the linux target,
esptool-js 0.7.x / Web Serial, node tests through pytest, Python `tools/flash_manifest.py`, MkDocs.

**Styleguide:** `STYLEGUIDE.md` (`make lint`): astyle C, ruff Python, `snake_case`, component prefixes (adds
`radio_console_`), `esp_err_t` + `ESP_RETURN_ON_ERROR`, host tests `TEST_CASE("<scenario>: <effect>", "[radio_console]")`
in `<component>/test_host/`, routine tasks as Makefile targets, guides vs reference split. The flasher keeps the style of
`docs/assets/flasher.js` (pure helpers + `createFlow(deps)` + `view(state)`; tests in `tools/tests/test_flash_page.py`
through `flash_page_harness.js`).

## Global constraints

- Never flash or OTA a board inside the tasks. Host tests, node tests and builds only. The on-board pass is separate,
  after review, on clerk `p4-otbr-rev3/p4-otbr-b`, and only when the user approves it.
- The C6 is never reset while it may be running the installer (unchanged `push_installer` rule). The page never asks
  for a reset or power cycle in that window.
- Every installer check stays on the P4 (`sync.c push_installer`): the page's own checks only fail earlier with the same
  text, they never replace the P4's.
- UART0 keeps its role as console and log output: nothing is turned off; the protocol lives next to the logs.
- Sizes for scale: `c6_installer.bin` about 400 KB, `c6_rcp.bin` about 280 KB; at 115200 baud about 35 s and 25 s.

## Serial protocol (`radio-fw`)

Reference page: `docs/reference/radio-console.md` (new, indexed in `docs/README.md`).

**Direction and coexistence.** P4 → host is text: the existing log lines and the REPL prompt, plus reply lines that
start with `@radio-fw ` and end with `\n`, each written with one write call so another task's log cannot split it. The
host ignores every other line, including the echo of the command it typed. Host → P4 is a typed command line, then,
only after `@radio-fw ready`, binary frames. While the command reads frames it reads the UART driver directly (the REPL
task is the one running the handler, so linenoise is not reading), so binary bytes never reach the line editor.

**Commands** (one line, ended with `\r`):

- `radio-fw status` → `@radio-fw status state=<s> installed=<version|-> base=<embedded base version> jumper=<0|1>`
  where `<s>` is `probing` (boot steps have not finished the probe), `esp_hosted` (factory C6), `updater` (updater
  reachable; `installed` is its RCP or `-`), `fault` (nothing answers), `busy` (a radio maintenance is running).
- `radio-fw load <size> <crc32 hex>` → `@radio-fw ready max=1024`, or `@radio-fw error <code> <detail>` with no
  transfer: `invalid_request` (bad arguments), `image_too_large` (size 0 or above the C6 slot, 0x180000),
  `no_memory` (no PSRAM buffer), `busy`.

**Frame** (host → P4, little endian): magic `R` `F`, offset u32, length u16 (1..1024, or 0 = abort), payload,
CRC-32 (IEEE, as `esp_rom_crc32_le`) over offset, length and payload.

**Per-frame replies:** `@radio-fw ack <next offset>` or `@radio-fw nak <expected offset> <crc|offset|length|timeout>`.
Stop and wait: one frame in flight.

- The P4 accepts only `offset == expected`. A frame below that (a repeat after a lost ack) is answered `ack <expected>`
  without writing; a frame above is `nak <expected> offset`.
- Bytes before a magic are skipped. A frame that stalls for 500 ms between bytes is dropped with `nak … timeout`.
- **Resume** is within one command: the host always sends from the offset in the last `ack`/`nak`. There is no resume
  across a lost port; a new `load` starts at 0 (the old buffer was freed).
- Host side: no reply within 2 s → resend the same frame; 5 tries at one offset → give up (sends an abort frame if the
  port still works).
- P4 side: 10 s without a frame, 8 `nak`s in a row, or an abort frame → `@radio-fw error <timeout|transfer|aborted>`,
  buffer freed, back to the prompt. Nothing has touched the C6 until the whole image is in.

**After the last byte:** the P4 checks the whole-image CRC from `load` (`error invalid_request image checksum
mismatch` otherwise), replies `@radio-fw received`, and runs `builtin_rcp_push_firmware` on a re-openable memory source
in its own task. While it runs the handler prints `@radio-fw busy <seconds>` every 5 s. The end is one of:

- `@radio-fw done installed=<version|->` (`-` after the installer: the updater answers, no RCP yet; the page knows
  which file it sent);
- `@radio-fw error <code> <detail>`, `<code>` the same as `POST /device/radio/firmware` (`device_api_map_push_error`:
  `busy`, `c6_busy`, `invalid_image`, `install_failed`, `restart_failed`, `no_memory`, …) and `<detail>` the push's
  text, e.g. `wrong c6_installer.bin: base-1234abcd, need base-5678ef01`.

A lost port does not stop a push that has started: the buffer is in PSRAM and the push owns it until it ends.

## Release files on the docs site

`tools/flash_manifest.py` also copies the release's `c6_rcp.bin` and, when present, `c6_installer.bin` into
`<firmware dir>/c6/` and writes `<firmware dir>/radio.json`:
`{"version": <release>, "rcp": {"path": "c6/c6_rcp.bin", "version": <app desc version>}, "installer": {"path":
"c6/c6_installer.bin", "base_version": <app desc version>}}` (`installer` absent when the release has none; no file at
all when the release has no `c6_rcp.bin`). The Makefile's `docs-firmware` downloads both files with the zips.

## Flasher flow

After `done` the page keeps the port open (today it closes it once it has the address) and goes on to a fourth step,
**Radio**, when the site has `radio.json` (without it the flow ends at `done` as before and the fourth step is
hidden). New stages: `radio_checking`, `radio_ready`, `radio_current`, `radio_no_installer`,
`radio_wrong_installer`, `radio_fault`, `radio_sending`, `radio_installing`, `radio_done`, `radio_failed`,
`radio_port_lost`. The address search runs as now (the link appears when found); the radio step does not wait for it.

1. `radio_checking`: `radio-fw status`, repeated every 2 s while `probing`, for up to 60 s.
2. By state: `esp_hosted` → needs the installer (`radio_no_installer` if `radio.json` has none;
   `radio_wrong_installer` if the installer's app description version is not `base`); `updater` with `installed` equal
   to the release's RCP version → `radio_current` (with **Install anyway**); `updater` otherwise → RCP only; `fault` →
   `radio_fault` (points to ways B and C of first-install); `busy` → wait and ask again.
3. `radio_ready` says what will be sent and the power warning for that path, with **Install radio firmware** (a click:
   the user accepts the warning; Web Serial needs no gesture for the transfer itself).
4. `radio_sending` (file name, percent) → `radio_installing` (after `received`; elapsed time, "don't power off") →
   for the installer, on `done base_only=1`, `radio-fw status` again and straight on to the RCP without another click →
   `radio_done` (installed version).
5. Errors: `radio_failed` shows code and detail and, for `install_failed` after the installer, the existing advice
   (wait a minute without powering off, then **Check radio**, which re-runs step 1).
6. Port gone (read ends, read or write throws) → `radio_port_lost`, worded by phase: while sending, "nothing on the C6
   changed"; after `received`, "the board carries on; don't power it off". **Reconnect** picks the port again, listens
   for 10 s (a running push keeps printing `busy`, then its result), then sends `radio-fw status`.

Host timeouts: no `busy` or result for 30 s after `received` counts as the port lost; overall cap 8 min for the
installer push, 2 min for the RCP.

## Files

- Create `p4/components/radio_console/` (`CMakeLists.txt`, `include/radio_console.h`, `radio_console.c`,
  `radio_console_proto.c`, `radio_console_proto.h`, `test_host/`).
- Modify `p4/main/console.c` (register), `p4/main/CMakeLists.txt` (requires), `p4/components/device_api/`
  (new public `device_api_push_error_code`, the code of `device_api_map_push_error`), `p4/components/rcp_sync`
  (`rcp_sync_base_image_version` public), `common/board_cores` (task list), `STYLEGUIDE.md` (prefix list).
- As built (after the PR 61 review): `esp_console_new_repl_uart` already installs the UART0 driver (256-byte RX ring)
  and linenoise reads through it; the command reads the frames from that driver and flushes its input before `ready`
  and before returning to the prompt. The page sends frames of at most the `max` in `ready`, takes `received` as the
  last frame's ack, and counts a lost port from the last frame on as "may be installing" (phase `finishing`).
- Modify `docs/assets/flasher.js`, `docs/overrides/flash.html`, `docs/assets/flash.css` (fourth step only),
  `tools/tests/flash_page_harness.js`, `tools/tests/test_flash_page.py`.
- Modify `tools/flash_manifest.py`, `tools/tests/test_flash_manifest.py`, `Makefile` (`docs-firmware`).
- Docs: `docs/flash.md`, `docs/first-install.md` (way A gets "from the flasher" first, the web UI upload as the other
  way), `docs/reference/radio-console.md` (new), `docs/README.md`, `docs/development.md` (on-board checks).

## BDD skeleton

```
radio_console_proto (pure; host tests)
    radio_console_proto_parse_load(argc, argv, out)
        On "<size> <crc32 hex>"
            - returns ESP_OK with size and crc
        On a missing, non-numeric or negative argument
            - returns ESP_ERR_INVALID_ARG
        On size 0 or above RADIO_CONSOLE_MAX_IMAGE
            - returns ESP_ERR_INVALID_SIZE
    radio_console_proto_feed(parser, byte, now_ms) / frame decoding
        On one whole valid frame at the expected offset
            - reports FRAME_OK with offset, length and payload
        On a frame split across many feeds
            - reports nothing until the last byte, then FRAME_OK
        On bytes before the magic
            - skips them and still decodes the frame
        On a CRC mismatch
            - reports NAK_CRC
        On length above 1024
            - reports NAK_LENGTH without waiting for a payload
        On length 0
            - reports ABORT
        On more than 500 ms between two bytes of a frame
            - reports NAK_TIMEOUT and starts over at the magic
    radio_console_proto_accept(expected, frame)
        On offset == expected and offset + length <= size
            - returns WRITE, next = offset + length
        On offset < expected
            - returns REPEAT, next = expected (nothing written)
        On offset > expected, or past the image size
            - returns NAK_OFFSET with expected
    radio_console_proto_format_* (reply lines)
        On status
            - writes "@radio-fw status state=… installed=… base=… jumper=…\n", "-" for an empty version
        On ack / nak / ready / received / busy / done / error
            - writes one line with the documented fields and a trailing "\n"
        On a detail longer than the line buffer
            - truncates the detail and keeps the "\n"
        On a detail with a newline
            - replaces it with a space (a reply is one line)
    radio_console_proto_state(probe, builtin status, maintenance running)
        On a probe with esp_hosted
            - returns "esp_hosted"
        On a reachable updater
            - returns "updater"
        On no probe yet
            - returns "probing"
        On a maintenance running
            - returns "busy"
        On nothing answering
            - returns "fault"
    radio_console_proto_result_line(err, code, detail, installed)
        (code comes from device_api_push_error_code, whose mapping device_api's host tests cover)
        On ESP_OK after the installer
            - is "done installed=-"
        On ESP_OK after an RCP
            - is "done installed=<version>"
        On a refused installer (code invalid_image, "wrong c6_installer.bin: X, need Y")
            - is "error invalid_image wrong c6_installer.bin: X, need Y"
        On an error without detail
            - falls back to esp_err_to_name

radio_console (on the board; covered by the on-board pass, not host tests)
    radio-fw load
        On a complete image with the right CRC
            - replies received, runs builtin_rcp_push_firmware once, prints busy every 5 s, then done or error
        On 10 s without frames
            - replies "error timeout", frees the buffer, returns to the prompt, the C6 untouched
        On a whole-image CRC mismatch
            - replies "error invalid_request …", no push
        On the port lost after received
            - the push runs to its end (its result appears in the log and /device/status)

flash_manifest (pytest)
    publish
        On a release with c6_rcp.bin and c6_installer.bin
            - copies both to c6/ and writes radio.json with the RCP version and the installer base version
        On a release without c6_installer.bin
            - writes radio.json without "installer"
        On a release without c6_rcp.bin
            - writes no radio.json
        On a C6 file that is not an app image
            - raises ValueError naming it

flasher.js helpers (node)
    crc32(bytes)
        On the standard check string "123456789"
            - returns 0xCBF43926
    frame(offset, bytes)
        On a chunk
            - starts with "RF", carries offset and length little endian, ends with the CRC of the header and payload
    parseReply(line)
        On a line with "@radio-fw " after log noise
            - returns the kind and fields
        On any other line
            - returns null
    appDesc(bytes)
        On an app image
            - returns project and version
        On anything else
            - returns null

createFlow radio step (node, through the harness)
    after install
        On the board printing its address
            - keeps the port open and moves to radio_checking
        On no address within ADDRESS_WAIT_MS
            - still moves to radio_checking
    checkRadio
        On state=probing then esp_hosted
            - asks again every 2 s, then shows radio_ready for the installer with the power warning
        On state=updater and an older installed version
            - shows radio_ready for c6_rcp.bin only
        On state=updater and the release's version installed
            - shows radio_current; installAnyway goes to radio_ready
        On state=fault
            - shows radio_fault
        On no radio.json
            - ends at done, the port closed, the fourth step hidden
        On esp_hosted and a release without c6_installer.bin
            - shows radio_no_installer and sends nothing
        On esp_hosted and an installer whose version is not base
            - shows radio_wrong_installer "wrong c6_installer.bin: X, need Y" and sends nothing
        On no status reply within 60 s
            - shows radio_failed with a timeout
    installRadio (factory C6)
        On the whole path answering
            - sends "radio-fw load" for the installer, its frames in order, shows radio_sending percent, then
              radio_installing on received, then a status, then the RCP's load and frames, then radio_done with the version
        On a nak
            - resends from the offset in the nak
        On a missing ack
            - resends the same frame after 2 s, and after 5 tries shows radio_failed and sends an abort frame
        On "error invalid_image wrong c6_installer.bin: …" from the board
            - shows radio_failed with that text and does not send the RCP
        On "error install_failed …" after the installer
            - shows radio_failed with the wait-then-check advice; checkRadio is offered
        On no busy line for 30 s after received
            - shows radio_port_lost in the installing wording
    installRadio (C6 with updater)
        On done
            - sends only c6_rcp.bin and shows radio_done
        On "error c6_busy …"
            - shows radio_failed with the code
    lost serial port
        On the read ending while sending
            - shows radio_port_lost saying the C6 did not change
        On a write throwing while sending
            - shows radio_port_lost saying the C6 did not change
        On the read ending after received
            - shows radio_port_lost saying don't power off
        On reconnect with the board printing busy and then done
            - shows radio_installing, then radio_done
        On reconnect with a silent board
            - sends radio-fw status after 10 s and continues from the state
    restart
        On any radio stage
            - cancels the read, closes the port, back to idle
    view
        On radio stages
            - step3 is "on"; percent, file name, installed version, error code and detail are shown
        On radio_sending and radio_installing
            - the unload warning is armed (like erasing and writing)
```

## Tasks

- [ ] **1. Styleguide.** Add `radio_console_` to the prefix list in `STYLEGUIDE.md`.
- [ ] **2. Pure protocol module.** `radio_console_proto.{c,h}` and its Unity host tests, one per effect of the
  `radio_console_proto` tree (TDD). `make test-host`.
- [ ] **3. Console command.** `radio_console.c` (`radio_console_register`), registered in `p4/main/console.c`;
  `device_api_map_push_error` made public. Build all targets (`make build`), no flashing.
- [ ] **4. Release files.** `flash_manifest.py` + pytest per the `flash_manifest` tree; `docs-firmware` downloads the
  C6 files. `make test-tools`.
- [ ] **5. Flasher helpers.** `crc32`, `frame`, `parseReply`, `appDesc` + node tests.
- [ ] **6. Flasher flow.** Harness gains a scripted fake board (writable stream; replies to `radio-fw` lines and frames
  with options for nak, lost ack, errors, silence, lost port); flow, view and `flash.html` per the `createFlow radio
  step` tree. `make test-tools`.
- [ ] **7. Docs.** `docs/flash.md` (the radio step, the warnings, a release without the installer, a lost port),
  `docs/first-install.md` (way A from the flasher first; web UI upload second; the `invalid_image` / `install_failed`
  texts), `docs/reference/radio-console.md` (the protocol above), `docs/README.md`, the on-board check list in
  `docs/development.md`. `make docs` (strict) and `make lint`.
- [ ] **8. PR** with all of the above; then review and fixes. The on-board pass waits for the user.

## On-board check list (separate pass, user-approved)

On `p4-otbr-rev3/p4-otbr-b`: factory C6 end to end from the flasher; C6 with updater (RCP only, and "already
installed"); unplug the cable while sending (C6 untouched, `radio-fw load` times out to the prompt) and while the
installer runs (push finishes; Reconnect shows the result); the console still works after each; log output during the
transfer does not break it.

## Open decisions for the user

- **Speed.** 115200 baud throughout (about 1 min of transfer for a factory C6, plus the installer's ~4 min). A baud
  switch for the transfer (e.g. 921600: P4 changes the UART after `ready`, the page reopens the port) would cut that to
  ~10 s but adds a failure mode on the CH343; left out unless you want it.
- **One click.** The radio step needs one click (the power warning is accepted there); after that installer and RCP run
  without another. Making the RCP-only path start by itself is possible.
