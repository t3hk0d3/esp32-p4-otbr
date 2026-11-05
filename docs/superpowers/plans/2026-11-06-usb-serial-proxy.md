# USB Serial Proxy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A USB serial dongle on the board's USB-A (OTG) port served byte for byte to one TCP client on port 6638
(listen mode), with settings and status at `/device/usb_serial`, without disturbing the C6 border router.

**Architecture:** A new component `usb_serial` holds a host-tested pure core (settings check, driver choice, client
rule, ring, baud and flow-control helpers) and the board glue: one owner task that serialises every state change, the
USB host library daemon and the CDC-ACM driver on core 1, and the TCP listener and client. `netcfg` gains a service
class and a socket-side check that refuses the Thread mesh. `device_api` adds the JSON and the two routes.

**Tech Stack:** ESP-IDF 6.1, `espressif/usb` 1.6.0, `espressif/usb_host_cdc_acm` 2.4.1,
`espressif/usb_host_cp210x_vcp` and `espressif/usb_host_ch34x_vcp` 2.2.0, lwIP sockets, cJSON, Unity host tests on the
`linux` target, pytest-embedded with `requests` and raw sockets on the board.

**Spec:** `docs/superpowers/specs/2026-11-05-usb-serial-proxy-design.md`

## Styleguide

`STYLEGUIDE.md` (enforced by `logrun make lint`): astyle for C, ruff for Python, `snake_case`, public symbols prefixed
with the component name (`usb_serial_`, `netcfg_`, `device_api_`), one `static const char *TAG` per file named after
the component, `esp_err_t` returns with `ESP_RETURN_ON_ERROR` / `ESP_GOTO_ON_ERROR`, `ESP_ERROR_CHECK` only in
`app_main` init, host tests in `<component>/test_host/` named `TEST_CASE("<scenario>: <effect>", "[<component>]")`,
target tests in `p4/pytest/test_usb_serial.py`, test-only code under `#ifdef OTBR_TEST_BUILD`, fixed hardware facts as
header constants (not Kconfig), every routine command through a `make` target. Vendored `esp_ot_br_server` changes go
into `p4/components/PATCHES.md` (next entry: 25). Every task follows it.

## Global Constraints

- Off by default. Settings in NVS namespace `usb_serial`, keys `enabled` (u8), `port` (u16), `baud` (u32), `flow` (u8).
- Defaults: port **6638**, baud **115200**, flow control **off**.
- Accepted baud rates, exactly: 38400, 57600, 115200, 230400, 460800, 921600, 1000000, 2000000.
- Port 1–65535, and not a TCP port the board itself listens on: 80 (web), 179 (BGP).
- Validation never depends on which dongle is attached.
- Drivers: CP210x for VID `0x10C4` PID `0xEA60`/`0xEA70`/`0xEA71`; CH34x for VID `0x1A86` PID
  `0x7522`/`0x7523`/`0x5523`; CDC-ACM only for a device with an interface of class `0x0A` (CDC data) and either the
  device class or an interface class `0x02` (CDC communications); everything else unsupported and left closed.
- On open: line coding (baud, 8N1), then flow control, then DTR and RTS set together, both asserted, in **one**
  `cdc_acm_host_set_control_line_state(hdl, true, true)` call (one `SET_MHS` on CP210x); never toggled afterwards.
- Flow control: CP210x only, through the vendor request `SET_FLOW` (0x13) with the 16-byte payload from
  `usb_serial_cp210x_flow`; on other drivers `flow_control: true` is not applied and `dongle.error` says
  `"flow control not supported by <driver>"`.
- Baud read-back (`cdc_acm_host_line_coding_get`): equal → fine; within 3 % → open, `dongle.error` =
  `"baud <set> read back as <actual>"`; further off → closed with the same text. *(Plan-level refinement: the spec's
  example rounding, 921600 → 923076, is 0.16 % and a UART runs fine with it, so it must not close the port.)*
- TCP: one listener, IPv6 dual-stack on `in6addr_any`, backlog 1. Client socket options: `TCP_NODELAY` 1,
  `SO_KEEPALIVE` 1, `TCP_KEEPIDLE` 10, `TCP_KEEPINTVL` 2, `TCP_KEEPCNT` 3.
- First client keeps the port: a later connection is accepted and closed at once with `SO_LINGER` {1, 0} (RST, no
  TIME_WAIT), counted in `refused_connections`.
