# Network modes, VLANs and prefix delegation

## Modes

The default is `single`: one untagged interface with DHCPv4 for everything. `split` separates two interfaces, `mgmt`
(web UI, `/device/*`, read-only `GET /node/*`) and `thread_bb` (the OpenThread backbone and the full Thread REST API
for Home Assistant), each untagged or on a VLAN ID (1..4094, distinct, at most one untagged). In split mode:

- Nothing is forwarded to or from `mgmt`. Replies leave by the interface the request came from.
- A request on the wrong interface is refused with 403 and the connection is closed.
- mDNS (hostname and every service, including `_meshcop._udp`) runs only on `thread_bb`; `mgmt` is never announced. A
  gateway that reflects mDNS between the VLANs may still show the `thread_bb` records on the `mgmt` VLAN; that is the reflector.

Change it on the Ethernet page (`http://<board>/ethernet.html`: pick the setup, edit the interfaces, review the
changes, then apply) or with `PUT /device/network`, for example:

```
curl -X PUT http://<board>/device/network -d '{"mode":"split","mgmt":{"vlan":10,"ipv4":{"dhcp":true}},"thread_bb":{"vlan":20,"ipv4":{"dhcp":true}}}'
```

Shape: `{"mode": "single"|"split", "single"|"mgmt"|"thread_bb": {"vlan": 0|1..4094, "ipv4": {"dhcp": true} | {"dhcp": false, "addr", "mask", "gw", "dns"}}}`.
`vlan: 0` is untagged. The switch port must carry the VLANs before you apply.

<table>
<tr>
<td width="33%"><img src="images/ethernet.png" alt="Ethernet page: the Single port and Split management and Thread setup cards, with the live interface and its addresses"></td>
<td width="33%"><img src="images/ethernet-edit.png" alt="Ethernet page while editing: Split is picked, VLAN 10 for management and VLAN 20 with a static address for the Thread backbone, and the bar says how many changes are not applied yet"></td>
<td width="33%"><img src="images/ethernet-review.png" alt="Review of the changes: a table of each setting now and after apply, the steps after the reboot, and the tick that the switch port carries VLAN 10 and 20 tagged"></td>
</tr>
<tr>
<td>1. Pick the setup.</td>
<td>2. Edit the interfaces.</td>
<td>3. Review, then apply and reboot.</td>
</tr>
</table>

## Confirm-or-revert

The change is stored as a trial and the board reboots into it. You then have 5 minutes to call
`POST /device/network/confirm` (or press **Keep new settings** on any device page) from the management side, that is the `single`
interface or `mgmt` of the trial config. If you do not (or the board resets first), the trial is dropped and the board
reboots into the previous config. If the board is unreachable after a network change: wait 5 minutes, or use the console.

<img src="images/ethernet-trial.png" width="640" alt="Ethernet page after the reboot into split mode: a banner says these settings are on trial and the board goes back in 4:08 unless you keep them, with the Keep new settings button">

Console (`make monitor`, USB-C port, 115200 baud, any build): `net show` prints the active config, trial and interfaces;
`net reset` restores `single`, untagged, DHCPv4 and reboots.

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

The DHCP client name is the name the board sends with its DHCP requests, which routers list. Default `esp-otbr`
(lwIP's `espressif` before); letters, digits and hyphens, at most 32 (the `esp_netif` limit). The board asks for a lease
renewal on save, so the router learns the name without the address changing. With a static IPv4 address there is no
DHCP and the setting has no effect.

`net reset` touches neither section.

## DHCPv6 prefix delegation

Always on. `pd.state` and `pd.prefix` in `/device/status` are `null` when OpenThread was busy and its state could not be
read (ask again). `pd.state` mirrors OpenThread's DHCPv6-PD client (`disabled`, `stopped`, `running`). The client is
`running` while it solicits, so with no delegating router `pd.state` is `running` and `pd.prefix` is `null`; the local ULA
prefix is then the OMR prefix. With a delegation, Thread devices get global addresses from it: this makes them reachable
from the Internet unless your router's IPv6 firewall blocks it. A delegating router must offer lifetimes above 300 s
(OpenThread raises T1/T2 to at least 300 s).
