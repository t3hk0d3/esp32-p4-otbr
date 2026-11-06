# Radio Channel Mux Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every radio (the on-board C6 over SPI, a USB dongle over CDC) becomes a channel with a stored mode (`off`,
`otbr`, `rcp-proxy`, plus the reserved `ziggurat`), so the board's border router can run on either radio and any radio
can be proxied over TCP or UDP, listening or connecting.

**Architecture:** A new component `radio_mux` owns the channel registry, the transports (USB, C6), the framing (HDLC)
and a per-channel frame pool, firmware detection at plug-in, and a mode-consumer registration that OTBR and the proxy
use (and the Zigbee work will). A new component `rcp_proxy` replaces the TCP half of `usb_serial` with listen/connect
over TCP/UDP adapters. OpenThread reaches any channel through a project `SpinelInterface` compiled into a generated
shadow copy of IDF's `openthread` component; `br_core` starts, stops and moves OpenThread between channels and gains
the board mode `NO_RADIO`. The device API serves `/device/channels`, and the web UI gets the Radios page.

**Tech Stack:** ESP-IDF 6.1, OpenThread (IDF's component, spinel host), `espressif/usb` 1.6.0,
`espressif/usb_host_cdc_acm` 2.4.1, cp210x / ch34x VCP drivers, lwIP sockets, NVS, cJSON, Unity host tests on the
`linux` target, pytest-embedded, node for the page tests.

**Spec:** `docs/superpowers/specs/2026-11-06-radio-channel-mux-design.md`, with the page layout in
`docs/superpowers/specs/2026-11-06-radio-channel-mux-mockup.html`.

## Styleguide

`STYLEGUIDE.md`, enforced by `make lint`:
- astyle for C, ruff for Python, `snake_case`.
- Public symbols prefixed with the component name: `radio_mux_`, `rcp_proxy_`, `br_core_`, `device_api_`, `netcfg_`.
  Add `radio_mux_` and `rcp_proxy_` to the styleguide's prefix list in Task 1 and Task 5.
- One `static const char *TAG` per file, named after the component.
- `esp_err_t` returns with `ESP_RETURN_ON_ERROR` / `ESP_GOTO_ON_ERROR`; `ESP_ERROR_CHECK` only in `app_main` init.
- Pure decision logic in its own files with no ESP-IDF calls, built alone on the `linux` target (like
  `usb_serial/core.c`, `br_core/mode_fsm.c`).
- Host tests in `<component>/test_host/`, named `TEST_CASE("<scenario>: <effect>", "[<component>]")`. Target tests in
  `p4/pytest/test_<feature>.py`. Page tests in `tools/tests/test_<page>_page.py`.
- Test seams (ops structs) only where a host test needs them.
- Fixed hardware facts are header constants, not Kconfig. Every routine command goes through a `make` target.
- Vendored `esp_ot_br_server` changes go into `p4/components/PATCHES.md` (next entry: 26). IDF changes (the generated
  radio spinel file) are listed in `PATCHES.md` too, under a new heading "Generated ESP-IDF copies".
- Commands are run as `set -o pipefail; <command> 2>&1 | tee "$LOG_DIR/<name>.log"`, where `$LOG_DIR` is your scratch
  directory; read the log instead of rerunning.
- No board access inside tasks: host tests and builds only. The on-board checks are written in their tasks and run in
  the hardware pass (Task 11), after the final review and its fixes.

## Global Constraints

- **Channel ids:** `c6`; `usb-<vid>-<pid>` with lowercase 4-digit hex, plus `-<serial>` when the device reports a
  serial string. Serial characters outside `[A-Za-z0-9]` are dropped; the id is at most 47 characters.
- **Modes (wire names):** `unavailable` (reported only), `off`, `otbr`, `ziggurat`, `rcp-proxy`.
- **Firmware types:** `spinel`, `ziggurat`, `znp`, `ezsp`, `unknown`. What fits: `otbr` needs `spinel`; a registered
  consumer declares its own need (`ziggurat` declares `spinel`); `rcp-proxy` and `off` fit anything.
- **Defaults:** `c6` stored mode `otbr`; a new USB channel `off`; baud 460800; flow control off; `rcp_proxy`:
  direction `listen`, adapter `tcp`, port 6638, host empty, framing `hdlc`.
- **Accepted bauds:** 38400, 57600, 115200, 230400, 460800, 921600, 1000000, 2000000. Proxy ports 1–65535 except 80
  and 179.
- **Mode rules:** OTBR declares a limit of one channel; a mode at its limit refuses another channel with 409
  `mode_taken`, except `otbr`, which moves (below). OTBR plus a registered `ziggurat` consumer at once is refused with
  409 `hybrid_not_supported`. An unregistered mode is refused with 409 `not_available`. `unavailable` set by a PUT is
  400; any mode PUT on an unavailable channel is 409 `unavailable`; a mode the detected firmware doesn't fit is 409
  `incompatible`.
- **Moving OTBR:** probe the target first (refuse 409 `probe_failed`, nothing changed); stop OpenThread on the old
  channel; start it on the new one; store both modes only after the new one runs; on a failed start, start again on
  the old channel and report the failure; only if that fails too, `RADIO_FAULT`. Runs as a long operation (PUT 202).
- **Detection:** on every USB plug-in (and for a device attached at boot): mode `off`; probe spinel at 460800, then
  115200 (`PROP_NCP_VERSION`, then `SPINEL_PROP_VENDOR_ZIGGURAT`), ZNP `SYS_PING` + `SYS_VERSION` at 115200, EZSP ASH
  `RST` at 115200, then 460800; first answer wins; the whole detection takes at most **3000 ms**; then close and reopen
  the device; then the stored mode if it fits, else stay `off` with a reason. No bootloader probes. No detection
  endpoint.
- **Events:** `RADIO_MUX_EV_RX_FRAME`, `RADIO_MUX_EV_RESET`, `RADIO_MUX_EV_GONE`, posted into the consumer's FreeRTOS
  queue (or, for OTBR, signalled through an eventfd). Never a call into a consumer from a transport task or an ISR. No
  TX completion event. TX never keeps the caller's pointer; TX after GONE fails at once with `ESP_ERR_INVALID_STATE`.
- **Frame pool:** per channel, fixed slots, DMA-capable (`MALLOC_CAP_DMA`) and aligned to the cache line, each holding
  a full SPI transaction (5-byte header + 2048-byte frame). A full pool drops the frame and counts it.
- **Unplug of the OTBR channel:** board mode `NO_RADIO`, OpenThread torn down from br_core's task before RadioSpinel's
  recovery reaches `DieNow`, stored mode stays `otbr`, Thread starts again on return. The mux never reboots on GONE.
- **C6:** the mux owns SPI2, the INT pin and the reset GPIO (`p4/main/board.h` pins, `P4_RCP_SPI_HZ`); rcp_sync keeps
  SPI3 and the jumper UART. `off` holds the C6 in reset. A C6 job (push, recovery) is GONE then re-attach, refused
  (409 `c6_busy`) only while `rcp-proxy` has a peer.
- **Dongle reset:** close and reopen over USB. Never a DTR/RTS pulse, never VBUS. DTR and RTS are asserted together
  once on open, as today.
- **Proxy:** TCP listen: first client wins, `SO_LINGER {1,0}` refusals, keepalive 10/2/3, `TCP_NODELAY`. TCP connect:
  back-off 1, 2, 4, 8, 16, 30, 30 … s, reset to 1 s after a connection that lasted 60 s. UDP: framing `hdlc` (one
  frame per datagram) or `raw` (chunks of at most 1024 bytes); listen locks to the first source until 30 s of silence
  or a kick; connect accepts only the configured peer. netcfg access rules apply to listen sockets and UDP sources.
- **Required radio caps for OTBR:** ack-timeout, tx-retries, CSMA backoff, tx-security, tx-timing (the ot-br-posix set
  for Thread 1.2+). The C6 keeps passing; the CC2652P now passes.
- **Board mode:** `BR_MODE_NO_RADIO` added; `/device/status` `mode` value `NO_RADIO`; the Thread REST API and the
  console WebSocket answer 503 in it, detail `no radio in OTBR mode`.
- **API:** `GET /device/channels`, `PUT /device/channels/{id}`, `DELETE /device/channels/{id}`; field names exactly as
  the spec's API section. `/device/usb_serial` is removed. `/device/status` gains `otbr_channel`.
- **Budgets:** `RCP_PROXY_SOCKETS` 6 (replaces `USB_SERIAL_SOCKETS` 3 in `routing/task.c` `SOCKETS_NEEDED`);
  `CONFIG_LWIP_MAX_SOCKETS` 21 → 24; `CONFIG_LWIP_MAX_ACTIVE_TCP` 20 → 22; br_core's `EVENTFD_COUNT` + 2 (one per
  channel); web server `EXTRA_HANDLER_SLOTS` 29 → 30 (two usb_serial handlers out, three channel handlers in).