- Access: `NETCFG_SVC_USB_SERIAL`; split mode allows both the management and the backbone side; any connection whose
  local address is on the OpenThread netif is refused in every mode.
- Rings: dongle → client at least 8192 bytes (above the 5760-byte TCP send buffer). A push that would overflow leaves
  the ring unchanged and makes the owner close the client (`overflow_disconnects` +1). Both rings are flushed on a new
  client and on a dongle open.
- Client → dongle: read from the socket only while a dongle is open; with no dongle, read and discard
  (`discarded_from_client` += n).
- Sends to the client are coalesced: flush when the ring holds at least 1024 bytes or 5 ms after its first pending byte.
- Tasks, all on `MGMT_CORE`: `usb_serial` (owner) priority 6, `usb_lib` (USB host daemon) priority 7, `USB-CDC` (CDC
  driver task, through `cdc_acm_host_driver_config_t`) priority 7. `usb_host_install` is called from a core-1 task.
  Nothing new on `NET_CORE`.
- Shutdown order: stop accepting; close the client; `cdc_acm_host_close`; `cdc_acm_host_uninstall`;
  `usb_host_device_free_all` (retry while `ESP_ERR_NOT_FINISHED`); wait for `USB_HOST_LIB_EVENT_FLAGS_ALL_FREE`;
  `usb_host_uninstall`; the `usb_lib` task ends.
- Only the owner task calls `cdc_acm_host_open*`, `cdc_acm_host_close`, `cdc_acm_host_uninstall` and closes sockets;
  callbacks and HTTP handlers post messages.
- lwIP: `USB_SERIAL_SOCKETS` = 3; `CONFIG_LWIP_MAX_SOCKETS` 18 → 21; `CONFIG_LWIP_MAX_ACTIVE_TCP` 16 → 20.
- API: `GET`/`PUT /device/usb_serial`, class `NETCFG_SVC_DEVICE_API` (the HTTP routes; the TCP port has its own
  class), working in every border router mode. Body as in the spec, field order: `enabled`, `port`, `baud`,
  `flow_control`, `dongle`, `client`, `bytes_to_dongle`, `bytes_from_dongle`, `discarded_from_client`,
  `overflow_disconnects`, `refused_connections`. PUT fields: `enabled`, `port`, `baud`, `flow_control`, `kick`; `kick`
  is not stored. Errors use the device API's `send_error` with code `invalid_request`.
- The settings API keeps room for connect mode: no field is named so that `mode`/`host` would need a rename; a missing
  `mode` will mean `"listen"`.

## Review Focus

1. A dongle unplugged while the owner is writing to it or while the proxy is being turned off: no abort, the next
   plug-in works (Task 3 on-board check, Task 5 pytest `test_disable_while_unplugged`).
2. A client that connects before any dongle and sends bytes: those bytes never reach the dongle when it appears
   (Task 1 host test on the ring flush rule, Task 5 loopback test `test_stale_bytes_discarded`).
3. A PUT changing the port while a client is connected: the client is dropped, the new port listens, the old one is
   closed (Task 5 pytest `test_port_change_moves_listener`).
4. A connection from a Thread mesh address in single mode is refused (Task 2 host test on the decision; Task 5 manual
   check from a Thread node is optional, called out in the hardware pass).
5. A burst from the dongle while the client reads slowly: the client is disconnected, never fed spliced data
   (Task 1 ring overflow test, Task 5 loopback test `test_slow_reader_disconnected`).

## File map

- Create `p4/components/usb_serial/`:
  - `CMakeLists.txt` (linux target builds `core.c` only, like `ot_console`), `idf_component.yml` (the four USB
    components, versions as in Tech Stack)
  - `include/usb_serial_core.h` (pure, host-safe), `core.c`
  - `include/usb_serial.h` (board API), `usb_serial_priv.h` (owner messages, shared state)
  - `usb_serial.c` (init, NVS, owner task, status snapshot), `usb_host_glue.c` (USB host lifecycle, driver open,
    line coding, flow control, data and event callbacks), `tcp.c` (listener, accept, client options, coalesced send)
  - `test_host/` (Unity project like `ot_console/test_host`)
- Modify `p4/components/netcfg/include/netcfg_policy.h`, `policy.c`, `include/netcfg.h`, `netcfg.c`,
  `netcfg_glue.h`, `ifaces.c`, `test_host/main/test_policy.c`.
