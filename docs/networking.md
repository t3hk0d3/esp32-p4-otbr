# Networking

Everything on **Device > Ethernet**: how the board sits on your network, its names, its clock, and Thread over Ethernet.

## Single port or split VLANs

- **Single port** (default): one network for everything, untagged, with DHCP. Right for most homes.
- **Split management and Thread:** two networks on the same cable, each untagged or on its own VLAN:
    - **Management** (`mgmt`): the web UI, the device API and metrics. Keep it on a private admin VLAN.
    - **Thread backbone** (`thread_bb`): where Thread meets your home network: Home Assistant's Thread REST API, mDNS,
      other border routers (TREL) and routing.

  Nothing passes between the two. A request on the wrong one is refused (403). mDNS runs only on the Thread backbone,
  so Home Assistant must be on that network. Set up the switch port before you apply: each VLAN you
  give an ID must be tagged on the port, and an interface you leave untagged uses the port's untagged (native) VLAN.

![Split mode: one tagged cable to a switch; the management VLAN for the browser and Prometheus, the Thread backbone VLAN for Home Assistant and other border routers](images/split-mode.svg)

Change it on the Ethernet page: pick the setup, edit the interfaces (VLAN, DHCP or a static address), review the changes,
then apply. The board reboots into the new settings on trial.

<table>
<tr>
<td width="33%"><img src="images/ethernet.png" alt="Ethernet page: the Single port and Split management and Thread setup cards, Split in use, and the Management (untagged) and Thread backbone (VLAN 49) interfaces with their addresses"></td>
<td width="33%"><img src="images/ethernet-edit.png" alt="Ethernet page while editing: Split is picked, VLAN 10 for management and VLAN 20 with a static address for the Thread backbone, and the bar says how many changes are not applied yet"></td>
<td width="33%"><img src="images/ethernet-review.png" alt="Review of the changes: a table of each setting now and after apply, the steps after the reboot, and the tick that the switch port carries VLAN 10 and 20 tagged"></td>
</tr>
<tr>
<td>1. Pick the setup.</td>
<td>2. Edit the interfaces.</td>
<td>3. Review, then apply and reboot.</td>
</tr>
</table>

