# USB/IP server: the USB-A port exported to a remote host: design

Status: draft for review, 2026-11-10.

Marks used below: *(my call)* is a choice made while writing this spec, not one the user made; everything else not so
marked was decided with the user.

## 1. Purpose and scope

The board's USB-A port (the P4's high-speed OTG peripheral) can hold one USB device. Today the board only uses it for
radio dongles it has a driver for (cp210x, ch34x, CDC-ACM) through `radio_mux`. USB/IP turns the port into a **remote
USB port**: the device shows up on another machine as if it were plugged in there, and that machine's own drivers run
it. The board needs no driver for the device.

Uses, both of which the design serves:

- **Long-lived devices** the board has no driver for: FTDI or PL2303 serial bridges, Bluetooth adapters, Z-Wave
  sticks, HID devices such as IR blasters or a UPS. Attached for weeks.
- **Short jobs**, mainly reflashing a radio dongle from a PC through the board. The dongle may reset or re-enumerate
  mid-job (bootloader entry).

Clients: Linux `usbip` (the kernel's `vhci-hcd` and the `usbip` tool) is the main client; Windows `usbip-win2` gets
one check *(my call)*. Home Assistant OS as a client is not a requirement.

Goals:

- Export whatever single device is on the USB-A port, under a stable bus id `1-1`, to one client at a time over the
  standard USB/IP protocol (TCP 3240), so the stock clients work without patches.
- Control, bulk and interrupt transfers, with unlink (cancel) support, so the client's drivers behave as with a local
  device.
- A device that drops off USB ends the session; whatever comes back (the same device or a different one) is exported
  again under `1-1`, so attaching again with the same command picks it up.
- A lower-level feature outside `radio_mux`: when USB/IP is on, it owns the USB port and USB channels are unavailable.
- Bounded memory: no client can make the board run out of internal RAM.

Non-goals:

- Isochronous transfers (audio, webcams, Bluetooth SCO voice). Devices that have isochronous endpoints are still
  exported; only transfers to those endpoints fail (section 6.6).
- Hubs, and more than one device. The board has no transaction translator support and the user decided hubs are not
  worth it.
- More than one client at a time.
- **Re-mapping**: keeping a client's session alive across a device reset that comes back with identical descriptors.
  Discussed and deferred by the user ("we can implement it later if needed"); see section 13.
- Authentication, encryption, or a client allowlist. Access is limited by the VLAN only (section 7).
- Choosing the listening interface. It is fixed (section 7).
- Switching USB/IP on or off without a reboot.
- Prometheus metrics for USB/IP *(my call: the status endpoint carries the counters; metrics can follow)*.

Anti-goals (a failure even if it works):

- USB/IP disturbing the C6 border router: OpenThread timing, the web server, TREL, ping loss on the mesh.
- A client exhausting the board's internal RAM, or a stuck device or client holding the board until a reboot.
- A client being able to brick the session for the next client (a session must always end cleanly, leaving the port
  ready for the next import).
- A Thread mesh device, or a web page in a LAN browser, being able to drive the device.
- Enabling USB/IP silently taking Thread or Zigbee away from a USB radio that runs them.

Success: on Linux, `usbip list -r <board>` shows the device on the port, `usbip attach -r <board> -b 1-1` makes it
appear locally, and its stock kernel driver works: a CP2102N or CH340 serial bridge passes a loopback or a zigpy
session through `/dev/ttyUSB*`, and a Bluetooth adapter comes up with `bluetoothctl` and scans. A dongle reflash with
`cc2538-bsl` through the attached serial port completes. Unplugging, killing the client and detaching all leave the
port ready for the next attach without a reboot. The C6 border router shows no change against its baseline.

## 2. Facts this design rests on

- esp-usb 1.6.0 (`espressif/usb`, pinned in `p4/components/radio_mux/idf_component.yml`) gives raw device access:
  `usb_host_client_register`, `usb_host_device_open`, `usb_host_interface_claim(dev, iface, alt)`,
  `usb_host_transfer_alloc` / `usb_host_transfer_submit` / `usb_host_transfer_submit_control`,
  `usb_host_endpoint_halt` / `_flush` / `_clear`, `usb_host_lib_set_root_port_power`.
- Transfers are asynchronous only. There are **no transfer timeouts** (`timeout_ms` is unsupported; `TIMED_OUT` is
  never reported) and **no per-transfer cancel**: the only way to cancel is to halt the endpoint, flush it (every
  queued transfer completes as canceled) and clear it.
- Halt, flush and clear refuse **EP0** (`check_ep_addr` in `usbh.c`). A control transfer the device keeps NAKing stays
  pending forever; only turning the root port off ends it.
- A flushed transfer reports `actual_num_bytes = 0` even if it was already in hardware (up to 2 per pipe) and had
  moved data (`hcd_dwc.c`). The board cannot tell a transfer that started from one that did not.
- The host side's data toggle of a bulk or interrupt endpoint is reset to DATA0 only when its channel is allocated,
  i.e. when the interface is claimed. `usb_host_endpoint_clear` only makes the pipe active again.
- After a STALL or an error the pipe halts and its queue waits until it is cleared.
- `usb_host_interface_claim` sends no SET_INTERFACE to the device. Per-endpoint queues have no fixed length.
- `usb_host_transfer_submit_control` does not filter requests: SET_CONFIGURATION, SET_INTERFACE and CLEAR_FEATURE
  would reach the device unchanged, behind the host stack's back (section 6.4 says what the board does instead).
- esp-usb enumerates the device and selects **one configuration** itself. Endpoints exist on the host side only for
  interfaces claimed, at the alternate setting claimed.
- `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE` (default 256, not set in `p4/sdkconfig.defaults`) sizes the per-device
  buffer esp-usb uses for enumeration, and **enumeration fails** (`ESP_ERR_INVALID_SIZE` in `enum.c`) for a device whose
  configuration descriptor is longer. Such a device never reaches any client. Transfers the board allocates itself are
  not limited by it. This design raises it to 1024 *(my call: composite devices and some Bluetooth adapters exceed
  256; one device, so about 768 bytes more internal RAM, also with USB/IP off)*.
- DMA buffers are internal RAM unless `CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM` is set (off in this build; it
  would also move the CDC drivers' buffers). The board has 32 MB PSRAM; internal RAM is tight.
- The P4 OTG controller has 16 host channels: EP0 plus at most 15 other endpoints at a time.
- A low- or full-speed device directly on the root port works without a transaction translator.
- Today `radio_mux_start()` (`p4/main/app_main.c`) calls `usb_transport_start()` (`radio_mux/usb_transport.c`), which
  installs the USB host from a task pinned to `NET_CORE` (interrupt latency: hardware 2026-11-07) and never
  uninstalls it. That is why ownership is a boot-time choice (section 3).
- The USB/IP protocol (Linux `Documentation/usb/usbip_protocol.rst`, version 0x0111): `OP_REQ_DEVLIST`,
  `OP_REQ_IMPORT`, then `USBIP_CMD_SUBMIT` / `USBIP_RET_SUBMIT` and `USBIP_CMD_UNLINK` / `USBIP_RET_UNLINK` on the same
  TCP connection. Headers are big-endian; the setup packet is carried as-is. Every client message starts with the
  version bytes `0x01 0x11` (OP) or a 32-bit command `0x00000001`/`0x00000002`; never with a letter.
- Linux's own server (`usbip-host`, `stub_rx.c`) intercepts the same standard requests this design does
  (SET_CONFIGURATION, SET_INTERFACE, CLEAR_FEATURE(ENDPOINT_HALT), port reset), and resets the device when a session
  ends.

## 3. Ownership of the USB port

Exactly one owner of the USB host stack per boot:

- **USB/IP off** (default): `radio_mux`'s USB transport installs the host as today. Nothing changes.
- **USB/IP on**: a new component `usbip` installs the USB host (same rules as `usb_transport.c`: from a task pinned to
  `NET_CORE`, never uninstalled) and registers its own USB host client. `radio_mux` is told before
  `radio_mux_start()` that the USB port belongs to someone else, with a reason string; it then does not start its USB
  transport, does not install the CDC driver, and creates no channel on plug-in.

The setting `usbip.enabled` is stored in NVS (namespace `usbip`) *(my call: its own component and namespace, like the
other features)*. Changing it reboots the board through `reboot_gate`, like a network mode change. The choice is made
once at boot, in `app_main`, before `radio_mux_start()`.

Boot conflict *(my call)*: if USB/IP is stored on but a USB channel's stored mode is `otbr` or `ziggurat` (only
possible through a race with the enable check or an NVS restore), Thread and Zigbee win: USB/IP stays off for this
boot, `radio_mux` owns the port, and `GET /device/usbip` shows `state: "blocked"` with the channel id as the reason.

## 4. Enable and disable rules

`PUT /device/usbip {"enabled": true|false}`:

| Request | Condition | Answer |
| --- | --- | --- |
| enable | a USB channel's stored mode is `otbr` or `ziggurat` (attached or not) | 409 `usb_channel_in_use`, naming the channel and its mode; nothing changes |
| enable | a long operation runs (a Thread move, an update, a firmware push) | 409 `busy` (existing rule) |
| enable | otherwise | 200 `{"reboot": true}`, stored, the board reboots |
| disable | always | 200 `{"reboot": true}`, stored; an attached client is dropped and the board reboots |
| same value as stored | | 200 `{"reboot": false}`, nothing happens |
| anything else in the body | | 400 `bad_value` |

The user moves Thread to the C6, or sets the USB channel `off`, before enabling. A USB channel in `rcp-proxy` does not
block enabling *(my call: the proxy holds no state the board owns; it simply stops serving after the reboot)*.

While USB/IP is on:

- Stored USB channels stay listed in `GET /device/channels` with mode `unavailable`, `channel_reason` "USB is exported
  over USB/IP". Their stored modes and settings are kept and apply again once USB/IP is turned off.
- A device plugged in creates no channel and is not probed.
- `PUT /device/channels/{usb-id}` that changes the mode is refused with 409 `usbip_enabled`. Line and proxy settings
  can still be changed (as for any `unavailable` channel). `DELETE` (forget) works as today.
- The C6 channel is not affected in any way.

## 5. Export lifecycle

The `usbip` component tracks one device and at most one session.

Device states (the `state` field of the status):

| State | Meaning |
| --- | --- |
| `off` | USB/IP is not enabled this boot |
| `blocked` | stored on, but a USB channel holds `otbr`/`ziggurat` (section 3) |
| `no_device` | nothing on the port |
| `unsupported` | a device is on the port but cannot be exported, with a reason: a hub, more than 15 non-control endpoints in the active configuration, the device failed to open |
| `exportable` | the device is opened, its interfaces claimed at alternate setting 0, no session |
| `attached` | a client imported it |

Rules:

- On a new device: open it, claim every interface of the active configuration at alternate setting 0, and become
  `exportable`. Failing any of that is `unsupported` with the reason.
- **Session end** (client detaches or disconnects, keepalive fails, protocol error, device gone, a stuck control
  transfer): every non-control endpoint is halted, flushed and cleared; if a control transfer is still pending, the
  root port is turned off first, which ends it (EP0 cannot be flushed); then interfaces are released and the device
  closed, and the board **resets the device by turning the root port's power off and on**
  (`usb_host_lib_set_root_port_power`). Power on enumerates the device again (with a bus reset), so the next import
  gets it in its default state, as with Linux's own server. Whether VBUS is physically cut depends on the board's
  wiring; it does not matter for the reset.
- **Device gone during a session**: the session ends at once (the TCP connection is closed, so the client's kernel
  sees an unplug). When something is plugged in again, or the device comes back after its own reset or in
  bootloader mode with a new product id, it is exported again as `1-1`; the client attaches again. No session is
  carried across a device change (re-mapping is deferred, section 13).
- The bus id is always `1-1`, `busnum` 1, `devnum` the device's USB address.

## 6. Protocol subset and its mapping to esp-usb

### 6.1 Connections

- One TCP listener on port 3240 (section 7 says where).
- A new connection must send its first byte within 10 s and must start with `0x01` (an OP request) *(my call)*;
  anything else, notably an HTTP request from a browser (`G`, `P`, ...), is refused and counted, without a reply.
- `OP_REQ_DEVLIST` is answered on any connection, also while a session runs, and the connection is closed after the
  reply, as the protocol says.
- `OP_REQ_IMPORT` with bus id `1-1`:
  - device `exportable` → `OP_REP_IMPORT` status 0 with the device record; the connection becomes the session.
  - a session already runs → status 2 (`ST_DEV_BUSY`), connection closed.
  - no device, `unsupported`, or another bus id → status 4 (`ST_NODEV`), connection closed.
- Any other OP code, or a version other than 0x0111, closes the connection.
- TCP keepalive on the session (idle 10 s, interval 5 s, 3 probes *(my call)*) so a dead client frees the device.

### 6.2 Device record (`OP_REP_DEVLIST`, `OP_REP_IMPORT`)

Built from the device and active configuration descriptors esp-usb holds: path
`/sys/devices/platform/esp32p4-otg/usb1/1-1` *(my call; clients only print it)*, bus id `1-1`, busnum 1, devnum =
address, speed (low 1, full 2, high 3, as Linux's `enum usb_device_speed`), idVendor, idProduct, bcdDevice, device
class/subclass/protocol, bConfigurationValue, bNumConfigurations, bNumInterfaces, and for DEVLIST each interface's
class/subclass/protocol. An empty port answers DEVLIST with zero devices. An `unsupported` device is not listed.

### 6.3 `USBIP_CMD_SUBMIT`

Each SUBMIT becomes one esp-usb transfer; its `RET_SUBMIT` carries the same seqnum.

- `devid` must be `(1 << 16) | devnum`; otherwise `RET_SUBMIT` with `-ENODEV`.
- **EP0 (control)**: the 8-byte setup packet goes first in the transfer buffer, followed by the OUT data. Standard
  requests in section 6.4 are handled on the board; everything else is forwarded. Data stage up to 4096 bytes *(my
  call: covers descriptor reads of large composite devices and DFU blocks)*; more is `-EINVAL`.
- **Bulk and interrupt**: submitted on the endpoint's queue in arrival order. An **OUT** endpoint has at most one
  transfer in flight; the rest wait in its FIFO (8.1) *(my call: an unlink then never cancels an OUT transfer other
  than the unlinked one, so no OUT data is ever sent twice or dropped behind the client's back, which matters for
  flashing; a 16 KB transfer at a time is plenty for serial and flashers)*. **IN** endpoints may have several in
  flight. After a STALL or error completion the board clears the endpoint so its queue moves on, after sending that
  transfer's `RET_SUBMIT` *(my call)*. IN buffers are rounded up to a multiple
  of the endpoint's max packet size (esp-usb requires it); `actual_length` reports what came. `URB_ZERO_PACKET` maps to
  `USB_TRANSFER_FLAG_ZERO_PACK`. A transfer above 16 KB is `-EINVAL` *(my call: splitting large URBs is future work;
  serial, Bluetooth, HID and flashers stay far below it)*.
- **Isochronous** endpoints, and endpoints not on a claimed interface/alt setting: `-EINVAL` at once (section 6.6).
- Status mapping from esp-usb to the Linux errno in `RET_SUBMIT`:

| esp-usb status | RET_SUBMIT status |
| --- | --- |
| `COMPLETED` | 0; `-EREMOTEIO` if the IN was short and `URB_SHORT_NOT_OK` was set |
| `STALL` | `-EPIPE` |
| `OVERFLOW` | `-EOVERFLOW` |
| `ERROR` | `-EPROTO` |
| `NO_DEVICE` | `-ENODEV`, then the session ends (section 5) |
| `CANCELED` | none for an unlinked transfer (its answer is the `RET_UNLINK`); collateral transfers are resubmitted (section 6.5) |

### 6.4 Standard requests handled on the board

Forwarding these unchanged would put the device and the host stack out of step (endpoint toggles, claimed alternate
settings), so the board handles them, as Linux's server does:

| Request | What the board does | Answer |
| --- | --- | --- |
| SET_CONFIGURATION, same value as active | nothing is sent to the device | 0 |
| SET_CONFIGURATION, other value | refused (esp-usb holds one configuration) | `-EPIPE` |
| SET_INTERFACE(iface, alt) | cancel the interface's pending transfers (`-ECONNRESET`), release the interface, forward the request, claim it again at `alt` | the forwarded request's status; on failure the interface is claimed again at its previous alt |
| CLEAR_FEATURE(ENDPOINT_HALT, ep) | forward it (the device resets its toggle to DATA0); then reset the host side's toggle the only way esp-usb allows: cancel the interface's endpoints, release the interface and claim it again at its current alt, and resubmit the canceled transfers of the *other* endpoints in order *(my call: halts are rare, after a stall; the resubmitted IN transfers share the data-loss window of 6.5)*. Transfers pending on `ep` itself get `-ECONNRESET`, as Linux does when clearing a halt | the forwarded status |
| SET_ADDRESS | refused | `-EPIPE` |
| port reset (`SET_FEATURE(PORT_RESET)` to the root hub) | not carried out; acknowledged *(my call: a reset re-enumerates the device, which ends the session; the board cannot reset in place)* | 0 |

A SET_INTERFACE whose new alternate setting would exceed the 15 endpoint channels, or contains isochronous endpoints
only, is still forwarded and claimed; transfers to iso endpoints fail per 6.6.

### 6.5 `USBIP_CMD_UNLINK`

Clients unlink when their own URB times out or a driver cancels reads (closing a tty, for example). esp-usb cannot
cancel one transfer, so the board cancels the endpoint's queue and puts the others back:

```
on UNLINK(seqnum S):
    T = transfer with seqnum S
    if T is not known (completed, its RET_SUBMIT sent or queued): RET_UNLINK status 0
    else if T still waits in its endpoint's PSRAM FIFO:
        remove it; RET_UNLINK status -ECONNRESET; no RET_SUBMIT for S
    else if T is on EP0 (cannot be flushed):
        wait up to 2 s for T to complete
        completed: send its RET_SUBMIT, then RET_UNLINK status 0
        still pending: the device is stuck; end the session (section 5), which the client sees as an unplug
    else:
        E = T's endpoint
        halt E; flush E; clear E             # every transfer in flight on E completes as CANCELED
        RET_UNLINK(S) status -ECONNRESET; no RET_SUBMIT for S
        resubmit E's other canceled transfers in their original order, ahead of E's FIFO
```

What this costs *(my call)*: on an OUT endpoint, T is the only transfer in flight (6.3), so nothing else is touched.
On an IN endpoint, a resubmitted transfer that was already in hardware may have received part of a packet stream
before the flush; esp-usb reports 0 bytes for it, so that data is lost. This only happens when a client unlinks one of
several pending reads on the same endpoint; drivers that close a device unlink all of them, and then nothing is
resubmitted. The 2 s for EP0 is below Linux's 5 s control timeout, so a stuck control request ends the session soon
after the client gives up on it.

Checked on hardware: that halt/flush/clear leaves the endpoint's data toggle in step with the device (the host
channel keeps its toggle across a flush; nothing in esp-usb resets it). If it does not, the unlink re-claims the
interface the way CLEAR_FEATURE does (6.4).

### 6.6 What is not supported, and how it shows

- Isochronous endpoints: the device is exported; `SUBMIT` to an iso endpoint answers `-EINVAL` at once. A Bluetooth
  adapter therefore works for data (ACL, BLE) but not for SCO voice.
- Hubs: `unsupported` ("hubs are not supported"), not listed.
- A configuration other than the one esp-usb chose: `SET_CONFIGURATION` answers `-EPIPE`.

## 7. Network scope and security

- TCP 3240 listens on the single interface in `single` mode, and on `mgmt` only in `split` mode. Not configurable.
  The user's decision: access is limited by the VLAN; there is no allowlist and no authentication.
- A new `netcfg` service `NETCFG_SVC_USBIP` carries this rule, and the RCP proxy's protections apply: the Thread-mesh
  refusal (a connection whose source is a mesh address, or that arrived from the Thread side, is refused) and, in
  split mode, the route-back check.
- The first-byte check (6.1) stops a web page in a LAN browser from talking to the port: browsers can open TCP to 3240
  but only send HTTP, which starts with a letter.
- Documented warning: anyone on the management VLAN can take the device and do anything with it, including flashing
  it. Turn USB/IP on only on a trusted management network.
- The service is not announced over mDNS *(my call)*.

## 8. Memory and concurrency budget

### 8.1 Memory

DMA buffers live in PSRAM (`CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM`; the P4's USB DMA reaches it) *(user's
call 2026-11-10, after the hardware pass: it reverses the earlier "internal RAM" call. The setting is global, so the
USB serial radio channels' buffers move too; esp-usb rounds every buffer to the cache line and cache-syncs it, as it
already did for the P4's cached internal RAM)*. One transfer may carry up to 120 KB (`USBIP_XFER_MAX`), Linux
`usb-storage`'s default read size, so USB flash drives work without client setup; esp-usb's per-transfer limit is
130048 bytes on the worst endpoint. A client can queue many URBs (Linux `cdc_acm` keeps 16 reads pending,
`btusb` keeps several bulk and interrupt reads pending forever), so the board bounds what is in flight and never lets
one endpoint's pending reads starve another endpoint:

- **In-flight transfer buffers** (PSRAM, DMA): IN endpoints share at most 256 KB (`USBIP_INFLIGHT_BYTES`) and 32
  transfers. All buffers in flight together stay at or below 408 KB (`USBIP_INFLIGHT_HARD` = 256 KB IN shared + one
  120 KB transfer + 16 × 2 KB small reserves) *(review fix: the earlier unbounded per-endpoint reserve could reach
  about 244 KB; raised from 32 / 16 / 80 KB when the buffers moved to PSRAM)*.
- **Small reserve**: every endpoint, EP0 included, is always granted a first transfer of up to 2 KB
  (`USBIP_RESERVE_SMALL`), whatever else is in flight. This is what keeps a Bluetooth adapter's always-pending
  bulk/interrupt reads from blocking the HCI command on EP0 or the bulk OUT.
- **Large reserve**: a larger first transfer (OUT, EP0, or an IN that does not fit the shared 256 KB) is granted
  while the *charged* bytes stay at or below 408 KB. Charged bytes count each endpoint as max(its bytes in flight,
  2 KB), so an idle endpoint's small reserve is always kept free. Shared and small grants alone stay at or below
  288 KB, so one 120 KB OUT or EP0 transfer always fits once no other large transfer is in flight; otherwise it waits in its FIFO and
  goes when a completion frees the bytes (every completion drains the FIFOs).
- **Heap floor**: the USB side refuses a DMA buffer (the client gets `-ENOMEM`) that would leave less than 1 MB of
  DMA-capable PSRAM free (`USBIP_PSRAM_FREE_FLOOR`).
- **Reply ring**: replies the socket does not take wait in a 768 KB PSRAM ring, which holds every IN completion that
  can be in flight (checked at compile time), so a burst never reads as a client that does not read.
- **Waiting URBs**: a SUBMIT that does not fit waits in a per-endpoint FIFO in PSRAM (its header and OUT data) and is
  submitted, in order, when that endpoint gets a slot. The socket is always read, so an UNLINK is never stuck behind
  a SUBMIT.
- **PSRAM cap** on waiting URBs: 1 MB per session *(my call)*. A client that exceeds it is a client the board cannot
  serve (mass storage, for example); the session ends with a logged reason, counted.

```
slot granted to SUBMIT for endpoint E with size N when:
    E has no transfer in flight and N <= 2 KB                                    # small reserve
    or (E is IN and bytes in flight + N <= 256 KB and transfers in flight < 32)  # shared; OUT: one at a time
    or (E has no transfer in flight and charged bytes after the grant <= 408 KB) # large reserve
otherwise: append to E's PSRAM FIFO
on each completion on E: free its slot, then grant slots to E's FIFO head first, then other FIFOs round-robin
```

- Every transfer buffer allocated for a session is freed when the session ends; the status shows the high-water mark
  of in-flight bytes and waiting bytes.

### 8.2 Tasks

- `usb_lib` task: installs the host and handles host library events, pinned to `NET_CORE` (as in `radio_mux`).
- `usbip` USB task: registers the USB host client and handles its events (new device, device gone, transfer
  completions). Transfer callbacks run here; they never touch sockets, they queue the completion for the network side.
- `usbip` network task: accepts connections, reads requests, submits transfers, writes replies. It does all socket I/O.
  Only this task submits transfers and changes endpoint state (halt/flush/clear/claim), so the unlink and
  SET_INTERFACE sequences need no lock against each other.
- Neither runs on the OpenThread task, and nothing on the OpenThread or esp_timer tasks calls into `usbip` (styleguide
  rule on lwIP).
- Stack sizes, priorities and the network task's core are the builder's call; the hardware pass checks the C6 border
  router's ping loss against its baseline under a bulk transfer load.

## 9. API

New endpoint, in `device_api` (`/device/*`, so it follows the same access rules as the rest of the device API).

`GET /device/usbip`:

```
{"enabled": true, "active": true, "pending_reboot": false,
 "state": "attached", "reason": null,
 "port": 3240, "interface": "mgmt",
 "device": {"busid": "1-1", "vid": "10c4", "pid": "ea60", "bcd_device": "0100", "class": "00",
            "speed": "full", "manufacturer": "Silicon Labs", "product": "CP2102N USB to UART Bridge Controller",
            "serial": "abc123", "interfaces": 1, "endpoints": 2},
 "client": {"addr": "192.168.10.20:51322", "since": 1762790000},
 "counters": {"sessions": 3, "refused": 1, "submits": 12000, "completed": 11950, "errors": 2, "unlinks": 48,
              "bytes_in": 1048576, "bytes_out": 65536, "sessions_ended_overload": 0},
 "memory": {"inflight_bytes": 4096, "inflight_peak": 16384, "waiting_bytes": 0, "waiting_peak": 2048},
 "last_error": null}
```

- `enabled` is the stored value; `active` whether USB/IP owns the port this boot; `pending_reboot` when they differ.
- `device` is `null` without a device; `client` is `null` without a session. `reason` explains `blocked` and
  `unsupported`. `interface` is `single` or `mgmt`.
- A start that failed at boot *(review fix)*: `state` `off`, `reason` and `last_error`
  `"USB/IP failed to start: <error>"`, `pending_reboot` `false` while it stays on (the port stays taken from the radio
  mux until the next reboot).
- Counters reset at boot.

`PUT /device/usbip {"enabled": bool}`: section 4. A client can be dropped with
`PUT /device/usbip {"kick": true}` *(my call: same idea as the RCP proxy's kick, for a client that holds the device
from a machine you cannot reach)*; not stored; 200 also without a session.

Changes elsewhere: `GET /device/channels` USB channels while USB/IP is on (section 4); `PUT /device/channels/{usb-id}`
409 `usbip_enabled`.

## 10. Web UI

A new page **USB/IP** (`usbip.js`, in the navigation next to Radios), following the existing pages' style:

- An on/off switch. Turning it either way asks for confirmation ("The board reboots"), then shows the reboot like the
  Ethernet page does. A 409 `usb_channel_in_use` is shown with a link to the Radios page and what to do.
- The device card: product, ids, speed, state, and the reason when `unsupported` or `blocked`.
- The client card: address and since when, with a Kick button.
- Counters and the memory peaks.
- A short "How to attach" box with the client commands, filled with the board's address:
  `usbip list -r <addr>`, `usbip attach -r <addr> -b 1-1`, and a pointer to usbip-win2 for Windows.

The Radios page: while USB/IP is on, the USB channel rows show the reason with a link to the USB/IP page, and the mode
selector is disabled for them.

Mockup: [2026-11-10-usbip-page-mockup.html](2026-11-10-usbip-page-mockup.html) (all states, the reboot dialog and the
409; open it in a browser).

## 11. Documentation to update

- New `docs/usbip.md`: what it is, the security warning, enabling (and the 409), attaching from Linux (`modprobe
  vhci-hcd`, list, attach, detach, and a sample retry loop or systemd unit for long-lived devices) and Windows
  (usbip-win2), what is not supported (iso, hubs, one client, one configuration, URBs over 16 KB), what happens on
  unplug and on a device reset. Indexed in `docs/README.md` and `mkdocs.yml`.
- `docs/radios.md`: a section saying USB/IP takes the USB port, USB channels become `unavailable` with their modes
  kept, and the 409s.
- `docs/api.md`: `/device/usbip` (GET, PUT) and the new channel error.
- `docs/networking.md`: TCP 3240 on the single interface or `mgmt`, and the mesh refusal.
- `CHANGELOG.md`.
- `STYLEGUIDE.md`: add the `usbip_` prefix to the naming list.

## 12. Testing

Host tests (`p4/components/usbip/test_host/`, Unity on the `linux` target), with an ops seam over esp-usb, cover the
parts with logic: the protocol codec, the standard-request decision table, the unlink sequence, the slot allocator,
the status mapping, the enable rules. Target tests in `p4/pytest/test_usbip.py`. Per the hardware budget, nothing runs
on the board inside implementation tasks; one lean hardware pass after review and fixes, one P4 app flash.

Hardware pass checklist (user present; the aux C6 is not needed):

1. No device: enable, reboot, status `no_device`, `usbip list` shows nothing; the 409 with a USB channel in `otbr`;
   disable restores the USB channel's mode.
2. A CP2102N or CH340 dongle (OllyZig): attach from Linux, zigpy-znp backup through `/dev/ttyUSB*` (as in the serial
   proxy PoC), close and reopen the tty (unlinks), detach, attach again.
3. Unplug mid-session; replug; attach again. Kill the client machine's link (keepalive frees it).
4. A Bluetooth adapter: `bluetoothctl` power on and scan for 60 s.
5. A reflash with `cc2538-bsl` through the attached port (bootloader entry by hand on the OllyZig).
6. Memory peaks and C6 border router baseline (ping loss, role) during item 2 under load.
7. One attach from Windows usbip-win2.
8. Whether the root port power cycle re-enumerates the device (section 5), the toggle check after unlink (6.5), and a
   device with a configuration descriptor over 256 bytes enumerating with the raised limit.

RF rule: any network formed in these tests uses channel 26 (Thread) or 20 (Zigbee), never 11, 15 or 25.

## 13. Future work

- **Re-mapping**: keep a session alive when the device drops and returns with identical descriptors within a short
  window (reopen, replay SET_CONFIGURATION/SET_INTERFACE, fail the transfers in flight), behind a flag. Deferred by the
  user.
- Splitting URBs over 16 KB.
- A client allowlist, mDNS announcement, Prometheus counters.

## 14. Behaviour contract (BDD skeleton)

```
usbip (settings)
    usbip_set_enabled
        On enable with no USB channel in otbr or ziggurat
            - stores enabled = true
            - answers reboot = true and the board reboots
        On enable while a USB channel's stored mode is otbr or ziggurat
            - answers 409 usb_channel_in_use naming the channel and mode
            - stores nothing
        On enable while a USB channel runs rcp-proxy
            - stores enabled = true and answers reboot = true
        On enable during a long operation
            - answers 409 busy
        On disable
            - stores enabled = false, drops the client, answers reboot = true
        On the stored value again
            - answers reboot = false and does not reboot

usbip (boot)
    usbip_boot
        When enabled and no USB channel holds otbr or ziggurat
            - installs the USB host and its client; radio_mux does not start its USB transport
            - USB channels are listed unavailable with "USB is exported over USB/IP"
        When enabled but a USB channel holds otbr or ziggurat
            - radio_mux owns the port; state is blocked with the channel as reason
        When disabled
            - radio_mux owns the port as before; state is off

radio_mux (while USB/IP owns the port)
    PUT /device/channels/{usb-id}
        On a mode change
            - answers 409 usbip_enabled
        On a line or proxy setting
            - stores it as for any unavailable channel
    on device plug-in
        - creates no channel and probes nothing

usbip (device)
    on new device
        When it opens and every interface claims at alt 0
            - state becomes exportable with the device record
        When it is a hub or needs more than 15 endpoints or fails to open
            - state becomes unsupported with the reason; DEVLIST does not list it
    on device gone
        During a session
            - closes the session's connection and frees its buffers
            - state becomes no_device
        After it returns (same or different device)
            - is exported again as 1-1

usbip (connections)
    on accept
        When the first byte is 0x01 within 10 s
            - reads the OP request
        When the first byte is anything else, or none comes
            - closes without reply and counts refused
        When the source is a Thread mesh address or the wrong interface
            - refuses the connection
    OP_REQ_DEVLIST
        - answers one device record, or zero without an exportable device, then closes
        - works while a session runs
    OP_REQ_IMPORT
        When 1-1 is exportable and no session runs
            - answers status 0 with the record and starts the session
        When a session runs
            - answers ST_DEV_BUSY and closes
        When there is no device or the bus id is not 1-1
            - answers ST_NODEV and closes
    on session end (detach, disconnect, keepalive, protocol error, kick, stuck control transfer)
        - halts, flushes and clears every non-control endpoint and releases the interfaces
        - turns the root port off first when a control transfer is still pending
        - power-cycles the root port so the device re-enumerates
        - state returns to exportable once the device is back

usbip (transfers)
    CMD_SUBMIT
        On a bulk or interrupt transfer within budget
            - submits it on the endpoint in arrival order
            - answers RET_SUBMIT with the mapped status, actual_length and IN data
        On an OUT endpoint that already has a transfer in flight
            - queues it in the endpoint's FIFO until the first completes
        On a STALL or error completion
            - answers its RET_SUBMIT, then clears the endpoint so its queue continues
        On a short IN with URB_SHORT_NOT_OK
            - answers -EREMOTEIO
        On a transfer above 16 KB, an iso endpoint, or an endpoint not claimed
            - answers -EINVAL at once
        On a wrong devid
            - answers -ENODEV
        When the budget is full
            - queues it in the endpoint's PSRAM FIFO and submits it in order when a slot frees
        When an endpoint has nothing in flight and the transfer is at most 2 KB
            - grants it a slot even when the shared budget is used up
        When an endpoint has nothing in flight and the transfer is larger
            - grants it while the charged bytes stay at or below 80 KB
            - otherwise queues it and submits it when a completion frees the bytes
        When a DMA buffer would leave less than 48 KB of internal DMA memory free
            - answers -ENOMEM
        When waiting data exceeds 1 MB
            - ends the session and counts sessions_ended_overload
    standard requests on EP0
        SET_CONFIGURATION to the active value
            - answers 0 without sending anything to the device
        SET_CONFIGURATION to another value
            - answers -EPIPE
        SET_INTERFACE
            - cancels the interface's pending transfers with -ECONNRESET
            - forwards the request and claims the interface at the new alt
            - on failure, claims it at the previous alt again and answers the failure
        CLEAR_FEATURE(ENDPOINT_HALT)
            - forwards it to the device
            - answers -ECONNRESET for transfers pending on that endpoint
            - re-claims the interface at its current alt and resubmits the other endpoints' transfers in order
        SET_ADDRESS
            - answers -EPIPE
        port reset
            - answers 0 and resets nothing
        any other request with up to 4096 data bytes
            - forwards it and answers its status and data
    CMD_UNLINK
        When the seqnum is not pending
            - answers RET_UNLINK status 0
        When it still waits in the endpoint's FIFO
            - removes it and answers RET_UNLINK -ECONNRESET, no RET_SUBMIT
        When it is in flight on a bulk or interrupt endpoint
            - answers RET_UNLINK -ECONNRESET and no RET_SUBMIT for it
            - resubmits the endpoint's other canceled transfers in their original order, ahead of its FIFO
        When it is pending on EP0 and completes within 2 s
            - answers its RET_SUBMIT, then RET_UNLINK status 0
        When it is pending on EP0 and does not complete within 2 s
            - ends the session (the client sees an unplug)

usbip (status API)
    GET /device/usbip
        - reports enabled, active, pending_reboot, state, reason, port, interface
        - reports device and client, or null
        - reports counters and memory peaks
    PUT /device/usbip kick
        - ends the session if one runs; answers 200 in any case
```