- Create `p4/components/device_api/usb_serial_json.c`, `usb_serial_json.h`, `test_host/main/test_usb_serial_json.c`;
  modify `device_api.c`, `CMakeLists.txt`, `test_host/main/CMakeLists.txt`.
- Modify `p4/components/esp_ot_br_server/src/esp_br_web.c` (`EXTRA_HANDLER_SLOTS` 27 → 29),
  `p4/components/PATCHES.md`.
- Modify `p4/components/routing/task.c` (`SOCKETS_NEEDED`), `p4/components/routing/CMakeLists.txt`,
  `p4/sdkconfig.defaults`, `common/board_cores/include/board_cores.h` (placement comment), `p4/pytest/core_map.py`,
  `p4/main/app_main.c`, `p4/main/CMakeLists.txt`.
- Create `p4/pytest/test_usb_serial.py`; modify `p4/pytest/pytest.ini` (marker `usb_loopback`).
- Create `docs/usb-serial.md`; modify `docs/README.md`, `docs/api.md`, `mkdocs.yml` (nav), `CHANGELOG.md`.

## BDD skeleton

```
usb_serial_core (pure)
    usb_serial_settings_check
        On the defaults
            - returns USB_SERIAL_OK
        On each accepted baud rate
            - returns USB_SERIAL_OK
        On a baud rate not in the list (9600, 115201, 3000000)
            - returns USB_SERIAL_BAD_BAUD
        On port 0, 80 or 179
            - returns USB_SERIAL_BAD_PORT
        On port 1 and port 65535
            - returns USB_SERIAL_OK
    usb_serial_settings_default
        On a call
            - gives enabled false, port 6638, baud 115200, flow_control false
    usb_serial_pick_driver
        On 10c4:ea60, 10c4:ea70, 10c4:ea71
            - returns USB_SERIAL_DRV_CP210X
        On 1a86:7522, 1a86:7523, 1a86:5523
            - returns USB_SERIAL_DRV_CH34X
        On a device with interfaces of class 0x02 and 0x0A (any VID)
            - returns USB_SERIAL_DRV_CDC_ACM
        On a device of class 0x02 with one interface of class 0x0A
            - returns USB_SERIAL_DRV_CDC_ACM
        On an FTDI device (0403:6001, vendor class 0xFF)
            - returns USB_SERIAL_DRV_NONE
        On a mass-storage device (interface class 0x08)
            - returns USB_SERIAL_DRV_NONE
        On a device with only a CDC data interface and no communications class
            - returns USB_SERIAL_DRV_NONE
    usb_serial_driver_name
        On each driver
            - returns "cp210x", "ch34x", "cdc_acm", or NULL for NONE
    usb_serial_accept
        On a connection with no client
            - returns USB_SERIAL_TAKE
        On a connection while a client is connected
            - returns USB_SERIAL_REFUSE
    usb_serial_ring
        On pushes within the capacity
            - take returns the bytes in order, across the wrap point
        On a push that would overflow
            - returns false and leaves the ring's contents and length unchanged
        On clear
            - leaves the ring empty
    usb_serial_baud_verdict
        On actual == requested
            - returns USB_SERIAL_BAUD_EXACT
        On 921600 read back as 923076
            - returns USB_SERIAL_BAUD_CLOSE
        On 115200 read back as 9600, and on actual 0
            - returns USB_SERIAL_BAUD_FAR
    usb_serial_cp210x_flow
        On flow control off
            - writes ulControlHandshake 0x00000001, ulFlowReplace 0x00000040, ulXonLimit 128, ulXoffLimit 128,
              little-endian
        On flow control on
            - writes ulControlHandshake 0x00000009, ulFlowReplace 0x00000080, ulXonLimit 128, ulXoffLimit 128
netcfg
    netcfg_policy_allows (NETCFG_SVC_USB_SERIAL)
        On split mode, management side
            - returns true
        On split mode, Thread backbone side
            - returns true
        On single mode, either side
            - returns true
    netcfg_mesh_refused
        On a local address on the OpenThread netif, single or split mode
            - returns true (refuse)
        On a local address on an Ethernet interface
            - returns false
    netcfg_socket_allowed (board)
        On a connection to the management or backbone address
            - returns true
        On a connection whose side cannot be found within 100 ms (netcfg lock held by a trial)
            - returns false and logs a warning
usb_serial (board)
    usb_serial_init
        On a board with nothing stored
            - loads the defaults and starts nothing
        On a stored enabled=1
            - starts the USB host and the listener
    usb_serial_apply
        On invalid settings
            - returns ESP_ERR_INVALID_ARG and stores nothing
        On enabled false → true
            - stores, starts the USB host and the listener, returns once the owner has done it (at most 5 s)
        On enabled true → false
            - closes the client, closes the dongle, uninstalls the USB host in the spec's order
        On a new port while running
            - closes the client and the old listener, listens on the new port
        On a new baud or flow setting with a dongle open
            - applies it to the open dongle; failures go to dongle.error
    usb_serial_kick
        On a connected client
            - closes it; the next connection is taken
        On no client
            - does nothing and returns ESP_OK
    dongle events
        On a supported dongle plugged in
            - opens it, flushes both rings, sets line coding, flow, DTR+RTS together; status shows open
        On an unsupported device plugged in
            - status shows its vid/pid with driver null; nothing is opened
        On the dongle unplugged
            - closes it in the owner task and closes the client; status dongle null
        On a baud rate read back far off
            - closes the dongle, dongle.error set
    client events
        On a first connection
            - sets the socket options, flushes both rings, status shows the peer
        On a second connection
            - is closed with RST; refused_connections +1; the first client keeps working
        On a connection refused by netcfg
            - is closed with RST; refused_connections +1
        On bytes from the client with a dongle open
            - are written to the dongle unchanged; bytes_to_dongle += n
        On bytes from the client with no dongle
            - are discarded; discarded_from_client += n
        On bytes from the dongle with a client
            - reach the client unchanged; bytes_from_dongle += n
        On a dongle burst the client cannot take
            - the client is closed; overflow_disconnects +1
        On a client that vanishes
            - is closed by keepalive within 20 s
device_api usb_serial
    device_api_usb_serial_json
        On no dongle and no client
            - gives the exact body with "dongle":null and "client":null
        On an open cp210x dongle and a client
            - gives the exact body from the spec
        On an unsupported device
            - gives "dongle":{"vid":"0403","pid":"6001","driver":null,"open":false,"flow_control":false,"error":null}
        On a cap too small
            - returns -1
    device_api_usb_serial_parse
        On a subset ({"baud":460800})
            - changes only that field of the current settings
        On {"kick":true}
            - sets kick, changes no setting
        On a wrong type ({"port":"6638"}, {"enabled":1}, {"kick":"yes"})
            - returns ESP_ERR_INVALID_ARG
        On invalid JSON or a non-object
            - returns ESP_ERR_INVALID_ARG
    GET /device/usb_serial
        On a call in any mode
            - 200 with the status body
    PUT /device/usb_serial
        On valid fields
            - 200 with the body after the change
        On an invalid port or baud rate
            - 400 invalid_request naming the field; nothing stored
```

