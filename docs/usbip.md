# USB/IP server

The board can export the device on its USB-A port over the network with USB/IP. A computer on the same network
attaches to it and sees the device as if it were plugged in locally. The typical use is a short job on a device that is
plugged into the board and out of reach of your PC, for example reflashing a radio dongle.

The feature is **off by default**. It uses the USB-A port, so a radio dongle there is no longer a radio while USB/IP is
on (see [Radios and USB/IP](#radios-and-usbip)).

!!! warning "No authentication, no encryption"
    Anyone who can reach TCP port 3240 on the board can take the device and talk to it. There is no password, no
    encryption and no allowlist. Turn USB/IP on only on a network you trust, keep the board's management interface
    off untrusted networks (use split mode with a VLAN for management), and turn it off when you are done. The server
    refuses connections from the Thread mesh.

## Turning it on

On the **USB/IP** page (Device menu) use the switch, or `PUT /device/usbip` with `{"enabled": true}`
([API](api.md#usbip)). Turning it on or off **reboots the board**; the page asks for confirmation first.

Turning it on is refused with 409 `usb_channel_in_use` while a USB channel's stored mode is `otbr` or `ziggurat`
(`"<id> runs <mode>: move it or switch it off first"`): move Thread or Zigbee to the C6 or switch the dongle off on the
[Radios page](radios.md#the-radios-page) first. A long operation (an update, a firmware push, a move of Thread)
answers 409 `busy`; try again afterwards.

If the board boots with USB/IP on and a USB channel holds `otbr` or `ziggurat`, Thread or Zigbee wins: the status
shows the state `blocked` with the reason, and nothing is exported.

## What is exported

- TCP port 3240. In `single` network mode on the one interface; in `split` mode on `mgmt` only
  ([Networking](networking.md#usbip)). Connections from the Thread mesh are refused. A connection whose first byte is
  not a USB/IP request (a browser, a scanner) is refused.
- One device, always with the bus id `1-1`, and one client at a time. A second client that tries to import it is told
  "device busy". Listing the device (`usbip list -r`) works at any time, also while a client holds it.
- When the session ends (the client detaches, disconnects or is kicked), the board resets the device by switching
  the USB port's power off and on and offers it again within a few seconds.
- When the device is unplugged, or resets itself, or comes back in a bootloader with another product id, the client
  sees an unplug and the attach is gone. Attach again with the same command (see [Keeping it attached](#keeping-it-attached)).

States (`state` in the API and on the page):

| State | Meaning |
| --- | --- |
| `off` | USB/IP is off (or switched on and the board not yet rebooted: `pending_reboot`). |
| `blocked` | It is on, but a USB channel holds Thread or Zigbee; `reason` says which. |
| `no_device` | Nothing is plugged into the USB-A port. |
| `unsupported` | A device is plugged in but cannot be exported; `reason` says why (a hub, more than 15 non-control endpoints, the device failed to open). |
| `exportable` | The device is ready and no client holds it. |
| `attached` | A client holds the device. |

## Attaching from Linux

```
sudo modprobe vhci-hcd
usbip list -r <board>
sudo usbip attach -r <board> -b 1-1
sudo usbip port
sudo usbip detach -p <port>
```

`<board>` is the board's address or hostname. `usbip list -r` shows the device with its ids; `usbip port` shows the
local port number to give to `detach`. The `usbip` tool comes with the `usbip` or `linux-tools` package of your
distribution. Add `vhci-hcd` to `/etc/modules-load.d/` to load it at boot.

Once attached, the device appears under `/dev` like a local one (`/dev/ttyUSB0`, `/dev/ttyACM0`) and tools such as
`esptool` or `cc2538-bsl` work with it.

## Attaching from Windows

Use [usbip-win2](https://github.com/vadimgrn/usbip-win2). Install it, then in a terminal:

```
usbip list -r <board>
usbip attach -r <board> -b 1-1
```

and `usbip detach` (see `usbip port`) to release it.

## Keeping it attached

After an unplug or a reset of the device the attach is gone and the client must attach again; nothing does that for
you. For a job that resets the device (reflashing, a bootloader switch), run a retry loop while you work:

```
while true; do sudo usbip attach -r <board> -b 1-1; sleep 3; done
```

(attach fails while the device is attached or not yet offered, which is harmless; stop it with Ctrl-C). For a device
that should always be there, a systemd unit on the client:

```
# /etc/systemd/system/usbip-attach.service
[Unit]
Description=Attach the USB device of the board over USB/IP
After=network-online.target
Wants=network-online.target

[Service]
ExecStartPre=/usr/sbin/modprobe vhci-hcd
ExecStart=/usr/sbin/usbip attach -r <board> -b 1-1
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

`usbip attach` returns as soon as the device is attached, so the unit restarts it every 5 s: while the device is
still attached the command fails harmlessly, and after an unplug or a reset it attaches again. Adjust the path of
`usbip` (`which usbip`).

## Limits

- No isochronous transfers: audio devices, webcams and Bluetooth SCO voice do not work. Bluetooth data does.
- No hubs. A hub on the port is `unsupported`.
- One configuration. `SET_CONFIGURATION` to another value than the active one is refused.
- Transfers over 16 KB are refused. Configuration descriptors up to 1024 bytes are read
  (`CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE`).
- A device with more than 15 non-control endpoints is not exported.
- One device and one client at a time.

Memory: the transfers in flight use internal RAM, at most 32 KB plus, in the worst case, one transfer of up to 16 KB
per endpoint. Requests that wait for the device queue in PSRAM, up to 1 MB per session; beyond that the session ends
(counter `sessions_ended_overload`).

Devices that stall or never answer a control transfer end the session, and the board resets them.

## The USB/IP page

`/usbip.html`, in the Device menu.

- The **switch** turns USB/IP on or off after a confirmation that the board reboots, then follows the reboot. A refused
  switch-on (409 `usb_channel_in_use`) links to the Radios page.
- The **device card** shows the product, ids, speed and state, with the reason when `unsupported` or `blocked`.
- The **client card** shows the address and since when, with a **Kick** button that ends the session.
- **Counters** (sessions, refused connections, submits, completed, errors, unlinks, bytes, sessions ended for overload)
  and the memory in use and its peaks. They count since boot.
- **How to attach** shows the client commands with the board's address.

## Radios and USB/IP

While USB/IP is on, the USB port belongs to it. The [Radios page](radios.md#usbip) lists USB channels as unavailable
with the reason "USB is exported over USB/IP", and links here. Their modes cannot be changed (409 `usbip_enabled`);
line and proxy settings and Forget still work. Their modes come back when USB/IP is turned off.

## Troubleshooting

| Symptom | Cause |
| --- | --- |
| `usbip attach` says device busy | Another client holds it, or the previous session is still ending. Kick it on the page. |
| Attach fails right after a detach | The board is resetting the device; retry after a few seconds. |
| The device vanished after a flash or reset | It re-enumerated; attach again. |
| `usbip list` shows nothing, state `unsupported` | See the reason: a hub, too many endpoints, or the device would not open. |
| State `blocked` | A USB channel runs Thread or Zigbee. Move it, then reboot. |
| Connection refused or reset at once | USB/IP is off, the request came from the wrong interface (split mode: use `mgmt`) or from the Thread mesh. |