- **Tasks and cores:** `radio_mux` (owner) and `usb_lib`/`USB-CDC` on `MGMT_CORE`, priorities as usb_serial's today
  (6, 7, 7); the C6 transport task on `NET_CORE` at the OpenThread task's priority + 1; `rcp_proxy` task on
  `MGMT_CORE` priority 6. Update `common/board_cores/include/board_cores.h` and `p4/pytest/core_map.py`.
- **Zigbee:** no Zigbee code. `ziggurat` exists only as a reserved mode name and through the registration interface.

## Review Focus

1. Unplugging the OTBR dongle in the middle of a synchronous spinel request (a REST call holding the OpenThread lock
   in `WaitForFrame`): no abort, `NO_RADIO` within a few seconds, the HTTP request answers (Task 7 interface rule
   `WaitForFrame` returns within 100 ms after GONE; Task 11 check `unplug_during_rest_load`).
2. A quick replug (under a second) while the mux is still closing the old handle: one close, then a normal detection
   and attach (Task 4 host test on the attach state machine: GONE, present, attach again; Task 11 check).
3. Moving Thread to a dongle whose start fails after the probe passed: Thread comes back on the C6 with the same
   network (Task 7 `mode_fsm` host tests on the move with a failing start; Task 11 check with a dongle at the wrong
   baud forced through the test build).
4. `rcp-proxy` in `connect` mode to an address that refuses or never answers: back-off, no socket leak over hours
   (Task 5 host test on the schedule; Task 11 check of `/device/diag` socket count after 10 minutes of refusals).
5. A slow consumer (the OpenThread task blocked behind the REST lock) while the dongle streams frames: drops are
   counted, the USB callback never blocks (Task 2 pool test; Task 4 host test that `RX_FRAME` posting is non-blocking).

## File map

- **Create `p4/components/radio_mux/`:**
  - `CMakeLists.txt` (the linux branch builds the pure files only), `idf_component.yml` (the USB components, moved from
    `usb_serial`).
  - Pure: `include/radio_mux_core.h` + `core.c` (ids, modes, fit, registrations, rule checks, plug-in decisions,
    driver choice, baud verdict, CP210x flow payload); `include/radio_mux_hdlc.h` + `hdlc.c`;
    `include/radio_mux_pool.h` + `pool.c`; `include/radio_mux_detect.h` + `detect.c`.
  - Board: `include/radio_mux.h` (public API), `radio_mux_priv.h`, `mux.c` (registry, owner task, NVS, attach,
    events, counters, plug-in flow), `usb_transport.c` (from `usb_serial/usb_host_glue.c`), `c6_transport.c`.
  - `test_host/` (Unity project like `usb_serial/test_host`).
- **Create `p4/components/rcp_proxy/`:** `CMakeLists.txt`; `include/rcp_proxy_core.h` + `core.c` (settings check,
  back-off, UDP peer lock, accept rule, byte ring moved from usb_serial); `include/rcp_proxy.h`; `rcp_proxy_priv.h`
  (adapter interface); `proxy.c` (consumer, task); `adapter_tcp.c` (from `usb_serial/tcp.c`); `adapter_udp.c`;
  `test_host/`.
- **Delete `p4/components/usb_serial/`** (its pieces move as above), `p4/components/device_api/usb_serial_json.c/.h`,
  `test_host/main/test_usb_serial_json.c`, `p4/pytest/test_usb_serial.py`, `docs/usb-serial.md`.
- **Create** `tools/ot_radio_port.cmake`, `p4/ot_port/esp_openthread_mux.h`, `p4/ot_port/mux_spinel_interface.cpp`.
- **Modify** `p4/CMakeLists.txt` (include `tools/ot_radio_port.cmake` before `project()`).
- **Modify `p4/components/br_core/`:** `br_core.c`, `include/br_core.h`, `include/br_core_mode.h`, `mode_fsm.c`,
  `mode_fsm.h`, `CMakeLists.txt`, `test_host/main/test_mode_fsm.c`, `br_core_test.c`.
- **Modify** `p4/components/netcfg/` (`NETCFG_SVC_USB_SERIAL` → `NETCFG_SVC_RCP_PROXY`; `netcfg_peer_allowed`),
  `test_host/main/test_policy.c`.
- **Create** `p4/components/device_api/channels_json.c/.h`, `test_host/main/test_channels_json.c`. **Modify**
  `device_api.c`, `status_json.c`, `device_api_priv.h`, `long_op.c`, `CMakeLists.txt`,
  `test_host/main/{CMakeLists.txt,test_status.c,test_long_op.c}`.
- **Modify** `p4/components/esp_ot_br_server/src/esp_br_guard.c`, `src/esp_br_web.c`, the guard's host test,
  `p4/components/PATCHES.md`.
- **Create** `frontend/radios.html`, `frontend/static/radios.js`, `tools/tests/test_radios_page.py`. **Modify**
  `frontend/static/api.js` (nav), `status.js`, `mgmt.js`, `capture.js`, `device_common.js`, `style.css`.
- **Modify** `p4/main/app_main.c`, `p4/main/CMakeLists.txt`, `p4/components/routing/task.c`,
  `p4/components/routing/CMakeLists.txt`, `p4/sdkconfig.defaults`, `common/board_cores/include/board_cores.h`,
  `p4/pytest/core_map.py`, `p4/pytest/pytest.ini` (marker `usb_dongle`), `STYLEGUIDE.md`.
- **Create** `p4/pytest/test_channels.py`, `p4/pytest/test_dongle.py`, `docs/radios.md`. **Modify** `docs/api.md`,
  `docs/README.md`, `mkdocs.yml`, `CHANGELOG.md`.

## BDD skeleton