---

### Task 1: `usb_serial` pure core

**Files:**
- Create: `p4/components/usb_serial/CMakeLists.txt`, `include/usb_serial_core.h`, `core.c`,
  `test_host/{CMakeLists.txt,main/CMakeLists.txt,main/test_core.c}`

**Interfaces:**
- Produces (all in `usb_serial_core.h`, only `<stdbool.h>`, `<stddef.h>`, `<stdint.h>`):
  - `typedef struct { bool enabled; uint16_t port; uint32_t baud; bool flow_control; } usb_serial_settings_t;`
  - `void usb_serial_settings_default(usb_serial_settings_t *out);`
  - `typedef enum { USB_SERIAL_OK, USB_SERIAL_BAD_PORT, USB_SERIAL_BAD_BAUD } usb_serial_check_t;`
  - `usb_serial_check_t usb_serial_settings_check(const usb_serial_settings_t *s);`
  - `typedef enum { USB_SERIAL_DRV_NONE, USB_SERIAL_DRV_CP210X, USB_SERIAL_DRV_CH34X, USB_SERIAL_DRV_CDC_ACM } usb_serial_driver_t;`
  - `usb_serial_driver_t usb_serial_pick_driver(uint16_t vid, uint16_t pid, uint8_t dev_class, const uint8_t *intf_classes, size_t n_intf);`
  - `const char *usb_serial_driver_name(usb_serial_driver_t d);`
  - `typedef enum { USB_SERIAL_TAKE, USB_SERIAL_REFUSE } usb_serial_accept_t;`
    `usb_serial_accept_t usb_serial_accept(bool client_connected);`
  - `typedef struct { uint8_t *data; size_t cap, head, len; } usb_serial_ring_t;` with
    `void usb_serial_ring_init(usb_serial_ring_t *r, uint8_t *storage, size_t cap);`,
    `bool usb_serial_ring_push(usb_serial_ring_t *r, const uint8_t *src, size_t n);`,
    `size_t usb_serial_ring_take(usb_serial_ring_t *r, uint8_t *out, size_t cap);`,
    `void usb_serial_ring_clear(usb_serial_ring_t *r);` (not thread-safe; the glue locks it)
  - `typedef enum { USB_SERIAL_BAUD_EXACT, USB_SERIAL_BAUD_CLOSE, USB_SERIAL_BAUD_FAR } usb_serial_baud_t;`
    `usb_serial_baud_t usb_serial_baud_verdict(uint32_t requested, uint32_t actual);` (CLOSE within 3 %)
  - `#define USB_SERIAL_CP210X_FLOW_LEN 16` and `void usb_serial_cp210x_flow(bool on, uint8_t out[USB_SERIAL_CP210X_FLOW_LEN]);`
  - Constants: `USB_SERIAL_DEFAULT_PORT 6638`, `USB_SERIAL_DEFAULT_BAUD 115200`, `USB_SERIAL_RING_MIN 8192`.

