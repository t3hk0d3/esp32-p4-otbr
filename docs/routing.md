# Routing to the mesh

Thread devices get IPv6 addresses from the mesh's **OMR prefix** (off-mesh routable prefix; see the
[Glossary](glossary.md)). Computers on the same
Ethernet link as the board reach them through the board's router advertisements. Computers on **other** networks reach
them only when your main router knows that the prefix lives behind the board. This page is about telling it.

**Most homes need nothing here.** If your router hands out IPv6 prefixes by DHCPv6 prefix delegation (most ISP routers
with IPv6 do), the default, **DHCPv6 PD**, works by itself. Read on if:

- your router has no prefix delegation, or you have no IPv6 from your ISP and want to reach Thread devices from other
  VLANs or subnets, or
- you have your own address space and want Thread devices on a prefix you choose.

## The Routing page

**Thread > Routing** (`http://<board>/routing.html`). Changes apply when you press **Save**: no reboot, no trial. They
are kept across reboots and updates.

1. **How your router reaches the mesh:** pick a route method.

    | Method | What your router learns | When to use it |
    | --- | --- | --- |
    | **DHCPv6 PD** (default) | it delegated the prefix itself, so it routes it already | your router delegates prefixes |
    | **RIPng** | the board announces the prefixes every 30 s | your router speaks RIPng (FRR, BIRD, many small routers) |
    | **BGP** | the board announces the prefixes to one or two BGP peers | your router already runs BGP (UniFi, FRR, BIRD, MikroTik) |
    | **Off** | nothing | you add a static route on the router yourself |

2. **OMR prefix:** **Automatic** (the delegated prefix, or else the board's own ULA) or **Custom** (a /64 you route to
   the board).
3. For BGP: this board's ASN and the peers (an IPv6 address and an ASN each).
4. Save, then check **Sessions** (BGP) or the RIPng counters, and the router's routing table.

The page shows the OMR prefix in use, the board's own ULA and what is announced, and refreshes every 5 seconds.

There is no password on BGP or RIPng. Limit what your router accepts from the board to the board's prefixes, as in the
examples below.

## DHCPv6 prefix delegation (PD)

On with the route method `pd` (the default), off with every other method (see
[the route method](#route-method)); `pd.state` then reads `disabled`. `pd.state` and `pd.prefix` in
`/device/status` are `null` when OpenThread was busy and its state could not be read (ask again). `pd.state` mirrors
OpenThread's DHCPv6-PD client (`disabled`, `stopped`, `running`). The client is `running` while it solicits, so with no
delegating router `pd.state` is `running` and `pd.prefix` is `null`; the local ULA prefix is then the OMR prefix. With a
delegation, Thread devices get global addresses from it: this makes them reachable from the Internet unless your
router's IPv6 firewall blocks it. A delegating router must offer lifetimes above 300 s.


## OMR prefix

- **Automatic** (`auto`, default): the delegated prefix while the method is `pd` and a prefix is delegated, else the
  self-generated ULA.
- **Custom** (`custom`): one /64 you route to the board, for example a GUA /64 from your own space. It overrides PD. The board sets
  it as OpenThread's OMR prefix with preference medium. It must be a /64 with the host bits zero, not link-local
  (`fe80::/10`), not multicast, and not a prefix that is on-link on the backbone.

"Also use and announce the board's own ULA" (`announce_ula`):

- with `auto`: decides only whether RIPng or BGP announces the ULA;
- with `custom`: the board also publishes the ULA on the mesh as a SLAAC on-mesh prefix with preference low, so Thread
  devices get an address from both prefixes and the custom one stays the favored OMR prefix. A custom prefix equal to
  the ULA is accepted; the box then adds nothing.

The ULA is the one OpenThread generates and keeps across reboots. Prefixes added on the Addresses page
(`/add_prefix`) are left alone and never announced.

## Route method

Exactly one of:

| Method | What the router learns |
| --- | --- |
| `pd` (default) | nothing from the board: the delegating router knows the prefix it delegated |
| `ripng` | the announced prefixes over RIPng |
| `bgp` | the announced prefixes over BGP, to up to 2 peers |
| `off` | nothing |

Announced (`ripng` and `bgp`): the custom prefix when set, and the ULA when the box is ticked, each only while it is in
the mesh's Network Data (another border router's favored prefix can push it out). Switching method stops the old one
first: BGP sends a Cease (6/2) on every session, RIPng sends every prefix with metric 16 once.

While the border router is not running (packet capture, radio fault, Thread disabled or detached) every route is
withdrawn at once, and announced again when it runs. A BGP session that goes down, or a router that stops talking
RIPng, changes nothing on the mesh: the prefixes stay published.

Everything runs on the backbone interface: the single interface, or `thread_bb` in split mode. Connections and
packets on `mgmt` are refused or ignored. There is no authentication (lwIP has no TCP-MD5, RIPng has none): limit what
the router accepts from the board, as in the examples below.

## BGP

An announce-only speaker: it sends the announced prefixes and never installs what it receives. Routes from a peer are
counted (`received_dropped`) and dropped.

- Peers: at most 2, each an IPv6 address (global, or link-local on the backbone) and an ASN. The same ASN as the board
  is iBGP, a different one eBGP. 4-octet ASNs work; 23456 (AS_TRANS) is not allowed.
- The peer must advertise the 4-octet ASN and the IPv6 unicast capabilities; one that does not is refused with
  NOTIFICATION 2/7 (FRR and BIRD advertise both).
- Router ID: the board's IPv4 address on the backbone, else the configured `router_id`. With neither, sessions do not
  start and `status.bgp.blocked` is `"no router id"`. Without a link-local address on the backbone it is
  `"no link-local address"`.
- TCP port 179 over IPv6, both directions: the board connects out and accepts connections from its peers (RFC 4271
  collision handling). Retries back off from 5 s to 120 s.
- Hold time: 0 or 3..65535 s, default 90. Keepalives every third of the negotiated hold time.
- Attributes: ORIGIN IGP; AS_PATH empty on iBGP, the board's ASN on eBGP; LOCAL_PREF 100 on iBGP.
- Next-hop: the board's global address on the backbone (a GUA before a ULA) and its link-local, the 32-byte form of
  RFC 2545. With no global address, the link-local alone in a 16-byte next-hop, which RFC 2545 does not cover. FRR 10.4
  (which the UDM Pro runs) and BIRD 2.0.12 accept it on a directly connected session: checked on the test rig with
  the backbone on a VLAN whose advertised prefix has autoconfiguration off (`p4/pytest/test_routing.py`,
  `TestBgpLinkLocal`).
- Changes: changing the OMR prefix or the ULA box only sends UPDATEs. Removing a peer or changing its ASN closes that
  session with Cease 6/3. Changing the local ASN, the hold time or the router ID in use restarts every session with
  Cease 6/6.

The board, iBGP with a UDM Pro at `2001:db8:42::1`:

```
curl -X PUT http://<board>/device/routing -d '{"method":"bgp","omr":{"mode":"custom","prefix":"2001:db8:42:100::/64"},"announce_ula":true,"bgp":{"local_asn":4200000001,"router_id":null,"hold_time":90,"peers":[{"address":"2001:db8:42::1","asn":4200000001}]}}'
```

The router (FRR), with the board at `2001:db8:42::15`: accept only the board's /64s, at most 2 of them (more drops the
session, which FRR brings back up after 5 minutes instead of waiting for an operator), and send it nothing.