```
radio_mux_core (pure)
    radio_mux_channel_id
        On a USB device with a serial string
            - returns "usb-<vid>-<pid>-<serial>" in lowercase hex, serial reduced to [A-Za-z0-9]
        On a USB device without a serial string
            - returns "usb-<vid>-<pid>"
        On a serial that would exceed 47 characters
            - truncates the serial so the id is 47 characters
    radio_mux_mode_parse / radio_mux_mode_name
        On each wire name
            - round-trips unavailable, off, otbr, ziggurat, rcp-proxy
        On an unknown name
            - returns RADIO_MUX_MODE_INVALID
    radio_mux_fits
        On otbr with spinel
            - returns true
        On otbr with znp, ezsp, ziggurat or unknown
            - returns false
        On rcp-proxy or off with any type, unknown included
            - returns true
        On a registered mode
            - uses the type its registration declares
    radio_mux_check_set_mode
        On unavailable as the requested mode
            - returns RADIO_MUX_REFUSE_BAD_VALUE
        On any mode for an unavailable channel
            - returns RADIO_MUX_REFUSE_UNAVAILABLE
        On a mode with no registered consumer (ziggurat in this build)
            - returns RADIO_MUX_REFUSE_NOT_AVAILABLE
        On a mode the detected firmware doesn't fit
            - returns RADIO_MUX_REFUSE_INCOMPATIBLE
        On otbr while another channel is otbr
            - returns RADIO_MUX_MOVE with the other channel as the source
        On a registered limit-1 mode already used by another channel
            - returns RADIO_MUX_REFUSE_MODE_TAKEN
        On otbr while a ziggurat consumer is active, or ziggurat while otbr is active
            - returns RADIO_MUX_REFUSE_HYBRID
        On a mode whose registration declares reboot on enter or leave
            - returns RADIO_MUX_APPLY_REBOOT
        On off or rcp-proxy otherwise
            - returns RADIO_MUX_APPLY
    radio_mux_after_detect
        On a stored mode the detected type fits
            - returns the stored mode
        On a stored mode it doesn't fit
            - returns off, with the reason "stored mode <mode> needs <type>; detected <type>"
        On a stored mode of an unregistered consumer
            - returns off, with the reason "<mode> is not in this build"
    radio_mux_pick_driver, radio_mux_baud_verdict, radio_mux_cp210x_flow
        (unchanged behaviour of the usb_serial functions they replace; their existing tests move with them)

radio_mux_hdlc (pure)
    radio_mux_hdlc_encode
        On a frame containing 0x7E, 0x7D, 0x11, 0x13, 0xF8
            - escapes each as 0x7D, byte ^ 0x20, appends CRC-16/X.25 (escaped, little-endian), wraps in 0x7E
        On an output buffer too small
            - returns 0 and writes nothing past its end
    radio_mux_hdlc_decode_feed
        On a frame split across several chunks
            - emits it once, complete, CRC stripped
        On a bad CRC
            - drops the frame and counts it in crc_errors
        On a frame longer than the slot
            - drops it, counts it in overruns, resynchronises on the next 0x7E
        On repeated 0x7E between frames
            - emits nothing for the empty frames
    radio_mux_hdlc_chunk (UDP framing hdlc)
        On a byte stream holding two frames and half of a third
            - yields the two frames with their flags, keeps the half for the next call

radio_mux_pool (pure accounting)
    radio_mux_pool_take / radio_mux_pool_put
        On a free slot
            - returns it, in_use + 1
        On a full pool
            - returns NULL, drops + 1
        On putting a slot back
            - makes it available again; putting a slot twice asserts in host builds

radio_mux_detect (pure)
    radio_mux_detect_next
        At the start
            - asks for spinel NCP_VERSION at 460800
        On no answer within the step's share of the budget
            - moves to the next probe in the order of the Global Constraints
        On a spinel answer
            - asks for SPINEL_PROP_VENDOR_ZIGGURAT, then finishes with spinel or ziggurat
        On a ZNP SYS_VERSION reply
            - finishes with znp, version and chip from the reply
        On an ASH RSTACK
            - finishes with ezsp and its version
        On 3000 ms elapsed
            - finishes with unknown, whatever step it was in
    radio_mux_detect_parse_spinel_version
        On "OPENTHREAD/1.4.0-Koenkk-2025.3.1; CC13XX_CC26XX; Mar 13 2025"
            - returns version "1.4.0-Koenkk-2025.3.1", chip "CC13XX_CC26XX"

radio_mux (board)
    radio_mux_register_consumer
        On a new mode name
            - records the declaration (fits, limit, reboot flags, attach, detach)
        On a duplicate mode name
            - returns ESP_ERR_INVALID_STATE
    plug-in (USB new device)
        On a supported device
            - lists the channel as off, runs detection, reopens, applies radio_mux_after_detect, attaches the consumer
        On an unsupported device
            - lists it as unavailable, reason "no driver for <vid>:<pid>"
        On a device seen before
            - keeps its stored settings and firmware record, refreshed by the new detection
        On a detection that answered at a baud while the channel has none stored
            - stores that baud as the channel's baud
    radio_mux_note_firmware
        On a spinel version seen while a consumer runs
            - updates the firmware record with source passive and the time
    unplug (USB disconnected)
        On a channel with an attached consumer
            - posts GONE, closes the device once from the owner task, lists it as unavailable "not detected"
        On a replug of the same device
            - runs detection again and attaches the consumer again
    radio_mux_tx
        On a present channel
            - encodes per the channel's framing and sends; the caller's buffer is free on return
        On a gone channel
            - returns ESP_ERR_INVALID_STATE at once
    radio_mux_reset
        On the C6
            - pulses the reset GPIO and posts RESET when the RCP is back
        On a dongle
            - closes and reopens it and posts RESET
    radio_mux_set (from the API)
        On a valid change
            - stores it in NVS and applies it (attach, detach, or the long-op move through br_core)
        On line or rcp_proxy settings for an unavailable channel
            - stores them; they apply when the channel returns
        On rcp_proxy.kick
            - drops the proxy's peer, stores nothing
        On a refused change
            - returns the refusal and stores nothing
    radio_mux_forget
        On an unplugged USB channel
            - erases its settings, removes it from the list
        On the C6 or a present channel
            - returns ESP_ERR_INVALID_STATE
    C6 job (br_core push or recovery)
        On begin
            - marks c6 busy, posts GONE, releases SPI2
        On end
            - takes SPI2 back, lists c6 present, attaches its consumer again

rcp_proxy_core (pure)
    rcp_proxy_settings_check
        On connect without a host
            - returns RCP_PROXY_NEED_HOST
        On framing with tcp
            - ignores it (kept for a later switch to udp)
        On port 80, 179, 0 or above 65535
            - returns RCP_PROXY_BAD_PORT
    rcp_proxy_backoff_next
        On consecutive failures
            - returns 1, 2, 4, 8, 16, 30, 30 s
        On a failure after a connection that lasted 60 s or more
            - returns 1 s
    rcp_proxy_udp_lock
        On the first datagram
            - locks to its source
        On a datagram from another source while locked
            - refuses it, refused + 1
        On 30 s of silence from the locked source, or a kick
            - unlocks; the next source locks
    rcp_proxy_accept (TCP listen)
        (unchanged: first client wins)

rcp_proxy (board)
    attach (a channel enters rcp-proxy)
        On listen/tcp
            - listens on the port, takes the first allowed client
        On connect/tcp
            - dials host:port, retries per rcp_proxy_backoff_next, resolves .local over mDNS
        On listen/udp and connect/udp
            - binds, applies the peer lock or the fixed peer, frames per the framing setting
        On a frame channel (the C6)
            - HDLC-encodes frames towards the peer, decodes the peer's bytes into frames, replays the reset-flag
              handshake when a peer connects (TCP) or first sends (UDP)
    GONE
        On any direction
            - drops the peer and closes the sockets; attach again when the channel returns

mux_spinel_interface (OpenThread, ot_port)
    Process / WaitForFrame
        On frames pending in the channel
            - copies each into OpenThread's RX frame buffer and calls OpenThread's receive callback
        On GONE
            - WaitForFrame returns OT_ERROR_RESPONSE_TIMEOUT within 100 ms
    SendFrame
        On a present channel
            - passes the frame to the channel's TX
        On GONE
            - drops it and returns OT_ERROR_NONE
    HardwareReset
        On a dongle or the C6
            - asks the channel for a reset and waits up to 2 s for RESET or a verdict from br_core

br_core / mode_fsm
    mode_fsm_move_otbr (fake ops)
        On a probe failure of the target
            - refuses with probe_failed; no stop, no start, nothing stored
        On a successful move
            - stops, starts on the target, stores both modes, ends in BR_RUNNING
        On a start failure on the target
            - starts on the source again, stores nothing, reports the failure, ends in BR_RUNNING
        On a start failure on the target and on the source
            - ends in RADIO_FAULT
        On a drain timeout
            - refuses with drain_timeout; nothing changed
    mode_fsm_on_channel_gone
        On the OTBR channel
            - tears OpenThread down, ends in NO_RADIO
    mode_fsm_on_channel_back
        On the OTBR channel present again
            - probes and starts OpenThread, ends in BR_RUNNING
    boot
        On no channel in otbr, or its channel absent
            - ends in NO_RADIO, no fault
        On the C6 in otbr (default)
            - behaves as today

netcfg
    netcfg_peer_allowed (UDP sources)
        On a source on the Thread mesh
            - returns false
        On split mode and a source on a side where NETCFG_SVC_RCP_PROXY is not allowed
            - returns false

device_api channels_json (pure)
    device_api_channels_json
        On the registry snapshot
            - emits the spec's fields in order; usb, rcp_proxy and firmware as null where they don't apply
    device_api_channel_put_parse
        On each field
            - fills the request; a wrong type or value returns the refusal for 400
    device_api_channel_refusal_reply
        On each refusal
            - maps to its status and code (409 mode_taken, incompatible, unavailable, not_available,
              hybrid_not_supported, probe_failed, c6_busy, drain_timeout; 400 bad value; 404 unknown)
    device_api status
        On a board in NO_RADIO
            - mode "NO_RADIO", otbr_channel as stored

esp_br_guard (vendored, patch 26)
    esp_br_guard_status
        On BR_MODE_NO_RADIO and THREAD_OP or READ
            - returns 503, detail "no radio in OTBR mode"

radios.js (page)
    table
        On the snapshot
            - one row per channel, unplugged ones greyed, mode icon and name, "Off, stored: <mode>" when it doesn't fit
    dialog
        On a mode card the channel can't take
            - shows it disabled with the reason
        On choosing otbr for a channel that doesn't run it
            - turns Save into "Move Thread here" with the move text
        On connect
            - shows the host field; on udp, the framing choice
```