- [ ] **Step 1: Write the failing host tests** in `test_host/main/test_core.c`, one `TEST_CASE` per effect of the
  skeleton's `usb_serial_core` branch, with the exact values from the skeleton and Global Constraints, e.g.
  `TEST_CASE("On a push that would overflow: the ring is unchanged", "[usb_serial]")` filling 8190 of 8192 bytes,
  pushing 3, asserting `false`, length 8190 and the first and last bytes taken back unchanged; the flow-control cases
  compare all 16 bytes.
- [ ] **Step 2: Run, expect build failure** — `logrun make test-host` (fails: `usb_serial_core.h` missing).
- [ ] **Step 3: Implement** `core.c` and the linux branch of `CMakeLists.txt` (`core.c` only); the target branch lists
  only `core.c` for now and grows in Task 3.
- [ ] **Step 4: Run, expect pass** — `logrun make test-host`.
- [ ] **Step 5: Commit** — `feat(usb_serial): pure core: settings, driver choice, client rule, ring, baud and flow helpers`.

### Task 2: `netcfg` service class and socket check

**Files:**
- Modify: `p4/components/netcfg/include/netcfg_policy.h`, `policy.c`, `include/netcfg.h`, `netcfg.c`,
  `netcfg_glue.h`, `ifaces.c`, `test_host/main/test_policy.c`

**Interfaces:**
- Produces:
  - `NETCFG_SVC_USB_SERIAL` appended to `netcfg_service_t`; `netcfg_policy_allows` allows it on both sides in split
    mode (single mode already allows everything).
  - Pure, in `netcfg_policy.h`: `bool netcfg_mesh_refused(bool on_mesh);` (true when the connection must be refused;
    a one-line rule kept pure so the "every mode" decision is host-tested next to the policy).
  - `netcfg_glue.h`: `bool netcfg_ifaces_socket_side(int fd, netcfg_side_t *side, bool *on_mesh);`
    (`netcfg_ifaces_request_side` becomes a wrapper over it; `on_mesh` is true when the match came from the
    OpenThread netif branch of `match_v6`).
  - `netcfg.h`: `bool netcfg_socket_allowed(int fd, netcfg_service_t svc);` — takes `s.lock` with a 100 ms timeout
    (refuses and logs on timeout), finds the side, refuses mesh addresses in every mode, else applies the policy.
    No 403 body: the caller closes the socket.

- [ ] **Step 1: Write the failing host tests** in `test_policy.c`: three `TEST_CASE`s for `NETCFG_SVC_USB_SERIAL`
  (split/mgmt true, split/thread true, single true) and two for `netcfg_mesh_refused`, named like the existing ones
  ("On a USB serial connection arriving on thread_bb in split mode: it is allowed").
