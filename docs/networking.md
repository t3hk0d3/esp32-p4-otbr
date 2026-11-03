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

## Names

The **Names** section at the end of the Ethernet page (or `GET`/`PUT /device/names`, see [API](api.md)) sets how the
board is named on the network. A save takes effect at once: no reboot, no trial, and the IPv4 lease is kept.

| Setting | What it is | Default |
| --- | --- | --- |
| Hostname | the mDNS name, `<name>.local` | `esp-otbr-<last six hex digits of the MAC>` |
| Name in Home Assistant | the `_meshcop._udp` instance name Home Assistant's Thread panel shows (Thread adds ` #XXXX`) | follows the hostname |
| Advertise the web UI | an `_http._tcp` record (port 80, `path=/`) so that network browsers find the web UI | off |
| DHCP client name | the name the board sends with its DHCP requests, which routers list | `esp-otbr` |

Hostname and DHCP name: letters, digits and hyphens, at most 63 (DHCP name: 32), not starting or ending with a
hyphen; upper case is stored in lower case. The Home Assistant name: at most 63 bytes of UTF-8, no control characters. An empty hostname or
DHCP name means the default.

- **Conflicts:** when another device already uses the hostname, the board answers as `<name>-2.local`. The page shows
  a **Renamed** chip and the name in use (`hostname_live`). Saving the same name again tries it again.
- **Moving to the new name:** a page opened at the old `.local` name moves to the new one after a save.
- **DHCP name:** the board asks for a lease renewal on save, so the router learns the name without the address
  changing. With a static IPv4 address there is no DHCP and the setting has no effect.
- **Split mode:** the hostname resolves on `thread_bb` only (mDNS runs there), while the web UI is on `mgmt`: open the
  page by the management address. For the same reason the web UI advert is not available in split mode (the setting is
  kept and applies again in single mode).
- `net reset` does not touch the names.

## DHCPv6 prefix delegation

Always on. `pd.state` and `pd.prefix` in `/device/status` are `null` when OpenThread was busy and its state could not be
read (ask again). `pd.state` mirrors OpenThread's DHCPv6-PD client (`disabled`, `stopped`, `running`). The client is
`running` while it solicits, so with no delegating router `pd.state` is `running` and `pd.prefix` is `null`; the local ULA
prefix is then the OMR prefix. With a delegation, Thread devices get global addresses from it: this makes them reachable
from the Internet unless your router's IPv6 firewall blocks it. A delegating router must offer lifetimes above 300 s
(OpenThread raises T1/T2 to at least 300 s).