API: `PUT /device/network` ([HTTP API](api.md#device-api)), for example
`curl -X PUT http://<board>/device/network -d '{"mode":"split","mgmt":{"vlan":10,"ipv4":{"dhcp":true}},"thread_bb":{"vlan":20,"ipv4":{"dhcp":true}}}'`.

## Confirm-or-revert

A wrong VLAN or address could lock you out of the board, so a network change is only a trial at first. After the reboot,
every Device page shows a banner with a countdown: open the board at its new address and press **Keep new settings**
within 5 minutes. If nobody does, the board goes back to the previous settings by itself.

<img src="images/ethernet-trial.png" width="640" alt="Ethernet page after the reboot into split mode: a banner says these settings are on trial and the board goes back in 4:08 unless you keep them, with the Keep new settings button">

Keep the settings from the management side: the single port, or `mgmt` in split mode.

Locked out anyway? Connect USB-C and open the console at 115200 baud (`make monitor` from a checkout): `net show` prints
the settings, `net reset` goes back to a single untagged port with DHCP and reboots.

## mDNS and hostname

Two sections at the end of the Ethernet page (or `GET`/`PUT /device/names`, see [API](api.md)) set how the board is
named on the network. Each has its own Save; a save takes effect at once: no reboot, no trial, and the IPv4 lease is
kept.

### mDNS

<img src="images/ethernet-mdns.png" width="640" alt="The mDNS section of the Ethernet page with an Announced chip: hostname kitchen with the .local suffix, Same as hostname ticked for the Home Assistant name, Advertise the web UI unticked, and the Discard and Save buttons">

| Setting | What it is | Default |
| --- | --- | --- |
| Hostname | the mDNS name, `<name>.local` | `esp-otbr-<last six hex digits of the MAC>` |
| Name in Home Assistant | the `_meshcop._udp` instance name Home Assistant's Thread panel shows | follows the hostname |
| Advertise the web UI | an `_http._tcp` record (port 80, `path=/`) so that network browsers find the web UI | off |

The hostname: letters, digits and hyphens, at most 63, not starting or ending with a hyphen; upper case is stored in
lower case; empty means the default. The Home Assistant name: at most 63 bytes of UTF-8, no control characters.

- **Conflicts:** when another device already uses the hostname, the board answers as `<name>-2.local`. The section
  shows a **Renamed** chip (**Announced** otherwise) and the name in use (`hostname_live`). Saving the same name again
  tries it again.

  <img src="images/ethernet-mdns-conflict.png" width="640" alt="The mDNS section after a conflict: a Renamed chip and a banner saying another device already uses kitchen.local, so the board answers as kitchen-2.local; Save is enabled to try the name again">

- **Moving to the new name:** a page opened at the old `.local` name moves to the new one after a save.
- **Split mode:** the hostname resolves on `thread_bb` only (mDNS runs there), while the web UI is on `mgmt`: open the
  page by the management address. For the same reason the web UI advert is not available in split mode (the setting is
  kept and applies again in single mode).

### Hostname

<img src="images/ethernet-hostname.png" width="640" alt="The Hostname section of the Ethernet page: an empty DHCP client name with the default esp-otbr in its hint, and the Discard and Save buttons">

The DHCP client name is the name the board sends with its DHCP requests, which routers list. Default `esp-otbr`; letters, digits and hyphens, at most 32. The board asks for a lease
renewal on save, so the router learns the name without the address changing. With a static IPv4 address there is no
DHCP and the setting has no effect.

`net reset` touches neither section.

## Time

The board has no battery clock, so it learns the date from a time server ([NTP](glossary.md)) after every
power-up. With the clock set, status pages show real dates, for example when a Zigbee client connected or when radio
firmware was installed, and syslog lines carry a timestamp. Without it they read "clock not set". The board keeps UTC;
the web UI shows times in your browser's time zone.

The **Time** section at the end of the Ethernet page shows the board time, whether it is synced, when it last synced,
and which server it asks:

<img src="images/ethernet-time.png" width="640" alt="The Time section of the Ethernet page with a Synced chip: board time, last synced 2 minutes ago, server 162.159.200.1 with an answered chip, from your router (DHCP on Management); Set the clock from the network ticked, Automatic picked, and the Discard and Save buttons">

| Choice | Server used |
| --- | --- |
| **Automatic** (default) | the NTP server your router hands out with the DHCP lease (up to two); if it hands out none, `pool.ntp.org` |
| **Custom server** | the hostname or IP address you enter, and only that one: no fall-back |
| Off (untick **Set the clock from the network**) | none: the board sends no NTP traffic |

A save takes effect at once, without a reboot. The clock survives a reboot or an update; after a power loss it is
unset until the first answer, usually a few seconds after the board gets an address.

What leaves the board: NTP requests (UDP port 123) to the server in use, about once an hour. With Automatic and a
router that names no server, that is `pool.ntp.org` on the internet; set a custom server or turn sync off if the board
must not reach the internet.

- **Chips:** **Synced**; **Waiting for server** (just started); **Server not answering** (no answer for a minute, or
  the servers stopped answering; the clock keeps running from the last answer); **Clock not set**; **Off**.
- **Split mode:** the board takes the servers from whichever lease names them; the section says which interface, and
  that interface's card shows them in a **Time server** row.
- **Static address:** there is no lease, so Automatic uses `pool.ntp.org`, which needs a DNS server in the static
  settings.

From a script:

```sh
curl http://esp-otbr.local/device/time
curl -X PUT -H 'Content-Type: application/json' \
     -d '{"enabled": true, "mode": "custom", "server": "ntp.home.lan"}' http://esp-otbr.local/device/time
```

Fields and errors: [API](api.md#device-api).

## TREL (Thread over Ethernet)

TREL (Thread Radio Encapsulation Link) carries Thread mesh links over Ethernet, next to the 15.4 radio: border routers of
the same Thread network on the same link become direct neighbours, so two radio islands joined only by the LAN form one
Thread network, and the mesh can route around weak radio hops. Thread devices themselves keep using the radio.

![TREL: two boards with separate radio areas joined over an Ethernet switch into one Thread mesh](images/trel.svg)

- On by default. It runs on the backbone interface: `thread_bb` in split mode, the Ethernet port otherwise. Peers are found
  with mDNS (`_trel._udp`) on that link, and only peers of the same Thread network (extended PAN ID) are used.
- It needs a partner: another border router of the same Thread network on that link that speaks TREL (OpenThread
  border routers built with TREL, such as ot-br-posix with `OTBR_TREL`, or another ESP Thread border router). To see
  which ones do: `avahi-browse -rt _trel._udp` (macOS: `dns-sd -B _trel._udp`). Without one, TREL does nothing.
- Switch it on the Ethernet page ("Thread over Ethernet": Off/On), or with `PUT /device/network` and only `trel` changed
  (send the active config with `trel` flipped). Such a change is applied at once and stored, without a trial or a
  reboot (`{"status":"applied"}`; `"saved"` when OpenThread was busy: it then applies at its next start). It is refused
  with 409 while a network trial runs. Changed together with interface settings, it goes through the trial with them.
- State, peers and counters: `trel` in `GET /device/status`, the card on the Ethernet page, `br_trel_*` in
  [metrics](metrics.md).
- TREL border routers of other Thread networks on the same link are found too, but never used. They are listed with
  their extended PAN ID and a red "other" chip and counted apart ("+ N peers of other networks", `peers_other`,
  `br_trel_peers_other`); "Peers" counts this network only.
- Preference: for a peer that is also a Thread neighbour, the card shows how much OpenThread trusts each path, radio
  and TREL, as a percentage. It sends over TREL while that is 86 % or more. The TREL figure turns red when it is lower
  than the radio's: the tunnel is losing. A broken tunnel shows as a falling TREL figure, not as a missing peer.
- Topology page: each TREL link of this board is a dashed purple curve through the "Home network · Ethernet" box with
  its TREL preference, red under the same rule; a link that runs over TREL only has no straight radio line. The device
  dialog and the "Connected via" column give both preferences and the radio OpenThread sends over. Links between
  other routers don't show whether they use TREL: Thread doesn't report it.
- In split mode UDP port 12390 (TREL) is dropped on `mgmt`.

<img src="images/ethernet-trel.png" width="640" alt="TREL card on the Ethernet page: active on the Thread backbone, the Off/On switch on On, 1 peer, sent and received packet counters, and the peer table with its extended address, network (this), preference 15.4 100% and TREL 100%, and address">

## USB/IP

With the [USB/IP server](usbip.md) on, the board listens on TCP port 3240. In `single` mode it listens on the one
interface; in `split` mode on `mgmt` only, and a connection on `thread_bb` is refused. Connections from the Thread mesh
are refused. There is no authentication or encryption: keep the port off untrusted networks.