- [ ] **Step 2: Run, expect failure** — `logrun make test-host`.
- [ ] **Step 3: Implement** the policy change, the pure helper, the `ifaces.c` split (behaviour of
  `netcfg_ifaces_request_side` unchanged) and `netcfg_socket_allowed`.
- [ ] **Step 4: Run** — `logrun make test-host`, `logrun make build-test`, `logrun make lint`: all pass.
- [ ] **Step 5: Commit** — `feat(netcfg): USB serial service class and a socket check that refuses the Thread mesh`.

### Task 3: `usb_serial` board glue, budgets, task placement

**Files:**
- Create: `p4/components/usb_serial/idf_component.yml`, `include/usb_serial.h`, `usb_serial_priv.h`, `usb_serial.c`,
  `usb_host_glue.c`, `tcp.c`
- Modify: `p4/components/usb_serial/CMakeLists.txt` (target branch: all sources; `PRIV_REQUIRES` netcfg nvs_flash lwip
  log freertos esp_timer board_cores, plus the USB components), `p4/components/routing/task.c`,
  `p4/components/routing/CMakeLists.txt` (`usb_serial` in `PRIV_REQUIRES`), `p4/sdkconfig.defaults` (both lwIP
  values and their comment), `common/board_cores/include/board_cores.h` (MGMT_CORE list gains `usb_serial`,
  `usb_lib`, `USB-CDC`), `p4/pytest/core_map.py` (the three names in `MANAGEMENT_TASKS`, not in `ALWAYS_PRESENT`),
  `p4/main/app_main.c` (call `usb_serial_init()` after `ot_console_init()`, log and continue on error),
  `p4/main/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1's core, Task 2's `netcfg_socket_allowed`.
- Produces (`usb_serial.h`):
  - `#define USB_SERIAL_SOCKETS 3` (listener, client, one connection being refused)
  - `typedef struct { bool present; uint16_t vid, pid; usb_serial_driver_t driver; bool open; bool flow_on; char error[64]; } usb_serial_dongle_t;`
  - `typedef struct { bool present; char peer[48]; uint32_t since_s; } usb_serial_client_t;`
  - `typedef struct { usb_serial_dongle_t dongle; usb_serial_client_t client; uint64_t bytes_to_dongle, bytes_from_dongle, discarded_from_client; uint32_t overflow_disconnects, refused_connections; } usb_serial_status_t;`
  - `esp_err_t usb_serial_init(void);`
  - `void usb_serial_get_settings(usb_serial_settings_t *out);`
  - `esp_err_t usb_serial_apply(const usb_serial_settings_t *s);` (`ESP_ERR_INVALID_ARG` on a failed check,
    `ESP_ERR_INVALID_STATE` before init, `ESP_ERR_TIMEOUT` if the owner did not confirm within 5 s, NVS errors)
  - `esp_err_t usb_serial_kick(void);`
  - `void usb_serial_get_status(usb_serial_status_t *out);` (a snapshot copied under a short lock)

- [ ] **Step 1: Implement `usb_host_glue.c`.** The USB host is installed from the owner task (core 1) with default
  `peripheral_map`; the `usb_lib` task loops on `usb_host_lib_handle_events` and ends after the all-free flag once a
  stop was requested. `cdc_acm_host_install` with `driver_task_priority` 7, `xCoreID` `MGMT_CORE` and a `new_dev_cb`
  that reads the device and active configuration descriptors, builds the interface class list, calls
  `usb_serial_pick_driver` and posts `{vid, pid, dev_addr, driver}` to the owner (never opens). Open by driver:
  `cp210x_vcp_open`, `ch34x_vcp_open`, or `cdc_acm_host_open_v2` with `dev_addr`; `in_buffer_size` 512,
  `out_buffer_size` 512. The data callback pushes into the dongle → client ring under a `portMUX` spinlock, posts an
  overflow message when the push fails, and returns `true`. The event callback posts a disconnect message. CP210x flow
  control goes through `cdc_acm_host_send_custom_request` (bmRequestType 0x41, bRequest 0x13, wValue 0, wIndex the
  data interface, the 16-byte payload).
- [ ] **Step 2: Implement `tcp.c`.** Listener per Global Constraints; accept, ask `usb_serial_accept` and
  `netcfg_socket_allowed(fd, NETCFG_SVC_USB_SERIAL)`, refuse with linger {1, 0}; client options; a send that flushes
  the coalesced ring with `MSG_DONTWAIT`, keeping what the socket did not take at the head of the ring.
