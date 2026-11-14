# Troubleshooting

Start at the Dashboard. A problem shows in red, with what to do and a link to the page that fixes it. A problem of the
whole board shows on the **Device and network** line; a radio problem of Thread shows in the **Thread** section:

<img src="images/dashboard-radio-fault.png" width="640" alt="Dashboard in RADIO_FAULT with fault_reason rcp incompatible: Device and network still says Board OK and the Radios card lists the ESP32-C6 (built-in) as Unavailable; the Thread section shows Radio fault in red, the radio firmware does not match this build, install the c6_rcp.bin of the same release, with a link to the Firmware page; the Thread cards are empty">

The Device pages (Radios, Ethernet, Firmware, Diagnostics) keep working in every fault, so you can always update or
repair the board from the browser.

## Radio faults

The Dashboard names the reason (`fault_reason` in `GET /device/status`):

| Reason | What it means | What to do |
| --- | --- | --- |
| **no rcp firmware** | The C6 has no radio firmware (new board, or after a repair). | Upload `c6_rcp.bin` on the Firmware page. |
| **esp-hosted firmware** | The C6 still runs its factory firmware. | Press **Install** on the Updates from GitHub card, or upload `c6_installer.bin`, then `c6_rcp.bin`, both from the board's release ([First install](first-install.md#a-no-wires)). |
| **rcp incompatible** | The radio firmware doesn't match the board's firmware, or the radio doesn't answer. | Upload `c6_rcp.bin` from the same release as the board's firmware. |
| **rom recovery failed** | The C6 didn't answer and the repair over the maintenance jumper failed. | Check the jumper wires, then **Recover radio** on the Firmware page. Without a jumper: [First install](first-install.md#c-usb-uart-adapter), way C. |
| **resetting the RCP failed** | The radio stopped answering and the board's automatic recovery failed. It tries up to three times in 10 minutes, then waits. | Check the antenna. Reboot the board, or upload `c6_rcp.bin` again. |
| **radio boot timed out**, **OpenThread main loop ended** | The radio didn't come up, or Thread stopped. The board does not retry these by itself. | Check the antenna, then reboot the board. If it comes back, upload `c6_rcp.bin` again. |
| **storage unavailable** | The board's settings storage didn't start. The board runs without Thread rather than restart in a loop. | Update the firmware. If that doesn't help, erase the settings: `esptool --chip esp32p4 -p /dev/ttyACM0 erase-region 0x9000 0x10000` over USB-C, then reboot; the Thread network and network settings have to be set up again. |

With a dongle running Thread, **No radio** means it is unplugged: plug it back in, or move Thread to another radio
([Radios](radios.md#unplugging-the-thread-dongle)).

## Common problems

- **The board is unreachable after a network change.** Wait 5 minutes: unconfirmed network settings go back by
  themselves. Or connect USB-C, open the console at 115200 baud (`make monitor` from a checkout) and type `net reset`:
  the board goes back to one port with DHCP and reboots.
- **No IP address.** Check the cable and the switch port (and its VLANs in split mode). The console command `net show`
  prints the interfaces and their addresses.
- **Dates show "clock not set".** The board has not reached a time server since it was powered up. On **Device >
  Ethernet**, the Time section says why: **Off** (sync is turned off), or **Server not answering** (the server is
  wrong or unreachable; with a static address, Automatic also needs a DNS server to find `pool.ntp.org`). See
  [Time](networking.md#time).
- **The Dashboard warns that Thread and Zigbee share a channel.** Both networks are on the same 802.15.4 channel: they
  work, but each slows the other under load. Move Thread with Home Assistant's **Change channel**, which keeps the
  same network ([how](home-assistant.md#move-the-board-onto-home-assistants-preferred-network); some devices may need
  to be re-joined). Don't form a new Thread or Zigbee network for this: devices stay on the old one and must be paired
  again. See [Thread and Zigbee together](zigbee.md#thread-and-zigbee-together).
- **Home Assistant can't connect, or doesn't list the board.** See [Home Assistant](home-assistant.md#troubleshooting).
- **The Thread pages say "radio not available"** (or Home Assistant gets 503): the radio is in fault, see above.
  **"sniffer active"**: a packet capture is running; stop it on the Packet capture page.
- **The board rebooted by itself** and the console showed `supervisor: ... is stuck`: the board caught a hang and
  restarted. Please [report it](https://github.com/t3hk0d3/esp32-p4-otbr/issues), with the console log if you have it.
- **Pings through the board are slower while the Topology page loads.** Expected: a scan asks every router and adds about
  0.1 s to the slowest replies while it runs.

## Repairing the board

- **The C6 is broken** (its firmware or base image damaged): with the [maintenance jumper](maintenance-jumper.md), press
  **Recover radio** on the Firmware page (the board also tries by itself at boot), then upload `c6_rcp.bin` if the
  Dashboard asks for it. Without the jumper: [First install](first-install.md#c-usb-uart-adapter), way C.
- **The P4 is broken** (a bad update that didn't roll back): flash it again over USB-C with the
  [browser flasher](flash.md), letting it erase first. This deletes the stored Thread network and network settings. If
  the P4 won't enter download mode, hold BOOT while you press RESET.
- **Back to the factory firmware:** the original firmware is not part of this project; get it from Waveshare or
  Espressif.

## Known limitations

- **No login and no encryption.** Anyone on the network can reconfigure the board, read the Thread network key or update
  its firmware. The [USB/IP server](usbip.md), when on, hands the USB device to whoever attaches first. Put the board
  on a network you trust; in [split mode](networking.md) keep the management VLAN private.
- **Prefix delegation makes Thread devices reachable from the internet** unless your router's IPv6 firewall blocks it.
- **Updates interrupt Thread.** A radio firmware update stops this border router for a few seconds, a board update
  reboots it. Other border routers keep the network up meanwhile.
- **Not yet run on hardware:** way C of the first install. Several boards on one network have not been tested
  together.
- **mDNS names:** another device that claims the board's `.local` name can make it rename itself (standard mDNS); see
  [Networking](networking.md#mdns).