```
ipv6 prefix-list thread-br-in seq 5 permit 2001:db8:42:100::/64
ipv6 prefix-list thread-br-in seq 10 permit fd98:2656:4740:1::/64
!
route-map thread-br-out deny 10
!
router bgp 4200000001
 neighbor 2001:db8:42::15 remote-as 4200000001
 neighbor 2001:db8:42::15 description thread-border-router
 address-family ipv6 unicast
  neighbor 2001:db8:42::15 activate
  neighbor 2001:db8:42::15 prefix-list thread-br-in in
  neighbor 2001:db8:42::15 maximum-prefix 2 restart 5
  neighbor 2001:db8:42::15 route-map thread-br-out out
 exit-address-family
```

Take the ULA from `status.ula_prefix` of `GET /device/routing`. For eBGP give the board its own ASN and use it in
`remote-as`. A link-local peer is `neighbor fe80::... interface <backbone if>`.

## RIPng

RFC 2080 with fixed values; there are no RIPng settings.

- Responses go to `ff02::9` port 521 from the board's link-local address, hop limit 255. Each prefix is a /64 entry with
  route tag 0 and metric 1, so the router sees metric 2 with the board's link-local as next-hop.
- A full update every 30 s plus a random 0..5 s. A change goes out as a triggered update with only the changed entries,
  at most one every 5 s. A removed prefix is sent with metric 16 in the triggered update and in the next 4 regular
  updates.
- Switching away from `ripng`, or the border router stopping, sends every prefix with metric 16 at once, so the router
  drops the routes at once. If the board dies, the router drops them 180 s after the last update (the RFC 2080
  timeout).
- Requests are answered unicast to the sender's address and port. Responses from routers are counted
  (`responses_dropped`) and dropped.

The router (FRR `ripngd`), with the backbone on `eth0`:

```
ipv6 prefix-list thread-br-in seq 5 permit 2001:db8:42:100::/64
ipv6 prefix-list thread-br-in seq 10 permit fd98:2656:4740:1::/64
ipv6 prefix-list nothing seq 5 deny any
!
router ripng
 network eth0
 distribute-list prefix thread-br-in in eth0
 distribute-list prefix nothing out eth0
```

The router (BIRD 2), with the backbone on `eth0` and the usual `protocol device` and `protocol kernel` exporting to the
kernel:

```
protocol rip ng thread_br {
  ipv6 {
    import where net ~ [ 2001:db8:42:100::/64, fd98:2656:4740:1::/64 ];
    export none;
  };
  interface "eth0" { };
}
```

## Status

- `GET /device/routing`, `status`: the OMR prefix in use (maybe another border router's), the board's own OMR
  prefix, the ULA, what is announced, and per BGP session the state, uptime, last error and routes dropped, or the
  RIPng counters.
- `/device/status`, `routing`: method, OMR mode, number of announced prefixes, BGP sessions up and configured.
- `/metrics`: the `br_routing_*`, `br_bgp_*` and `br_ripng_*` series ([metrics](metrics.md#routing-metrics)).
- The Routing page shows the same and refreshes every 5 s.