- [ ] **Step 3: Implement `usb_serial.c`.** NVS load/store per Global Constraints; the owner task `usb_serial`
  (priority 6, `MGMT_CORE`, stack the builder's call) and its loop:

  ```
  loop:
      select(listener, client) with a 5 ms timeout
      if listener readable: accept → take or refuse
      if client readable:
          if dongle open: recv up to 512 → data_tx_blocking (100 ms timeout); on failure count as discarded
          else: recv → discard and count
          on recv 0 / error: close client
      drain every queued message (settings, kick, new device, disconnect, overflow, stop) in order
      flush the dongle → client ring if ≥ 1024 bytes or the oldest pending byte is ≥ 5 ms old
  ```

  `usb_serial_apply` validates, stores, posts and waits on a per-call semaphore given by the owner; the owner runs
  start, stop (the shutdown order), port change and serial changes. The status snapshot is updated by the owner only.
- [ ] **Step 4: Budgets and placement.** `SOCKETS_NEEDED` gains `USB_SERIAL_SOCKETS` (and the comment above it), the
  two lwIP values and their comment in `p4/sdkconfig.defaults`, the `board_cores.h` comment, `core_map.py`.
- [ ] **Step 5: Build and lint** — `logrun make build-test`, `logrun make build`, `logrun make test-host`,
  `logrun make lint`: all pass. With a stale `p4/sdkconfig` the build check fails with its existing message; delete
  `p4/sdkconfig` as `docs/development.md` says, then build again.
- [ ] **Step 6: Commit** — `feat(usb_serial): USB host, owner task and TCP listener`.

The on-board checks of this task's skeleton effects are batched into Task 5 (no flashing inside tasks).

### Task 4: `/device/usb_serial` routes

**Files:**
- Create: `p4/components/device_api/usb_serial_json.c`, `usb_serial_json.h`, `test_host/main/test_usb_serial_json.c`
- Modify: `p4/components/device_api/device_api.c` (two handlers after the console ones, two rows in `routes[]`),
  `p4/components/device_api/CMakeLists.txt` (`usb_serial` in `PRIV_REQUIRES`, `usb_serial_json.c` in both branches),
  `test_host/main/CMakeLists.txt` (`test_usb_serial_json.c`, `usb_serial` in `REQUIRES`),
  `p4/components/esp_ot_br_server/src/esp_br_web.c` (`EXTRA_HANDLER_SLOTS` 27 → 29, comment updated),
  `p4/components/PATCHES.md` (entry 25)

**Interfaces:**
- Consumes: Task 1's `usb_serial_settings_t`, `usb_serial_settings_check`, `usb_serial_driver_name`; Task 3's
  `usb_serial_status_t` and board API.
- Produces:
  - `int device_api_usb_serial_json(char *out, size_t cap, const usb_serial_settings_t *s, const usb_serial_status_t *st);`
    (length or -1; vid/pid as 4 lowercase hex digits; field order per Global Constraints)
  - `esp_err_t device_api_usb_serial_parse(const char *body, usb_serial_settings_t *inout, bool *kick);`
    (types only; ranges are `usb_serial_settings_check`'s job)
  - Handlers: GET answers the body; PUT parses onto `usb_serial_get_settings`, checks, then `usb_serial_kick` if asked
    and `usb_serial_apply` if any setting changed. 400 details: `"port must be 1-65535 and not 80 or 179"`,
    `"baud must be one of 38400, 57600, 115200, 230400, 460800, 921600, 1000000, 2000000"`,
    `"send any of enabled, port, baud, flow_control (booleans and numbers) and kick (true)"`; 503 `not_started` on
    `ESP_ERR_INVALID_STATE`, 500 `storage_failed` on an NVS error, 504 `timeout` on `ESP_ERR_TIMEOUT`. The body cap is
    256 bytes (`device_api_length_check`).

- [ ] **Step 1: Write the failing host tests** in `test_usb_serial_json.c`, one per effect of the skeleton's
  `device_api usb_serial` JSON and parse branches; the spec example is compared byte for byte (status with an open
  `10c4:ea60` cp210x, peer `192.168.10.23`, `since_s` 812, counters 12345, 67890, 0, 0, 0).
- [ ] **Step 2: Run, expect failure** — `logrun make test-host`.
- [ ] **Step 3: Implement** the JSON helpers with cJSON (like `console_json.c`), the handlers, the routes, the slot
  count and the PATCHES entry (title, files, reason: two more device API routes, spec reference).
- [ ] **Step 4: Run** — `logrun make test-host`, `logrun make build-test`, `logrun make lint`: all pass.
- [ ] **Step 5: Commit** — `feat(device_api): /device/usb_serial settings and status`.

### Task 5: On-board tests, docs, hardware pass

**Files:**
- Create: `p4/pytest/test_usb_serial.py`, `docs/usb-serial.md`
- Modify: `p4/pytest/pytest.ini` (marker `usb_loopback: needs a USB serial adapter with TX jumpered to RX in the
  Type-A port`), `docs/README.md`, `docs/api.md` (a "USB serial proxy" section after "Thread console"), `mkdocs.yml`,
  `CHANGELOG.md`

- [ ] **Step 1: Write the no-dongle tests** in `test_usb_serial.py` (fixture turns the proxy on and restores the
  defaults afterwards): `test_off_by_default`, `test_put_validates` (each 400 case, nothing stored),
  `test_settings_persist_across_reboot`, `test_second_client_refused`, `test_kick_frees_the_port`,
  `test_port_change_moves_listener`, `test_enable_disable_cycles` (100 cycles; `uptime_s` in `/device/diag` keeps growing),
  `test_bytes_without_dongle_discarded` (send 100 bytes, `discarded_from_client` grows by 100).
- [ ] **Step 2: Write the loopback tests**, marked `usb_loopback`: `test_loopback_integrity` (5 min each at 115200,
  460800, 921600, random data, zero mismatches, while a background thread loads `/device/status` and a
  `/device/sniffer.pcap` stream runs), `test_stale_bytes_discarded` (bytes sent before the adapter is attached never
  echo), `test_slow_reader_disconnected` (stop reading for 10 s at 921600: the connection closes and
  `overflow_disconnects` grows, no partial garbage before the close), `test_disable_while_unplugged` (manual unplug
  prompt, run with `-s`).
- [ ] **Step 3: Docs** — `docs/usb-serial.md` (what it is, experimental, the warning that anyone who reaches the port
  controls the radio, ZHA `socket://`, Zigbee2MQTT `tcp://` with `zstack`, `otbr-agent` through `socat`, the SLZB-06
  notes: USB mode, 115200, no flow control, back up its network first), the `docs/api.md` section (routes, body,
  errors), index and nav entries, `CHANGELOG.md` entry under Unreleased.
- [ ] **Step 4: Full checks** — `logrun make test`, `logrun make build`, `logrun make lint`: all pass.
- [ ] **Step 5: Commit** — `test, docs(usb_serial): on-board tests and docs`.
- [ ] **Step 6: Hardware pass** (one flash, after Tasks 1–5 are reviewed and fixed): check `/device/status`
  `app.version` against the last build put on the board in its own step, then `logrun make build-test`,
  `logrun make flash`, `logrun make test-target TEST=p4/pytest/test_usb_serial.py`, then with the loopback adapter
  `logrun make test-target TEST=p4/pytest/test_usb_serial.py MARK=usb_loopback`. Then the manual steps with the user,
  in the spec's order:
  1. SLZB-06 (USB mode, Zigbee firmware) on the Type-A port: it enumerates as `cp210x`; its uptime on its own page
     does not reset when the port opens; 10 hot-plugs on PoE power, `uptime_s` and `reset_reason` in `/device/diag` show no reset.
  2. The SLZB-06's Zigbee network backed up (zigpy-znp backup) or confirmed disposable, then ZHA or Zigbee2MQTT through
     the proxy forms a network and a device joins.
  3. Unplug and replug mid-session; kill and restart the client; a second client is refused.
  4. 30 minutes of Zigbee traffic against a 30-minute baseline without the dongle (same REST load in both), Zigbee and
     Thread channels at least 5 apart, the dongle on an extension cable: C6 role, `/device/diag` OpenThread lock
     timings, TX retries, CCA failures and ping loss to a Thread device.
  5. Optional: a connection from a Thread node's mesh address is refused; the spinel check with the SLZB-06 on its
     Thread firmware (`otbr-agent` through `socat`).
  Record the results in the memory file for this feature, including anything left unverified.
