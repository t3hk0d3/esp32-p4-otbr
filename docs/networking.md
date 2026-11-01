# Network modes, VLANs and prefix delegation

## Modes

The default is `single`: one untagged interface with DHCPv4 for everything. `split` separates two interfaces, `mgmt`
(web UI, `/device/*`, read-only `GET /node/*`) and `thread_bb` (the OpenThread backbone and the full Thread REST API
for Home Assistant), each untagged or on a VLAN ID (1..4094, distinct, at most one untagged). In split mode:

- Nothing is forwarded to or from `mgmt`. Replies leave by the interface the request came from.
- A request on the wrong interface is refused with 403 and the connection is closed.
- mDNS (hostname and every service, including `_meshcop._udp`) runs only on `thread_bb`; `mgmt` is never announced. A
  gateway that reflects mDNS between the VLANs may still show the `thread_bb` records on the `mgmt` VLAN; that is the reflector.

Change it with the device page or `PUT /device/network`, for example:

```
curl -X PUT http://<board>/device/network -d '{"mode":"split","mgmt":{"vlan":10,"ipv4":{"dhcp":true}},"thread_bb":{"vlan":20,"ipv4":{"dhcp":true}}}'
```

Shape: `{"mode": "single"|"split", "single"|"mgmt"|"thread_bb": {"vlan": 0|1..4094, "ipv4": {"dhcp": true} | {"dhcp": false, "addr", "mask", "gw", "dns"}}}`.
`vlan: 0` is untagged. The switch port must carry the VLANs before you apply.

## Confirm-or-revert

The change is stored as a trial and the board reboots into it. You then have 5 minutes to call
`POST /device/network/confirm` (or press Confirm on the device page) from the management side, that is the `single`
interface or `mgmt` of the trial config. If you do not (or the board resets first), the trial is dropped and the board
reboots into the previous config. If the board is unreachable after a network change: wait 5 minutes, or use the console.

Console (`make monitor`, USB-C port, 115200 baud, any build): `net show` prints the active config, trial and interfaces;
`net reset` restores `single`, untagged, DHCPv4 and reboots.

## DHCPv6 prefix delegation

Always on. `pd.state` and `pd.prefix` in `/device/status` are `null` when OpenThread was busy and its state could not be
read (ask again). `pd.state` mirrors OpenThread's DHCPv6-PD client (`disabled`, `stopped`, `running`). The client is
`running` while it solicits, so with no delegating router `pd.state` is `running` and `pd.prefix` is `null`; the local ULA
prefix is then the OMR prefix. With a delegation, Thread devices get global addresses from it: this makes them reachable
from the Internet unless your router's IPv6 firewall blocks it. A delegating router must offer lifetimes above 300 s
(OpenThread raises T1/T2 to at least 300 s).
