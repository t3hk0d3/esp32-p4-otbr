# USB/IP server

The board can export the device on its USB-A port over the network with USB/IP. A computer on the same network
attaches to it and sees the device as if it were plugged in locally. The typical use is a short job on a device that is
plugged into the board and out of reach of your PC, for example reflashing a radio dongle.

The feature is **off by default**. It uses the USB-A port, so a radio dongle there is no longer a radio while USB/IP is
on (see [Radios and USB/IP](#radios-and-usbip)).

!!! warning "No authentication, no encryption"
    Anyone who can reach TCP port 3240 on the board can take the device and talk to it. There is no password, no
    encryption and no allowlist. Turn USB/IP on only on a network you trust, keep the board's management interface
    off untrusted networks (see [Networking](networking.md#modes)), and turn it off when you are done. The server
    refuses connections from the Thread mesh.

## Turning it on

On the **USB/IP** page (Device menu) use the switch, or `PUT /device/usbip` with `{"enabled": true}`
([API](api.md#usbip)). Turning it on or off **reboots the board**: flip the switch, then press **Save and Reboot**.

Turning it on is refused with 409 `usb_channel_in_use` while a USB channel's stored mode is `otbr` or `ziggurat`
(`"<id> runs <mode>: move it or switch it off first"`): move Thread or Zigbee to the C6 or switch the dongle off on the
[Radios page](radios.md#the-radios-page) first. A long operation (an update, a firmware push, a move of Thread)
answers 409 `busy` ("an update or firmware push is running or waits for its reboot"); try again afterwards.

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
| `off` | USB/IP is off (or switched on and the board not yet rebooted: `pending_reboot`; or on but failed to start at boot: `reason` says why, reboot to try again). |
| `blocked` | It is on, but a USB channel holds Thread or Zigbee; `reason` says which. |
| `no_device` | Nothing is plugged into the USB-A port. |
| `unsupported` | A device is plugged in but cannot be exported; `reason` says why (a hub, more than 15 endpoints, the device failed to open). |
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

If a retry loop or unit keeps the device attached ([Keeping it attached](#keeping-it-attached)), stop it before you
detach, or it attaches again within seconds.

Once attached, the device appears on the client like a local one, for example as `/dev/ttyUSB0` or `/dev/ttyACM0` for
a serial adapter.

## Attaching from Windows

Use [usbip-win2](https://github.com/vadimgrn/usbip-win2). Install it, then in a terminal:

```
usbip list -r <board>
usbip attach -r <board> -b 1-1
```

and `usbip detach` (see `usbip port`) to release it. Stop a retry loop first
([Keeping it attached](#keeping-it-attached)), or it attaches again within seconds.

## Keeping it attached

After an unplug or a reset of the device the attach is gone and the client must attach again; nothing does that for
you. For a job that resets the device (reflashing, a bootloader switch), run a retry loop while you work. It attaches
only when the board's device is not already attached here (`usbip port` lists an attached device as
`usbip://<board>:3240/1-1`):

```
while true; do
  usbip port | grep -q "<board>:3240/1-1" || sudo usbip attach -r <board> -b 1-1
  sleep 3
done
```

Stop it with Ctrl-C. For a device that should always be there, a systemd unit on the client does the same:

```
# /etc/systemd/system/usbip-attach.service
[Unit]
Description=Attach the USB device of the board over USB/IP
After=network-online.target
Wants=network-online.target
StartLimitIntervalSec=0

[Service]
ExecStartPre=/usr/sbin/modprobe vhci-hcd
ExecStart=/bin/sh -c 'usbip port | grep -q "<board>:3240/1-1" || exec usbip attach -r <board> -b 1-1'
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

The command exits as soon as the device is attached, so the unit runs it again every 5 s: nothing happens while the
device is attached, and after an unplug or a reset it attaches again. Adjust the path of `usbip` (`which usbip`) if
needed. **Stop the unit or the loop before you detach or kick**, otherwise it attaches again within seconds and the
device stays out of reach of any other computer.

## Limits

- No isochronous transfers: isochronous transfers, as used by audio devices, webcams and Bluetooth SCO voice, are not supported.
- No hubs. A hub on the port is `unsupported`.
- One configuration. `SET_CONFIGURATION` to another value than the active one is refused.
- Transfers over 16 KB are refused. Configuration descriptors up to 1024 bytes are read
  (`CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE`).
- A device with more than 15 endpoints, counting alternate setting 0 of every interface, is not exported.
- Interrupt transfers longer than 32 packets fail (`-EPROTO`), for example over 256 bytes on a full-speed HID device
  with 8-byte packets: the USB host driver schedules at most 32 packets per interrupt transfer.
- One device and one client at a time.

Memory: the transfers in flight use internal RAM, at most 80 KB in all. Reads (IN) share 32 KB; every endpoint can
always have one transfer of up to 2 KB in flight, and a larger first transfer (a 16 KB write, for example) goes when it
fits under the 80 KB. A transfer is also refused (`-ENOMEM`) when it would leave less than 48 KB of internal DMA memory
free. Requests that wait for the device queue in PSRAM, up to 1 MB per session; beyond that the session ends (counter
`sessions_ended_overload`).

Devices that stall or never answer a control transfer end the session, and the board resets them.

## The USB/IP page

`/usbip.html`, in the Device menu.

- The **switch** only stages the change: **Save and Reboot** appears next to it, says what the reboot does, stores the
  value and follows the reboot. Flipping the switch back drops the change. A refused switch-on (409
  `usb_channel_in_use`) links to the Radios page.
- The **device card** shows the product, ids, speed and state, with the reason when `unsupported` or `blocked`.
- The **client card** shows the address and since when, with a **Kick** button that ends the session.
- Four tiles show **Sessions**, **Transfers**, **From device** and **Buffers in use**. The numbers count since boot;
  the API has the full set.
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