## Tasks

### Task 1: `radio_mux` pure core

**Files:** Create `p4/components/radio_mux/{CMakeLists.txt, include/radio_mux_core.h, core.c}`,
`test_host/{CMakeLists.txt, main/CMakeLists.txt, main/test_core.c}`. Modify `STYLEGUIDE.md` (prefix list).

**Interfaces (produces, all in `radio_mux_core.h`, standard headers only):**
- `#define RADIO_MUX_ID_MAX 48` (47 + NUL); `void radio_mux_channel_id(uint16_t vid, uint16_t pid, const char *serial,
  char out[RADIO_MUX_ID_MAX]);`
- `typedef enum { RADIO_MUX_MODE_UNAVAILABLE, RADIO_MUX_MODE_OFF, RADIO_MUX_MODE_OTBR, RADIO_MUX_MODE_ZIGGURAT,
  RADIO_MUX_MODE_RCP_PROXY, RADIO_MUX_MODE_INVALID } radio_mux_mode_t;` with `radio_mux_mode_parse(const char *)` and
  `const char *radio_mux_mode_name(radio_mux_mode_t)`.
- `typedef enum { RADIO_MUX_FW_UNKNOWN, RADIO_MUX_FW_SPINEL, RADIO_MUX_FW_ZIGGURAT, RADIO_MUX_FW_ZNP, RADIO_MUX_FW_EZSP }
  radio_mux_fw_t;` and its name function.
