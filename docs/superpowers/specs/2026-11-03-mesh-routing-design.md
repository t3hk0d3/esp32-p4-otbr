# Routing to the mesh (BGP, RIPng, custom OMR prefix): design

Date: 2026-11-03. Status: draft for review.

## 1. Goal

Some networks cannot route to the Thread mesh because DHCPv6 prefix delegation is not available. The case behind this
design is a UniFi UDM Pro whose ISP gives no IPv6 at all. Its IPv6 space comes from the owner's own ASN, over BGP
sessions on WireGuard tunnels, so there is no delegation for the board to ask for. Today the board then falls back to a
self-generated ULA OMR prefix. The UDM Pro never learns that prefix, because routers ignore the Route Information
Options in the board's Router Advertisements. Traffic from other VLANs, other sites or the Internet cannot reach the
mesh.

This feature lets the board:

1. use an OMR prefix the owner chooses (for example a GUA /64 from their own space) instead of a generated or
   delegated one, optionally keeping the self-generated ULA alongside it;
2. tell the core router how to reach the mesh by one of three methods: DHCPv6 prefix delegation (today's behavior),
   RIPng, or BGP, or none of them;
3. stop the DHCPv6-PD client when another method (or none) is chosen, so it stops soliciting on a network that will
   never answer.

Success: with one BGP peer configured on the UDM Pro, Thread devices get an address from the custom OMR prefix (and
from the ULA when it is kept), and hosts anywhere the UDM Pro routes (VLANs, other sites over the tunnels, the
Internet) reach them. A router without BGP but with RIPng (for example OpenWrt with FRR or BIRD) gets the same result
with RIPng. Access control is the core router's firewall, not the board's.

## 2. Scope

In scope:

- an OMR prefix setting (auto or custom) and an "announce self-generated ULA" setting;
- a route method setting: `pd`, `ripng`, `bgp` or `off`. Exactly one is active (owner's decision);
- an announce-only BGP speaker with up to 2 peers (iBGP or eBGP);
- an announce-only RIPng sender;
- `GET/PUT /device/routing`, a Routing web page, the `routing show` console command;
- status in `/device/status`, the Routing page and `/metrics`;
- docs.

Non-goals:

- running two methods at once;
- more than one custom prefix, or arbitrary extra prefixes (the existing `/add_prefix` stays for that, unannounced);
- installing or using routes received from peers or RIPng neighbours (they are counted and dropped);
- a general-purpose router: no route reflection, no redistribution, no policy language, no split horizon logic beyond
  what announce-only needs;
- BGP over IPv4 transport, TCP-MD5 or TCP-AO authentication, graceful restart, route refresh, add-path, BFD;
- RIPng settings (interval, metric, route tag): fixed values only;
- OSPFv3, Babel;
- filtering traffic to the mesh.

Anti-goals (a failure even if it works):

- a misbehaving or chatty peer (for example a BGP peer that sends a full Internet table, or a RIPng neighbour flooding
  responses) exhausts memory or starves the OpenThread task;
- a routing setting can lock the owner out of the board;
- boards with no routing settings behave differently from today.

## 3. Decisions

| Topic | Decision |
| --- | --- |
| Route method | one of `pd` (default, today's behavior), `ripng`, `bgp`, `off`. Exactly one is active; switching stops the old one first (owner's decision) |
| DHCPv6-PD | on only with method `pd`. Every other method calls `otBorderRoutingDhcp6PdSetEnabled(false)`; `pd.state` then reads `disabled` |
| OMR prefix | `auto` (default, today's behavior: the delegated prefix while method is `pd` and a prefix is delegated, else the self-generated ULA) or `custom`: one /64 the owner gives, set through OpenThread's custom OMR config with preference medium. A custom prefix overrides PD |
| Self-generated ULA | the ULA OMR prefix OpenThread generates in `auto` mode (stable across reboots: OpenThread persists the /48 it is derived from) |
| "Announce self-generated ULA" | in `auto`: decides only whether RIPng or BGP announces the ULA. In `custom`: the board also publishes the ULA on the mesh as a SLAAC on-mesh prefix with preference low, so devices get both addresses and the custom prefix stays the favored one (*my call* on low: a medium ULA would sort below many GUAs and make OpenThread withdraw the custom prefix) |
| What is announced | with `ripng` or `bgp`: the custom OMR prefix (when set) and the ULA (when the box is ticked), each only while it is present in Network Data. The same announcement set feeds both methods |
| Session down | prefixes stay published on the mesh no matter what a BGP session or RIPng does (owner's decision) |
| Border router not running | in `SNIFFER`, `RADIO_FAULT`, or while Thread is disabled or detached, every route is withdrawn at once (BGP withdraw, RIPng metric 16); it is announced again when the border router runs and Thread is attached (*my call*) |
| Interface | the backbone interface: `thread_bb` in split mode, the single interface otherwise. Nothing is sent, and connections or requests arriving on `mgmt` are refused or ignored |
| Applying | changes apply live, without a reboot and without confirm-or-revert, because they cannot cut off management access |
| Storage | NVS, a namespace of its own; settings survive reboot and OTA. BGP settings are kept while another method is active |

BGP:

| Topic | Decision |
| --- | --- |
| Peers | at most 2. Each has an IPv6 address (global, or link-local on the backbone interface) and an ASN |
| iBGP / eBGP | local ASN equal to the peer's means iBGP, otherwise eBGP. 4-octet ASNs supported |
| Router ID | the board's IPv4 address on the backbone interface if it has one, else a configured value; with neither, BGP does not start and the status says why |
| Transport | IPv6 TCP only, port 179. The board both connects out and accepts connections; collisions are resolved as in RFC 4271 section 6.8 |
| Auth | none (lwIP has no TCP-MD5). The owner restricts the peer with a prefix-list and max-prefix on the router |
| Hold time | configurable, default 90 s, 0 or 3..65535 as in RFC 4271; keepalive is a third of the negotiated hold time |
| Inbound routes | read into the one per-session 4096-byte buffer, walked just far enough to count the prefixes in MP_REACH_NLRI (and IPv4 NLRI), then dropped; never installed or kept |
| Next-hop | the board's global address on the backbone interface plus its link-local (the 32-byte form, RFC 2545). With no global address, link-local alone in a 16-byte next-hop; that is outside RFC 2545, so it is supported only where the rig shows the peer accepts it (FRR, which the UDM Pro runs, is the target) |

RIPng (RFC 2080), no settings besides the method (*my calls* on every fixed value):

| Topic | Decision |
| --- | --- |
| Transport | UDP port 521 to and from, sent to `ff02::9` from the board's link-local address on the backbone interface, hop limit 255. The board joins `ff02::9` there to hear requests |
| Next-hop | implicit: the source address of the response (the board's link-local). No next-hop RTE |
| Route entries | prefix, prefix length 64, route tag 0, metric 1 |
| Regular updates | a full response every 30 s, with a random offset of 0..5 s so boards do not synchronise |
| Triggered updates | when the announcement set changes, a response with only the changed entries, at most one every 5 s |
| Withdrawal | a removed prefix is sent with metric 16 in the triggered update and in the next 4 regular updates, then dropped from the messages |
| Stopping | switching away from `ripng`, or the border router stopping, sends every announced prefix with metric 16 once, immediately |
| Requests | a whole-table request gets the full table back, unicast to the requester's address and port; a request for specific prefixes gets those entries (metric 16 for unknown ones). Requests from a source port other than 521 are answered too, as RFC 2080 says |
| Received responses | counted and dropped, never parsed beyond the header and the entry count |
| Router side | a router drops the route 180 s after the last update (RFC 2080 timeout), so a board that dies stops attracting traffic within 3 minutes. No authentication exists in RIPng; the router side limits what it accepts |

## 4. What OpenThread already does (checked in the ESP-IDF 6.1 sources)

`border_router/routing_manager.cpp`. This is what the design relies on:

- **DHCPv6-PD today.** The built-in PD client hands the delegated prefix to `PdPrefixManager::ProcessPrefix`. A prefix
  shorter than /64 is cut to its first /64. When several are offered, the numerically smallest wins (so GUA beats ULA).
  The prefix becomes the *local* OMR prefix with preference medium and is published in Network Data. It is withdrawn
  when deprecated, when it conflicts with an on-link or route prefix seen on the backbone, or when another border
  router's infrastructure-derived OMR prefix is favored (the PD client then goes `idle`). With no delegation, the local
  OMR prefix is the self-generated ULA with preference low.
- **Custom OMR.** `OT_BORDER_ROUTING_OMR_CONFIG_CUSTOM` makes the given prefix the local OMR prefix, ignoring PD. The
  routing manager withdraws its local OMR prefix whenever another valid OMR prefix in Network Data is favored (higher
  preference, or equal preference and numerically smaller). That is why the extra ULA is published at low preference.
- **"Infrastructure-derived"** means a favored OMR prefix with preference medium or higher. A medium custom prefix
  qualifies.
- **RIOs on the backbone come for free.** The board's RAs advertise a Route Information Option for every valid OMR
  prefix in Network Data, its own or another border router's, manually published ones included. Nothing extra is
  needed for hosts that honor RIOs; RIPng or BGP covers the router.
- **`::/0` into the mesh** (`RoutePublisher::Evaluate`) is published when either:
  - the favored OMR prefix is infrastructure-derived and the received RAs contain a default route or a non-ULA route
    prefix; or
  - the received RAs contain a non-ULA on-link prefix.

  Otherwise OpenThread publishes only `fc00::/7` (when a ULA on-link prefix is seen) or nothing. OpenThread sets the
  default-route flag on its local OMR prefix only while it publishes `::/0`.
- **Checked on the owner's network (2026-11-03):** the board's backbone (untagged VLAN) carries RAs from the UDM Pro
  with a ULA (`fdaa:1::/64`) and a GUA (`2a06:9801:c7:aa01::/64`) on-link prefix, so `::/0` is published there. The
  mesh's OMR prefix is the self-generated ULA `fd98:2656:4740:1::/64`.

## 5. Architecture

New component `p4/components/routing/`, symbol prefix `routing_`:

- **Announcement set** (pure, host-tested, protocol-neutral): computes the set of prefixes to announce from the
  settings, the custom and ULA prefixes, which of them are in Network Data, and the border-router state. It also
  computes the diff against what was last announced. Both senders consume it; nothing in it is BGP- or RIPng-specific.
- **BGP message codec** (pure, host-tested): it encodes OPEN (Multiprotocol IPv6 unicast and 4-octet ASN
  capabilities), KEEPALIVE, UPDATE (ORIGIN IGP; AS_PATH empty for iBGP, `[local ASN]` for eBGP; LOCAL_PREF 100 on
  iBGP only; MP_REACH_NLRI / MP_UNREACH_NLRI for AFI 2 / SAFI 1) and NOTIFICATION. It decodes the header of every
  message, OPEN and NOTIFICATION enough to validate and negotiate, and walks UPDATE attributes just far enough to count
  NLRI. Messages are at most 4096 bytes, since extended messages are not advertised.
- **BGP session state machine** (pure, host-tested, driven by events and a clock). It covers the RFC 4271 states
  Idle, Connect, Active, OpenSent, OpenConfirm and Established, plus hold, keepalive and connect-retry timers, with
  exponential backoff from 5 s up to 120 s between attempts (*my call*). It outputs actions (connect, send, close,
  announce set changed) and makes no socket calls.
- **RIPng codec and scheduler** (pure, host-tested, driven by events and a clock): encodes responses (header, one
  20-byte RTE per prefix, split across messages so each fits the backbone MTU), decodes request and response headers,
  and decides when to send regular and triggered updates and how long a withdrawn prefix keeps metric 16. It makes no
  socket calls.
- **Prefix publisher** (OpenThread glue), all calls under the OpenThread lock:
  - learns the self-generated ULA from `otBorderRoutingGetOmrPrefix` while the OMR config is `auto` and no PD prefix
    is in use (at border-router init, before any custom config is applied), and keeps it for the session;
  - `auto`: sets `OT_BORDER_ROUTING_OMR_CONFIG_AUTO` and removes the extra ULA if it published one;
  - `custom`: sets `OT_BORDER_ROUTING_OMR_CONFIG_CUSTOM` with the prefix and preference medium. With the ULA box
    ticked it also adds the ULA with `otBorderRouterAddOnMeshPrefix` (preferred, SLAAC, on-mesh, stable, preference
    low; the default-route flag follows whether OpenThread currently publishes `::/0`) and calls
    `otBorderRouterRegister`; with the box clear it removes it;
  - turns the DHCPv6-PD client on for method `pd` and off for every other method.

  Existing endpoints: `/add_prefix` and `/delete_prefix` (Addresses page) publish SLAAC on-mesh prefixes through the
  same OpenThread call, unannounced. The publisher adds and removes only the ULA entry it owns; prefixes added through
  `/add_prefix` are left alone and not announced (*my call*). `/add_ipaddr` and `/delete_ipaddr` only add an address
  to the board's own Thread interface (`otIp6AddUnicastAddress`), never reach Network Data, and are unrelated.
- **Routing task**: one FreeRTOS task that owns the sockets and runs whichever sender the method selects:
  - BGP: one listening TCP socket and one per peer (plus a short-lived socket while a collision is resolved);
  - RIPng: one UDP socket bound to port 521 on the backbone interface.

  It reads the OpenThread state through a callback or a periodic check, and never holds the OpenThread lock while
  blocking on a socket. Switching method stops the old sender (BGP Cease, RIPng metric 16) and closes its sockets
  before the new one starts.
- **Settings**: load, validate and store in NVS; parse and render the JSON shape in section 6.

Changes elsewhere:

- `br_core`: the hard-coded `otBorderRoutingDhcp6PdSetEnabled(instance, true)` in `br_core.c` follows the stored
  method instead. After border router init, br_core hands control to the prefix publisher.
- `device_api`: `GET /device/routing` and `PUT /device/routing`, and a `routing` object in `/device/status`. The route
  table registers 16 routes today plus the catch-all (17), while `EXTRA_HANDLER_SLOTS` in `esp_br_web.c` is 16 (the
  `+30` slack in `max_uri_handlers` hides the shortfall). It rises to at least 19. That file is vendored, so the
  change gets logged in `PATCHES.md`.
- `netcfg`: the routing task asks netcfg for the backbone netif and its addresses, and uses its rules to refuse port
  179 and ignore port 521 on `mgmt` in split mode.
- `metrics`: new series, listed in section 8.
- Frontend: a new `routing.html` page and an entry in the navigation.
- `p4/main/console.c`: `routing show`.

Sockets: `CONFIG_LWIP_MAX_SOCKETS` is 10 today and the HTTP server uses most of them. BGP needs up to 4 at once,
RIPng 1, never both. The builder raises the limit as needed and checks that the HTTP server still gets its share. RIPng
needs MLD on the backbone netif to join `ff02::9`.

## 6. Settings and API

Shape of `GET /device/routing`, the body `PUT /device/routing` accepts (with `status` ignored):

```
{
  "method": "bgp",
  "omr": {"mode": "custom", "prefix": "2001:db8:42:100::/64"},
  "announce_ula": true,
  "bgp": {
    "local_asn": 4200000001,
    "router_id": null,
    "hold_time": 90,
    "peers": [ {"address": "2001:db8:42::1", "asn": 4200000001} ]
  },
  "status": {
    "omr_prefix": "2001:db8:42:100::/64",
    "ula_prefix": "fd98:2656:4740:1::/64",
    "announced": ["2001:db8:42:100::/64", "fd98:2656:4740:1::/64"],
    "bgp": {
      "router_id": "192.168.20.15",
      "blocked": null,
      "peers": [ {
        "address": "2001:db8:42::1",
        "state": "established",
        "uptime_s": 8123,
        "last_error": null,
        "received_dropped": 3
      } ]
    },
    "ripng": null
  }
}
```

With method `ripng`, `status.bgp` is null and `status.ripng` is
`{"last_update_s_ago", "updates_sent", "requests_answered", "responses_dropped"}`.

`status.omr_prefix` is the local OMR prefix in use (custom, delegated or ULA), `status.ula_prefix` the self-generated
ULA, each null when unknown. `status.announced` is the current announcement set (empty with method `pd` or `off`).
`status.bgp.blocked` is null, or the reason sessions do not run (for example `"no router id"`).

Validation. A PUT that fails any of these checks returns 400 naming the field and changes nothing:

- `method`: `pd`, `ripng`, `bgp` or `off`;
- `omr.mode`: `auto` or `custom`. `omr.prefix`: required for `custom`, ignored for `auto`; a /64, not `::/64`, not link-local
  (`fe80::/10`) or multicast (`ff00::/8`);
- `bgp` is validated whenever present, whatever the method, so stored BGP settings are always valid:
  - `local_asn` and `asn`: 1..4294967295, excluding 23456 (AS_TRANS);
  - `router_id`: null or a dotted IPv4 address other than `0.0.0.0`;
  - `hold_time`: 0 or 3..65535;
  - `peers`: 0..2 entries with unique addresses. A link-local address is scoped to the backbone interface.
- method `bgp` with no peers.

A custom prefix equal to the self-generated ULA is accepted; the ULA box then adds nothing (*my call*).

Switching method away from `bgp` closes every session with a NOTIFICATION Cease (6/2) and frees the listening socket.
A PUT that changes the peer list, the ASNs or the router ID resets the affected sessions. A PUT that only changes
`omr` or `announce_ula` sends UPDATEs (BGP) or a triggered update (RIPng) with no reset.

BGP `state` values: `idle`, `connect`, `active`, `opensent`, `openconfirm`, `established`. `last_error` is null, or a
short text such as `"notification 6/2 received"`, `"connect: host unreachable"` or `"hold timer expired"`.

`/device/status` gains `routing: {"method", "omr_mode", "announced_count", "peers_established", "peers_configured"}`
(the peer fields null unless the method is `bgp`). The existing `pd` object stays.

## 7. BDD skeleton

```
routing_announce (announcement set)
    compute
        Custom OMR, ULA box ticked, both in Network Data, border router running and attached
            - returns the custom prefix and the ULA
        Custom OMR, ULA box clear
            - returns the custom prefix only
        Auto OMR, ULA box ticked, ULA in Network Data
            - returns the ULA
        Auto OMR, ULA box clear
            - returns an empty set
        A selected prefix is not in Network Data (another border router's prefix is favored)
            - leaves that prefix out
        Border router not running or Thread detached
            - returns an empty set
        Method pd or off
            - returns an empty set
    diff
        Set changed
            - returns the added and removed prefixes separately
        Set unchanged
            - returns no change

routing_bgp_msg (codec)
    encode_open
        With a 4-octet local ASN
            - writes AS_TRANS (23456) in My AS and the real ASN in the 4-octet capability
            - includes the Multiprotocol capability for AFI 2 / SAFI 1
    encode_update
        Announcing prefixes on iBGP
            - carries ORIGIN IGP, an empty AS_PATH and LOCAL_PREF 100
            - carries MP_REACH_NLRI with a 32-byte next-hop (global + link-local) and every prefix
        Announcing prefixes on eBGP
            - carries AS_PATH [local ASN] and no LOCAL_PREF
        Without a global address
            - carries a 16-byte link-local next-hop
        Withdrawing prefixes
            - carries MP_UNREACH_NLRI only, with no path attributes besides it
    decode_header
        Valid header
            - returns type and length
        Marker not all ones
            - returns a header error (NOTIFICATION 1/1)
        Length below 19 or above 4096
            - returns a header error (NOTIFICATION 1/2)
    count_update_nlri
        UPDATE with MP_REACH_NLRI and IPv4 NLRI
            - returns the number of prefixes in both
        Attribute length runs past the message
            - returns an UPDATE error (NOTIFICATION 3/1)
        Prefix length above 128 inside MP_REACH_NLRI
            - returns an UPDATE error (NOTIFICATION 3/10)
    decode_open
        Peer ASN differs from the configured one
            - returns an OPEN error (NOTIFICATION 2/2)
        Peer offers hold time 1 or 2
            - returns an OPEN error (NOTIFICATION 2/6)
        Peer lacks IPv6 unicast
            - returns an OPEN error (NOTIFICATION 2/7)
        Valid OPEN
            - returns the peer ASN (from the 4-octet capability when present), router ID and hold time

routing_bgp_fsm (session state machine)
    start
        Peer enabled
            - moves to Connect and asks for an outgoing connection
    connected
        Outgoing or incoming connection succeeds
            - sends OPEN and moves to OpenSent
    connect failed
        Connection refused or timed out
            - moves to Active and schedules a retry after the current backoff
            - doubles the backoff, up to 120 s
    open received
        Valid OPEN in OpenSent
            - sends KEEPALIVE, moves to OpenConfirm, negotiates hold time as the smaller of the two
        Invalid OPEN
            - sends the matching NOTIFICATION, closes, moves to Idle and schedules a retry
    keepalive received
        In OpenConfirm
            - moves to Established, resets the backoff, sends the full announcement set
        In Established
            - restarts the hold timer
    update received
        In Established
            - increments received_dropped by the number of announced NLRI and restarts the hold timer
            - installs nothing
    hold timer expires
        Negotiated hold time is not 0
            - sends NOTIFICATION 4/0, closes, moves to Idle, sets last_error "hold timer expired"
    notification received
        Any state
            - closes, moves to Idle, records the code and subcode in last_error, schedules a retry
    collision
        Incoming connection while an outgoing one is in OpenSent or OpenConfirm
            - keeps the connection opened by the side with the higher router ID and closes the other with NOTIFICATION 6/7
    announcement set changed
        In Established
            - sends one UPDATE withdrawing removed prefixes and one announcing added prefixes
        Not established
            - sends nothing; the full set goes out on the next Established
    stop
        Method changed away from bgp, or peer removed
            - sends NOTIFICATION 6/2 (or 6/3 for a removed peer) when connected, closes, stays in Idle

routing_ripng (codec and scheduler)
    encode_response
        Announcement set of two prefixes
            - writes command 2, version 1, and one RTE per prefix with length 64, tag 0, metric 1
        Withdrawn prefix
            - writes its RTE with metric 16
        More entries than fit one message
            - splits them across messages that each fit the backbone MTU
    decode
        Datagram shorter than the 4-byte header, or version not 1
            - is dropped and counted
        Response from a neighbour
            - counts it in responses_dropped and changes nothing
        Whole-table request (one RTE, prefix ::/0, length 0, metric 16)
            - returns a full-table response addressed to the requester's address and port
        Request for specific prefixes
            - returns those entries, metric 1 for announced ones and 16 for the others
    schedule
        Started
            - sends a full response at once, then every 30 s plus a random 0..5 s
        Announcement set changed
            - sends a triggered response with only the changed entries, no sooner than 5 s after the previous triggered one
        Prefix removed
            - sends it with metric 16 in the triggered update and the next 4 regular updates, then leaves it out
        Stopped (method changed or border router stopped)
            - sends every announced prefix with metric 16 once, at once, and sends nothing after

routing_publisher (mesh side)
    init
        Border router started
            - records the self-generated ULA before applying a custom OMR config
    apply
        Custom OMR, ULA box clear
            - sets the custom OMR config with the prefix and preference medium
            - removes the ULA on-mesh entry if it added one
        Custom OMR, ULA box ticked
            - sets the custom OMR config with preference medium
            - adds the ULA as a SLAAC on-mesh prefix with preference low and registers Network Data
            - the custom prefix stays the favored OMR prefix
        Auto OMR
            - restores the automatic OMR config and removes the ULA on-mesh entry if it added one
        Prefix added through /add_prefix
            - is left untouched
        BGP session down or no RIPng neighbour
            - changes nothing on the mesh
    default route flag
        OpenThread starts or stops publishing ::/0
            - the ULA on-mesh entry's default-route flag follows it
    apply_method
        Method pd
            - enables the DHCPv6-PD client, as today
        Method ripng, bgp or off
            - disables the DHCPv6-PD client; pd.state reads "disabled"

routing_settings
    put
        Valid body
            - stores the settings in NVS, applies them live, returns 200 with the new GET shape
        Method changed
            - stops the old method before starting the new one
        Method bgp with no peers
            - returns 400 naming bgp.peers
        Custom mode without a prefix, or a prefix that is not a valid /64
            - returns 400 naming omr.prefix
        Any other invalid field (each rule in section 6)
            - returns 400 naming the field, stores and changes nothing
        Only omr or announce_ula changed
            - does not reset BGP sessions; RIPng sends a triggered update
    load
        Nothing stored
            - returns defaults: method pd, omr auto, announce_ula false, no peers, hold time 90
        Stored settings
            - returns them unchanged after a reboot

routing task (target)
    bgp listening
        Split mode, connection arrives on mgmt
            - refuses it
        Connection from an address that is not a configured peer
            - closes it without sending anything
    bgp router id
        No IPv4 on the backbone and no configured router ID
            - does not start sessions; status.bgp.blocked explains why
    bgp inbound flood
        Peer sends a large table
            - free heap does not fall; the OpenThread task keeps running; received_dropped counts it
    ripng
        Method ripng on the backbone
            - multicasts responses to ff02::9 port 521 from the link-local address, hop limit 255
        Split mode
            - sends nothing on mgmt and ignores datagrams arriving there
        Neighbour floods responses
            - free heap does not fall; responses_dropped counts them
    border router stops
        Switching to sniffer mode
            - withdraws every route within one second (BGP withdraw or RIPng metric 16)
```

## 8. Status surfaces

- **Routing page** (`routing.html`), in the look of the Ethernet page:
  - "How your router reaches the mesh": four mode cards, DHCPv6 PD, RIPng, BGP and Off, with an
    "In use now" chip on the active one and a status chip (PD state, RIPng last update, BGP sessions up);
  - "OMR prefix": Automatic and Custom mode cards (the prefix field only for Custom), the "announce self-generated
    ULA" box, and a facts list: OMR prefix in use, board's ULA, whether the default route into the mesh is published,
    and what is announced;
  - "BGP" (only while the BGP card is picked): the board's fields (local ASN, router ID, hold time), up to 2 peer rows
    (Remove, Add peer) and the sessions table (state, uptime, received and dropped, last error);
  - "RIPng" (only while the RIPng card is picked): no settings, only status (last update, updates sent, requests
    answered, responses dropped) and a hint that the router keeps the route for 3 minutes after the board goes quiet;
  - a bottom action bar with "Discard changes" and "Save". There is no review step and no reboot.

  It saves with `PUT /device/routing` and refreshes the status every 5 s. The agreed layout is the static mockup
  `2026-11-03-routing-page-mockup.html` next to this document, which uses the real `style.css`. Its yellow "mockup
  controls" strip and its failing second BGP peer are illustration only.
- **Console:** `routing show` prints the method, the prefixes in use, the announcement set, and then either one line
  per BGP peer (state, uptime, last error, counters) or the RIPng counters.
- **Metrics:**
  - `br_routing_method{method}`: a state set;
  - `br_routing_announced_prefixes`;
  - `br_routing_omr_custom`: 1 while a custom OMR prefix is configured;
  - `br_bgp_peer_state{peer,state}`: a state set;
  - `br_bgp_peer_uptime_seconds{peer}`;
  - `br_bgp_received_dropped_total{peer}`;
  - `br_bgp_session_resets_total{peer}`;
  - `br_ripng_updates_sent_total`, `br_ripng_requests_answered_total`, `br_ripng_responses_dropped_total`.

## 9. Tests

- Host tests in `p4/components/routing/test_host/` for the announcement set, the BGP codec and state machine, the
  RIPng codec and scheduler, and settings validation, one test per effect in section 7, named as the styleguide says.
- Codec test vectors are byte arrays checked against a reference. *My call:* capture them once from FRR or BIRD
  and keep them as fixtures.
- Interop on the Docker VLAN rig, with FRR and BIRD containers:
  - BGP, iBGP and eBGP: the routes appear with the right next-hop; they are withdrawn when the board enters sniffer
    mode; switching the OMR mode or the ULA box causes no session reset; a full-table feed from the peer does not hurt
    the board;
  - RIPng: the routes appear with the board's link-local as next-hop and metric 2 on the router (1 + the router's
    cost); a changed set shows up within 5 s; switching to `off` makes the router drop them at once (metric 16), not
    after 180 s;
  - with a custom prefix and the ULA box ticked, a Thread device (the C6 peer) holds addresses from both.

  This runs in the batched on-board pass, not during each task.
- A pytest target test for `PUT /device/routing` validation (including method `bgp` with no peers) and for every
  method other than `pd` showing `pd.state` `disabled`.

## 10. Docs

- `docs/networking.md`: the DHCPv6-PD section points to the method setting. A new section covers the OMR prefix
  setting, the ULA box and the three methods, with example router configs: FRR for a UDM Pro BGP peer (neighbor, a
  prefix-list limited to the announced /64s, maximum-prefix, no export to the board) and FRR or BIRD for RIPng.
- `docs/api.md`: `/device/routing` and the new `/device/status` field.
- `docs/metrics.md`: the new series.

## 11. Constraints

- ESP-IDF 6.1 and its OpenThread. `otBorderRoutingSetOmrConfig` with the custom mode exists in this version.
- lwIP sockets only. No TCP-MD5. MLD must be on for the backbone netif.
- A peer or neighbour that sends a lot must not hurt the board: BGP inbound handling is streaming and bounded by one
  4096-byte buffer per session; RIPng reads into one datagram buffer and drops responses after the header.
- Follow `STYLEGUIDE.md`: `esp_err_t` returns, the `routing_` prefix, Unity host tests, Makefile targets.

## 12. Left to the builder

- File layout inside `routing/`, task priority and stack size, and the exact socket limit.
- How the routing task learns that Network Data, the OMR prefix or the border-router state changed (an OpenThread
  state callback or polling), as long as withdrawal on a mode switch happens within one second.
- How the publisher tells whether OpenThread currently publishes `::/0`.
- How link-local BGP peer addresses and the RIPng socket are scoped to the backbone netif in lwIP.
- Field labels and layout details on the Routing page within the mockup.
- Checking on the rig that a link-local-only BGP next-hop is accepted by FRR (and noting the result for BIRD).

## 13. Follow-ups (not in this design)

- Installing received routes (a lwIP route hook), for networks with more than one router.
- TCP-MD5 if lwIP ever supports it.
- RIPng settings (metric, interval) if anyone needs them.
- A troubleshooting switch that stops the board sending Router Advertisements: a `--wrap` of
  `otPlatInfraIfSendIcmp6Nd` (in the prebuilt `libopenthread_br.a`) dropping ICMPv6 type 134, not stored.
