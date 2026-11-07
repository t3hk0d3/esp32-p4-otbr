# Zigbee coordinator (experimental)

!!! warning "Experimental"
    The Ziggurat stack ran on a board in a spike. This feature on the board's firmware has been built and host-tested,
    and its on-board tests are written but have not yet been run on a board. Expect rough edges, and do not rely on it
    for a network you cannot afford to lose.

In Zigbee mode the board is a Zigbee coordinator instead of a Thread border router. A radio of the board runs
Ziggurat (a Zigbee stack written in Rust) and Home Assistant's ZHA integration
talks to it over a WebSocket. The radio is the C6 or a supported USB dongle; the board's own Zigbee stack runs on
the P4. Nothing else changes: Ethernet, the web UI, updates and the diagnostics work as before.


## Choosing it

On the **Radios** page set the mode of the radio to **Zigbee** (`ziggurat`), or `PUT /device/channels/<id>` with
`{"mode": "ziggurat"}` (the C6 is `c6`, a USB dongle `usb-<vid>-<pid>[-<serial>]`). Changing into or out of Zigbee
mode reboots the board, and the answer says `"reboot": true`. When the board is back, `GET /device/zigbee` shows
`running` and the **Zigbee** page appears in the menu.

A radio in Zigbee mode is not a Thread radio. If the C6 is the Zigbee radio, Thread does not run on the board (the
Thread menu entries are hidden and the Thread REST API does not serve a network); Home Assistant's OpenThread Border
Router integration then has nothing to talk to. Setting the C6 back to `otbr` reboots into Thread with the dataset
the board had stored.

## Connecting Home Assistant

1. Add the ZHA integration and choose to set it up manually.
2. Radio type: **Ziggurat**. Serial device path: `ws://<board>/zigbee` (the Dashboard's Zigbee section shows the
   exact address and a Copy button).
3. ZHA forms a new network (the stack keeps no state of its own: ZHA's database holds the network settings) or
   restores one from a backup.

Zigbee2MQTT and other zigpy-based software work the same way if they have the Ziggurat adapter.

One client at a time. A second client that connects takes the connection over: the first one is dropped. The Zigbee
page and `GET /device/zigbee` show the current client and the last one.

!!! warning "No login"
    Like the rest of the board's HTTP API, the WebSocket needs no password. Anyone who can reach the board can control
    your Zigbee network. Keep the board off networks you do not trust. The WebSocket follows the same per-interface
    policy as the device API (`/device/`): on an interface where the device API is not allowed, the upgrade is refused
    with 403.

## The Dashboard's Zigbee section

The Dashboard (`/`) has three sections: Device and network, Thread and Zigbee. While a radio runs Zigbee, the Zigbee
section shows the state (`starting`, `running`, `no_radio`, `incompatible`, `idle`), the Home Assistant connection
path with a Copy button, the connected or last client, the network (channel, PAN ID, addresses, TX power), the radio
traffic counters, the memory the stack uses and the radio firmware; it polls `GET /device/zigbee`. While no radio
runs Zigbee it says so in one line and links the Radios page, and nothing is polled.

<img src="images/dashboard.png" width="640" alt="Dashboard in three sections. Device and network: Board OK, the Radios card (C6 runs Zigbee, USB radio off), Ethernet, board firmware, load and memory. Thread: not active, with a link to the Radios page. Zigbee: Coordinator running on channel 25 on the on-board C6, ZHA connected; cards for Connect Home Assistant (the ws:// path and a Copy button), the connected client, the network as ZHA set it, radio traffic, memory and the radio firmware">

## Radio resets and loss

- **Reset.** When the radio resets (the C6 restarts, a dongle is reopened), the board sets the radio up again by
  itself (it replays the radio configuration) and the client stays connected. ZHA sees no event. A transmit that was
  in flight fails, as a lost frame does. `counters.rcp_resets` counts them.
- **Radio gone.** When the radio disappears (a dongle is unplugged, the C6 is held in reset or being updated) the
  state is `no_radio` with a reason. The board does not reboot, the stack keeps running and the client stays
  connected; transmits fail meanwhile. When the radio returns, the board replays the radio configuration and the
  state is `running` again.
- **Firmware that does not fit.** A radio whose firmware cannot run the stack shows `incompatible` with the reason.

## Limits

- Experimental; the on-board checks are listed in the tests (`p4/pytest/test_zigbee.py`).
- No hybrid: a radio runs either Thread or Zigbee, never both at once.
- One Zigbee radio at a time; a second channel in Zigbee mode is refused.
- No settings on the board: the network (channel, PAN ID, keys) is ZHA's. Resetting the board forgets the network, so
  keep ZHA's backups.
- Mode changes into and out of Zigbee reboot the board.