- `typedef struct { radio_mux_mode_t mode; radio_mux_fw_t needs; /* UNKNOWN = anything */ uint8_t limit; /* 0 = none */
  bool reboot_on_change; } radio_mux_rule_t;` and `typedef struct { radio_mux_rule_t rules[4]; size_t n; }
  radio_mux_rules_t;` (the registered consumers' declarations, copied in by the board code).
- `typedef struct { const char *id; radio_mux_mode_t mode; bool present; bool supported; radio_mux_fw_t fw; }
  radio_mux_chan_view_t;`
- `typedef enum { RADIO_MUX_APPLY, RADIO_MUX_APPLY_REBOOT, RADIO_MUX_MOVE, RADIO_MUX_REFUSE_BAD_VALUE,
  RADIO_MUX_REFUSE_UNAVAILABLE, RADIO_MUX_REFUSE_NOT_AVAILABLE, RADIO_MUX_REFUSE_INCOMPATIBLE,
  RADIO_MUX_REFUSE_MODE_TAKEN, RADIO_MUX_REFUSE_HYBRID } radio_mux_verdict_t;`
- `radio_mux_verdict_t radio_mux_check_set_mode(const radio_mux_rules_t *rules, const radio_mux_chan_view_t *chans,
  size_t n, size_t target, radio_mux_mode_t want, size_t *move_from);`
- `bool radio_mux_fits(const radio_mux_rules_t *rules, radio_mux_mode_t mode, radio_mux_fw_t fw);`
- `radio_mux_mode_t radio_mux_after_detect(const radio_mux_rules_t *rules, radio_mux_mode_t stored, radio_mux_fw_t fw,
  char *reason, size_t reason_cap);`
- `radio_mux_pick_driver`, `radio_mux_driver_name`, `radio_mux_baud_verdict`, `radio_mux_cp210x_flow`,
  `radio_mux_baud_ok`: the `usb_serial_core.h` functions of the same purpose, renamed, with their tests.
- `otbr` is a built-in rule here (needs spinel, limit 1, no reboot); `rcp-proxy` and `off` always apply.
- The stored proxy settings, shared with `rcp_proxy` (which depends on `radio_mux`, never the reverse):
  `typedef enum { RADIO_MUX_PROXY_LISTEN, RADIO_MUX_PROXY_CONNECT } radio_mux_proxy_dir_t;`,
  `typedef enum { RADIO_MUX_PROXY_TCP, RADIO_MUX_PROXY_UDP } radio_mux_proxy_adapter_t;`,
  `typedef enum { RADIO_MUX_PROXY_HDLC, RADIO_MUX_PROXY_RAW } radio_mux_proxy_framing_t;`,
  `typedef struct { radio_mux_proxy_dir_t dir; radio_mux_proxy_adapter_t adapter; uint16_t port; char host[64];
  radio_mux_proxy_framing_t framing; } radio_mux_proxy_settings_t;` and `void radio_mux_proxy_defaults(
  radio_mux_proxy_settings_t *out);`.

- [ ] **Step 1: Write the failing host tests** in `test_host/main/test_core.c`, one `TEST_CASE` per effect of the
  skeleton's `radio_mux_core` branch, e.g. `TEST_CASE("On otbr while another channel is otbr: returns MOVE with the
  other channel as the source", "[radio_mux]")`. Move the driver, baud and flow tests from
  `usb_serial/test_host/main/test_core.c`, renamed.
- [ ] **Step 2: Run, expect build failure:** `set -o pipefail; make test-host 2>&1 | tee "$LOG_DIR/t1-red.log"`.
- [ ] **Step 3: Implement** `core.c`; the CMake linux branch builds `core.c` only.
- [ ] **Step 4: Run, expect pass:** `make test-host` as above (`t1-green.log`).
- [ ] **Step 5: Commit:** `feat(radio_mux): pure core: channel ids, modes, fit rules, mode checks, driver helpers`.

### Task 2: HDLC and the frame pool (pure)

**Files:** Create `include/radio_mux_hdlc.h`, `hdlc.c`, `include/radio_mux_pool.h`, `pool.c` in
`p4/components/radio_mux/`; `test_host/main/test_hdlc.c`, `test_pool.c`.

**Interfaces:**
- `size_t radio_mux_hdlc_encode(const uint8_t *frame, size_t n, uint8_t *out, size_t cap);` (0 when `cap` is short).
- `typedef struct { uint8_t *buf; size_t cap, len; bool esc, in_frame; uint16_t crc; uint32_t crc_errors, overruns; }
  radio_mux_hdlc_dec_t;` with `void radio_mux_hdlc_dec_init(radio_mux_hdlc_dec_t *d, uint8_t *buf, size_t cap);` and
  `size_t radio_mux_hdlc_decode_feed(radio_mux_hdlc_dec_t *d, const uint8_t *in, size_t n, bool *frame_done);`
  (consumes until a frame completes or the input ends; the caller swaps `buf` for a fresh slot after `frame_done`).
- `size_t radio_mux_hdlc_chunk(const uint8_t *in, size_t n, size_t *frame_len);` (length of the first complete
  flag-delimited frame, flags included, 0 when none yet).
- `typedef struct { uint8_t *slots; size_t slot_size, n; uint32_t free_mask; uint32_t in_use, drops; }
  radio_mux_pool_t;` (n ≤ 32) with `radio_mux_pool_init`, `uint8_t *radio_mux_pool_take(radio_mux_pool_t *)`,
  `void radio_mux_pool_put(radio_mux_pool_t *, uint8_t *slot)`. Accounting only: the board code adds a spinlock.
- CRC is CRC-16/X.25 (poly 0x8408 reflected, init 0xFFFF, xorout 0xFFFF), the same as OpenThread's `Hdlc` and the
  scratchpad `spinel_probe.py` used in the spike.

- [ ] **Step 1: Failing tests** per the skeleton's `radio_mux_hdlc` and `radio_mux_pool` branches. Golden vector: the
  spinel `PROP_VALUE_GET PROTOCOL_VERSION` request with TID 1 encodes to the bytes `spinel_probe.py` sent in the spike
  (`7E 81 02 01 <crc lo> <crc hi> 7E`; compute the CRC in the test from the reference algorithm, not by hand).
- [ ] **Step 2: Run, expect failure** (`make test-host`, `t2-red.log`).
- [ ] **Step 3: Implement** `hdlc.c`, `pool.c`; add them to the linux branch.
- [ ] **Step 4: Run, expect pass** (`t2-green.log`).
- [ ] **Step 5: Commit:** `feat(radio_mux): HDLC codec, UDP chunking and frame pool accounting`.

### Task 3: Firmware detection (pure)

**Files:** Create `include/radio_mux_detect.h`, `detect.c`, `test_host/main/test_detect.c`.

**Interfaces:**
- `typedef struct { radio_mux_fw_t type; char version[48]; char chip[24]; uint32_t baud; } radio_mux_fw_record_t;`
- `typedef enum { RADIO_MUX_PROBE_SEND, RADIO_MUX_PROBE_DONE } radio_mux_probe_action_t;`
- `typedef struct { /* step, deadline, scratch */ } radio_mux_detect_t;` and
  `void radio_mux_detect_start(radio_mux_detect_t *d, int64_t now_ms);`
- `radio_mux_probe_action_t radio_mux_detect_next(radio_mux_detect_t *d, int64_t now_ms, const uint8_t *rx, size_t rx_n,
  uint32_t *baud, const uint8_t **tx, size_t *tx_n, int64_t *wait_until_ms, radio_mux_fw_record_t *result);`
  The board code reopens at `*baud` when it changes, writes `tx`, collects bytes until `wait_until_ms` or a complete
  answer, and calls again with them.
- Per-step budgets *(plan call within the 3000 ms cap)*: spinel 460800 350 ms, spinel 115200 350 ms, ZNP 400 ms, EZSP
  115200 800 ms, EZSP 460800 800 ms; the vendor-property follow-up after a spinel answer takes at most 200 ms.
- Frames: spinel requests HDLC-encoded with Task 2; ZNP `SYS_PING` = `FE 00 21 01 20`, `SYS_VERSION` = `FE 00 21 02 23`
  (MT frame, FCS = XOR of length..data); ASH `RST` = `1A C0 38 BC 7E` preceded by a cancel byte `1A`, answer `RSTACK`
  (`C1` control byte).
- `bool radio_mux_detect_parse_spinel_version(const char *s, char *version, size_t vcap, char *chip, size_t ccap);`

- [ ] **Step 1: Failing tests** per the skeleton's `radio_mux_detect` branch, feeding recorded answers (a spinel
  `PROP_VALUE_IS NCP_VERSION` with the OllyZig's string, a ZNP `SYS_VERSION` SRSP, an ASH `RSTACK`) and silence with a
  fake clock; one test asserts the 3000 ms cap with every step silent.
- [ ] **Step 2–4:** red, implement, green (`t3-*.log`).
- [ ] **Step 5: Commit:** `feat(radio_mux): firmware detection sequencer and answer parsers`.

### Task 4: `radio_mux` runtime: registry, USB transport, plug-in flow

**Files:** Create `include/radio_mux.h`, `radio_mux_priv.h`, `mux.c`, `usb_transport.c`; move
`usb_serial/idf_component.yml` to `radio_mux/`. Modify `radio_mux/CMakeLists.txt` (target branch),
`p4/main/app_main.c`, `p4/main/CMakeLists.txt`, `common/board_cores/include/board_cores.h`, `p4/pytest/core_map.py`.
Test: `test_host/main/test_attach.c` against a pure attach state machine in `core.c` (below).

**Interfaces (produces, `radio_mux.h`):**
- `typedef enum { RADIO_MUX_EV_RX_FRAME, RADIO_MUX_EV_RESET, RADIO_MUX_EV_GONE } radio_mux_ev_type_t;`
  `typedef struct { radio_mux_ev_type_t type; uint8_t *slot; size_t len; int64_t rx_us; } radio_mux_ev_t;`
- `typedef struct radio_mux_chan radio_mux_chan_t;` (opaque handle).
- `typedef struct { radio_mux_mode_t mode; radio_mux_fw_t needs; uint8_t limit; bool reboot_on_change; bool frames; /*
  false = raw bytes */ esp_err_t (*attach)(radio_mux_chan_t *ch, void *ctx); void (*detach)(radio_mux_chan_t *ch, void
  *ctx); void *ctx; } radio_mux_consumer_t;`
- `esp_err_t radio_mux_register_consumer(const radio_mux_consumer_t *c);` (before `radio_mux_start`).
- `esp_err_t radio_mux_start(void);` (loads NVS, registers `c6`, starts the USB host and the owner task).
- Consumer side: `esp_err_t radio_mux_bind_queue(radio_mux_chan_t *ch, QueueHandle_t q);`,
  `int radio_mux_eventfd(radio_mux_chan_t *ch);` (OTBR), `bool radio_mux_poll(radio_mux_chan_t *ch, radio_mux_ev_t
  *ev);` (non-blocking take of the next event for eventfd users), `void radio_mux_release(radio_mux_chan_t *ch, uint8_t
  *slot);`, `esp_err_t radio_mux_tx(radio_mux_chan_t *ch, const uint8_t *data, size_t n);`,
  `esp_err_t radio_mux_reset(radio_mux_chan_t *ch);`, `size_t radio_mux_read_raw(radio_mux_chan_t *ch, uint8_t *out,
  size_t cap);` (raw consumers), `const char *radio_mux_chan_id(const radio_mux_chan_t *ch);`,
  `void radio_mux_note_firmware(radio_mux_chan_t *ch, const radio_mux_fw_record_t *fw);` (passive updates).
- API side: `typedef struct { bool has_mode; radio_mux_mode_t mode; bool has_baud; uint32_t baud; bool has_flow; bool
  flow; bool has_proxy; radio_mux_proxy_settings_t proxy; uint8_t proxy_fields; /* bit per field given */ bool kick; }
  radio_mux_set_req_t;` (`kick` is never stored), `typedef struct { ... } radio_mux_snapshot_t;` (every channel with the spec's API fields, counters
  `frames_in`, `frames_out`, `bytes_in`, `bytes_out`, `drops`, `resets`) with `void radio_mux_get_snapshot(
  radio_mux_snapshot_t *out);`, `radio_mux_verdict_t radio_mux_set(const char *id, const radio_mux_set_req_t *req,
  char *moved_from, size_t cap);`, `esp_err_t radio_mux_forget(const char *id);`, `const char
  *radio_mux_otbr_channel(void);`.
- br_core side: `void radio_mux_set_otbr_mover(esp_err_t (*move)(const char *from, const char *to));` (the mux calls
  it for a `RADIO_MUX_MOVE` verdict; br_core runs the move as the long operation of Task 7).
- Pure attach state machine in `core.c`: `radio_mux_attach_next(state, event) -> action` over states `ABSENT, PROBING,
  REOPENING, IDLE, ATTACHED, CLOSING` and events `PLUG, DETECT_DONE, REOPENED, MODE_SET, UNPLUG, CLOSED`, with host
  tests for the Review Focus 2 sequence (`UNPLUG` while `PROBING`; `PLUG` while `CLOSING` waits for `CLOSED`).

- [ ] **Step 1: Failing host tests** for `radio_mux_attach_next` (every transition of the skeleton's plug-in and unplug
  branches, plus Review Focus 2) and for the non-blocking post: a helper `radio_mux_post_rx(queue_full)` in `core.c`
  that returns "dropped" without waiting.
- [ ] **Step 2: Run, expect failure** (`t4-red.log`).
- [ ] **Step 3: Implement the pure additions, then the board code:**
  - `usb_transport.c` from `usb_host_glue.c` (renamed, one open handle, the single-closer rule, `data_cb` feeding
    either the raw ring with an eventfd poke or the HDLC decoder into pool slots with a queue post).
  - `mux.c`: owner task, NVS namespace `radio_mux` with an index key `chans` and one blob per channel (key `c6` or
    `u` + 8 hex of an FNV-1a hash of the id; blob version 1: stored mode, line, `rcp_proxy`, firmware record, first and
    last seen), detection driven by `radio_mux_detect_next`, consumer registration and attach. `ziggurat` has no
    built-in consumer.
  - `app_main.c`: `radio_mux_start()` after `ot_console_init` and before `net_init_task` starts br_core (the C6
    consumer registration happens in Task 7; until then `c6` has no consumer and br_core keeps its own SPI path).
- [ ] **Step 4: Run** `make test-host` (`t4-green.log`) and `make build` (`t4-build.log`): both pass.
- [ ] **Step 5: Commit:** `feat(radio_mux): channel registry, USB transport, detection at plug-in, consumer attach`.

### Task 5: `rcp_proxy` component (replaces the usb_serial TCP half)

**Files:** Create `p4/components/rcp_proxy/` per the file map. Modify `p4/components/netcfg/` (rename the service
class; add `netcfg_peer_allowed`), `routing/task.c` and `routing/CMakeLists.txt`, `p4/sdkconfig.defaults`,
`board_cores.h`, `core_map.py`, `app_main.c` (register the consumer before `radio_mux_start`), `STYLEGUIDE.md`. Delete
`p4/components/usb_serial/`.

**Interfaces:**
- `rcp_proxy_core.h` (includes the pure `radio_mux_core.h` for `radio_mux_proxy_settings_t`):
  `typedef enum { RCP_PROXY_OK, RCP_PROXY_NEED_HOST, RCP_PROXY_BAD_PORT, RCP_PROXY_BAD_HOST } rcp_proxy_check_t;`,
  `rcp_proxy_check_t rcp_proxy_settings_check(const radio_mux_proxy_settings_t *s);`, `uint32_t
  rcp_proxy_backoff_next(uint32_t prev_s, int64_t connected_for_ms);`, the UDP lock
  (`rcp_proxy_udp_lock_offer(lock, src, now_ms) -> bool`, `rcp_proxy_udp_lock_kick(lock)`), the ring and accept rule
  moved from usb_serial.
- `rcp_proxy_priv.h`: the adapter interface `typedef struct { const char *name; bool datagram; esp_err_t
  (*open)(proxy_t *p); void (*close)(proxy_t *p); int (*fds)(proxy_t *p, fd_set *rd, int maxfd); void
  (*ready)(proxy_t *p, fd_set *rd); esp_err_t (*send)(proxy_t *p, const uint8_t *d, size_t n); }
  rcp_proxy_adapter_t;` with `rcp_proxy_adapter_tcp` and `rcp_proxy_adapter_udp`. spinel2quic later adds one more
  instance; nothing else changes.
- `rcp_proxy.h`: `esp_err_t rcp_proxy_register(void);` (registers the `rcp-proxy` consumer: needs nothing, no limit,
  no reboot, `frames` false on USB channels; for the C6 it asks for frames and encodes HDLC itself);
  `void rcp_proxy_kick(const char *channel_id);`; status for the snapshot (`state`: `listening`, `connecting`,
  `backoff`, `connected`; `peer`).
- `netcfg.h`: `bool netcfg_peer_allowed(const struct sockaddr *peer, netcfg_service_t svc);`.

- [ ] **Step 1: Failing host tests** per the skeleton's `rcp_proxy_core` branch and the netcfg `netcfg_peer_allowed`
  cases in `test_policy.c`.
- [ ] **Step 2: Run, expect failure** (`t5-red.log`).
- [ ] **Step 3: Implement** the core, the two adapters, the consumer task (one `select` over the adapter fds, the
  channel's eventfd and a kick eventfd; no polling timeout below 1 s), the budget changes, and delete `usb_serial`
  (its pieces are now in `radio_mux` and `rcp_proxy`).
- [ ] **Step 4: Run** `make test-host` (`t5-green.log`) and `make build` (`t5-build.log`).
- [ ] **Step 5: Commit:** `feat(rcp_proxy): listen/connect over TCP and UDP adapters; usb_serial removed`.

### Task 6: C6 transport

**Files:** Create `p4/components/radio_mux/c6_transport.c`; modify `mux.c`, `radio_mux_priv.h`, `CMakeLists.txt`
(requires `esp_driver_spi`, `esp_driver_gpio`, `rcp_sync`), `board_cores.h`, `core_map.py`.

**Interfaces:**
- Internal: `esp_err_t c6_transport_open(void);`, `void c6_transport_close(void);` (holds the C6 in reset),
  `esp_err_t c6_transport_tx(const uint8_t *frame, size_t n);`, `esp_err_t c6_transport_reset(void);`.
- Public, for br_core's job paths: `esp_err_t radio_mux_c6_job_begin(void);` (refuses with `ESP_ERR_INVALID_STATE` when
  `rcp-proxy` has a peer on `c6`; otherwise GONE, SPI2 released), `void radio_mux_c6_job_end(bool c6_ok);`.
- `c6` is `unavailable` when rcp_sync's last probe reports no RCP, the ESP-Hosted factory firmware, or a failed ROM
  recovery; the reason names which (`rcp_sync_get_last_probe`).
- SPI framing per OpenThread's `spi_frame.hpp`: 5-byte header (flag byte with pattern `0x02` and reset bit `0x80`,
  accept length, data length, both little-endian), the reset bit on the first transaction after open or reset, a
  15 ms retry on an invalid header (as IDF's interface does). Transactions are full duplex into a pool slot; TX and the
  INT-driven RX share one mutex; a frame received during a TX transaction is posted like any other.

- [ ] **Step 1: Failing host tests** for the pure header encode and decode helpers (in `core.c`: `radio_mux_spi_hdr_
  encode/decode`) against the byte layouts above, including the reset bit and a corrupt pattern.
- [ ] **Step 2–3:** red, implement the helpers, then `c6_transport.c` (GPIO negedge ISR on `P4_PIN_RCP_INT` notifying
  the C6 task; the task runs the RX transaction into a slot and posts it).
- [ ] **Step 4: Run** `make test-host` and `make build` (`t6-*.log`).
- [ ] **Step 5: Commit:** `feat(radio_mux): C6 transport over SPI2 with frame-pool DMA and job hand-off`.

### Task 7: OTBR on any channel

**Files:** Create `tools/ot_radio_port.cmake`, `p4/ot_port/esp_openthread_mux.h`, `p4/ot_port/mux_spinel_interface.cpp`.
Modify `p4/CMakeLists.txt`, `p4/components/br_core/{br_core.c, include/br_core.h, include/br_core_mode.h, mode_fsm.c,
mode_fsm.h, CMakeLists.txt, br_core_test.c, test_host/main/test_mode_fsm.c}`, `p4/components/PATCHES.md`
("Generated ESP-IDF copies").

**Interfaces:**
- `tools/ot_radio_port.cmake` (from the spike's `tools/dongle_spike.cmake`): generates
  `${CMAKE_BINARY_DIR}/ot_port_components/openthread` with symlinks to IDF's component, a real `src/port` and `include`
  holding symlinks plus `mux_spinel_interface.cpp` and `esp_openthread_mux.h`, and a patched copy of
  `src/port/esp_openthread_radio_spinel.cpp`: the static adapter becomes the mux interface, the concrete-only calls
  route to it, and `s_radio_caps` becomes the Global Constraints set. Every replaced text is checked with
  `string(FIND)`; a miss is a `FATAL_ERROR` naming the IDF file. Kconfig stays `CONFIG_OPENTHREAD_RADIO_SPINEL_SPI`.
- `esp_openthread_mux.h` (C, seen by br_core through its `openthread` requirement):
  `typedef struct { esp_err_t (*tx)(void *ctx, const uint8_t *f, size_t n); bool (*poll)(void *ctx, uint8_t **slot,
  size_t *len); void (*release)(void *ctx, uint8_t *slot); int (*eventfd)(void *ctx); esp_err_t (*reset)(void *ctx);
  bool (*gone)(void *ctx); } esp_openthread_mux_ops_t;` and `void esp_openthread_mux_set(const esp_openthread_mux_ops_t
  *ops, void *ctx);` (called by br_core before every `esp_openthread_init`).
- `br_core_mode.h`: `BR_MODE_NO_RADIO`.
- `mode_fsm.h`: ops gain `esp_err_t (*probe_channel)(const char *id, char *err, size_t cap)`, `esp_err_t
  (*ot_start)(const char *id)`, `esp_err_t (*ot_stop)(void)`, `esp_err_t (*store_otbr)(const char *from, const char
  *to)`; `esp_err_t mode_fsm_move_otbr(mode_fsm_t *fsm, const char *from, const char *to, char *err, size_t cap);`,
  `void mode_fsm_on_channel_gone(mode_fsm_t *fsm);`, `void mode_fsm_on_channel_back(mode_fsm_t *fsm, const char *id);`.
  The existing `reset_rcp` op now calls `radio_mux_reset` on the OTBR channel; `push_firmware` and `rom_recover` wrap
  rcp_sync between `radio_mux_c6_job_begin` and `_end`.
- `br_core.c`: registers the `otbr` consumer with the mux (`frames` true); its attach runs boot_radio's probe through
  the channel (spinel reset, `PROP_NCP_VERSION`, caps) and then `esp_openthread_init`, and records the version with `radio_mux_note_firmware`; detach and GONE go through
  `mode_fsm_on_channel_gone`. The C6-specific boot steps (rcp_sync probe, ROM recovery) run only for `c6`. The
  dongle spike's `#ifdef OTBR_DONGLE_SPIKE` code is not carried over. `/device/status` gets `otbr_channel` through
  `br_core_get_status`.
- `HardwareReset` waiting up to 2 s and `WaitForFrame` returning within 100 ms after GONE are the interface's half of
  "torn down before `DieNow`"; br_core's half is tearing down on GONE from its own task.

- [ ] **Step 1: Failing host tests** in `test_mode_fsm.c` per the skeleton's `br_core / mode_fsm` branch, with the
  existing fake-ops pattern (a fake probe that fails, a fake `ot_start` failing on the target only, then on both).
- [ ] **Step 2: Run, expect failure** (`t7-red.log`).
- [ ] **Step 3: Implement** the cmake generator, the interface, the mode FSM additions and br_core's consumer. Write
  the `PATCHES.md` entry listing each replaced text of the generated file.
- [ ] **Step 4: Run** `make test-host` (`t7-green.log`), `make build` (`t7-build.log`), and `make test-vendored`
  (`t7-vendored.log`): all pass. Check in `p4/build` that `mux_spinel_interface.cpp` and the generated radio spinel file
  are the ones compiled (`grep` the ninja file for the `ot_port_components` paths).
- [ ] **Step 5: Commit:** `feat(br_core): OpenThread on any mux channel, moves with rollback, NO_RADIO`.

### Task 8: Device API and the REST guard

**Files:** Create `p4/components/device_api/channels_json.c/.h`, `test_host/main/test_channels_json.c`. Modify
`device_api.c` (routes `GET /device/channels`, `PUT /device/channels/*`, `DELETE /device/channels/*`; remove
`/device/usb_serial`), `status_json.c` (`otbr_channel`, `NO_RADIO`), `long_op.c` and `device_api_priv.h`
(`DEVICE_API_OP_RADIO_MOVE` in the shared slot with OTA, push and recovery), `CMakeLists.txt`,
`test_host/main/{CMakeLists.txt, test_status.c, test_long_op.c}`, `esp_ot_br_server/src/esp_br_guard.c`, its host test,
`src/esp_br_web.c` (`EXTRA_HANDLER_SLOTS` 29 → 30), `PATCHES.md` (entry 26). Delete `usb_serial_json.*` and its test.

**Interfaces:**
- `int device_api_channels_json(char *buf, size_t cap, const radio_mux_snapshot_t *s);` (field order as the spec's API
  section; returns length or -1).
- `radio_mux_set_req_t` (Task 4) is filled here: `esp_err_t device_api_channel_put_parse(const char *body, radio_mux_set_req_t *out, const char
  **bad_field);`.
- `void device_api_channel_refusal(radio_mux_verdict_t v, int *status, const char **code);`.
- PUT accepts `rcp_proxy.kick: true` (drops the current peer; not stored) *(plan-level refinement: the spec's
  `kick` had no field)*.
- A move answers 202 `{"moving": true, "moved_from": "<id>"}` and the channel reports `state` `starting` until it
  settles; a long operation already running answers 409 `busy` with the existing detail text.

- [ ] **Step 1: Failing host tests** per the skeleton's `device_api` and `esp_br_guard` branches.
- [ ] **Step 2–4:** red, implement, green: `make test-host` and `make test-vendored` (`t8-*.log`), `make build`.
- [ ] **Step 5: Commit:** `feat(device_api): /device/channels, NO_RADIO in status and the REST guard`.

### Task 9: Radios page

**Files:** Create `p4/components/esp_ot_br_server/frontend/radios.html`, `frontend/static/radios.js`,
`tools/tests/test_radios_page.py`. Modify `frontend/static/api.js` (Radios first in the Device group; the Thread-menu rename below only if the user assigns it to
this plan),
`status.js` (Dashboard chip "Thread runs on: <label>"; `NO_RADIO` banner), `mgmt.js` and `capture.js` (the shared
`NO_RADIO` rule: hidden pages when no channel is in `otbr`, notice when the OTBR channel is unavailable or faulted),
`device_common.js` (shared helpers for that rule), `style.css` (the table, the dialog and the mode cards, from the
mockup's CSS), `PATCHES.md` (the frontend files are vendored: add them to entry 26).

**Navigation:** the hide rule applies to the Thread menu (Network, Topology, Commissioner, Addresses, Tools, Console),
Routing and Packet capture. The rename of Management to Thread with Topology inside, and the Zigbee link, are decided
(top bar: Dashboard, Thread, Zigbee, Device, About); whether this task or the Zigbee work implements the rename is
open, so ask before starting this task. The Zigbee link itself is never this plan's.

**Interfaces:** the page reads `GET /device/channels` and `/device/status`, writes with `PUT` and `DELETE`, polls every
2 s while a channel is `starting`. Structure, icons and texts follow the mockup; the mode cards come from a list in
`radios.js` (`off`, `otbr`, `rcp-proxy`, `ziggurat` disabled with "Not in this build" unless the snapshot lists it as
available).

- [ ] **Step 1: Failing page tests** in `test_radios_page.py` with `device_pages_harness.js`, per the skeleton's
  `radios.js` branch, using recorded `/device/channels` bodies (the C6 running, the OllyZig in `connect`, an unplugged
  SLZB-06, an EZSP stick stored as `otbr`).
- [ ] **Step 2–4:** red, implement, green: `make test-tools` (`t9-*.log`), then `make build`.
- [ ] **Step 5: Commit:** `feat(web): Radios page with channel table, configure dialog and mode cards`.

### Task 10: On-board tests and docs

**Files:** Create `p4/pytest/test_channels.py` (no dongle), `p4/pytest/test_dongle.py` (marker `usb_dongle`),
`docs/radios.md`. Modify `p4/pytest/pytest.ini`, `docs/api.md`, `docs/README.md`, `mkdocs.yml`, `CHANGELOG.md`. Delete
`docs/usb-serial.md`.

- [ ] **Step 1: Write the tests** (not run here):
  - `test_channels.py`: `test_c6_listed_in_otbr`, `test_put_validation` (each 400 and 409 code), `test_c6_off_and_back`
    (Thread stops, `NO_RADIO`, the same dataset after it returns), `test_ziggurat_not_available`,
    `test_rest_503_in_no_radio`.
  - `test_dongle.py`: `test_detect_ollyzig_spinel` (type, chip `CC13XX_CC26XX`, baud 460800),
    `test_move_thread_to_dongle_and_back` (same network both ways), `test_unplug_under_otbr` (manual unplug prompt;
    `NO_RADIO`, no reboot, `uptime_s` keeps growing), `test_replug_restores_thread`, `test_proxy_listen_tcp_latency`
    (spinel round trip through the proxy below 15 ms median), `test_proxy_connect_backoff`,
    `test_proxy_udp_hdlc_roundtrip`, `test_c6_proxy_otbr_agent` (container `usbproxy-otbr:local` from the spike).
- [ ] **Step 2: Docs:** `docs/radios.md` (channels, modes, detection, the proxy's directions and adapters, moving
  Thread, unplug behaviour), the API section for `/device/channels` and the status changes in `docs/api.md`, index and
  nav entries, a CHANGELOG entry.
- [ ] **Step 3: Run** `make lint` on the changed files and `make docs` (`t10-*.log`).
- [ ] **Step 4: Commit:** `test, docs(radio_mux): on-board tests and the Radios documentation`.

### Task 11: Hardware pass (after the final review and its fixes)

Budget: one P4 app flash over the network (`make ota BOARD=192.168.1.117` after `make build-test`), no C6 flash.
Before it, read `/device/status` `app.version` in a separate step and compare with the last build put on the board
(the rig memory's rule); check `fuser -v` on every serial port before opening one; use the ha-thread-bbc1 network.

- [ ] `make test-target TEST=p4/pytest/test_channels.py` (with `TARGET_PYTEST` per the rig notes for a worktree).
- [ ] With the OllyZig on the USB-A port: `make test-target TEST=p4/pytest/test_dongle.py MARK=usb_dongle`, with the
  manual unplug and replug steps.
- [ ] Review Focus checks: `unplug_during_rest_load` (unplug while a script polls `/node/dataset/active`), the quick
  replug, the forced wrong-baud move with rollback, 10 minutes of `connect` refusals with the socket count in
  `/device/diag` unchanged.
- [ ] With the user present and only if they agree to the dongle flashes: flash the OllyZig's Z-Stack backup
  (`~/esp/phase0/backup/ollyzig-znp-full-20261106.bin`, manual BSL), plug it in with stored mode `otbr`, check it
  stays `off` with the reason, then flash the Koenkk RCP back.
- [ ] A targeted subset of the existing suite on the C6 (`test_br_boot.py`, `test_rest_guard.py`, `test_sniffer.py`,
  `test_rcp_push.py`) as the regression gate for "no dongle behaves as before".
- [ ] Record the results in the session's memory notes and the PR description.
