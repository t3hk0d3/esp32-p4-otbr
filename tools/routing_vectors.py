"""Reference messages for the routing host tests, built with scapy.

Usage: make routing-vectors
  (runs: uv run --no-project --with scapy==2.6.1 python tools/routing_vectors.py <out>)

Writes p4/components/routing/test_host/main/vectors.h: one `static const uint8_t` array
per message. scapy is an independent BGP implementation, so the codec's output is
compared with bytes it did not produce. Every vector is parsed back with scapy and its
fields are asserted before the header is written; the script fails on any mismatch.

Malformed vectors start from a scapy message and patch the one field that makes them
malformed; the patch is commented where it is made.
"""

import argparse
import sys
from pathlib import Path

import scapy
from scapy.contrib import bgp
from scapy.contrib.bgp import (
    BGP,
    BGPAuthenticationInformation,
    BGPCapability,
    BGPCapFourBytesASN,
    BGPCapGeneric,
    BGPCapMultiprotocol,
    BGPHeader,
    BGPKeepAlive,
    BGPNLRI_IPv4,
    BGPNLRI_IPv6,
    BGPNotification,
    BGPOpen,
    BGPOptParam,
    BGPPAAS4BytesPath,
    BGPPACommunity,
    BGPPALocalPref,
    BGPPAMPReachNLRI,
    BGPPAMPUnreachNLRI,
    BGPPAMPUnreachNLRI_IPv6,
    BGPPANextHop,
    BGPPAOrigin,
    BGPPathAttr,
    BGPUpdate,
)
from scapy.packet import Raw

# We advertise the 4-octet ASN capability, so AS_PATH carries 4-octet ASNs (RFC 6793).
bgp.bgp_module_conf.use_2_bytes_asn = False

AS_TRANS = 23456
ASN4 = 4200000001
ASN2 = 65010
HOLD = 90
ROUTER_ID = "192.168.1.117"
PREFIXES = ["2001:db8:42:100::/64", "fd98:2656:4740:1::/64"]
NH_GLOBAL = "2001:db8:42::17"
NH_LINK_LOCAL = "fe80::82f1:b2ff:fed2:cafc"

TRANSITIVE = "Transitive"  # 0x40: well-known transitive
OPTIONAL = "Optional"  # 0x80: optional non-transitive


def check(cond, what):
    if not cond:
        sys.exit(f"cross-check failed: {what}")


# --- builders ---


def caps_bytes(*caps):
    """Capabilities back to back, as one Capabilities parameter carries them."""
    return b"".join(bytes(c) for c in caps)


def open_msg(my_as, hold, bgp_id, params, version=4):
    return bytes(
        BGPHeader(type=1)
        / BGPOpen(
            version=version,
            my_as=my_as,
            hold_time=hold,
            bgp_id=bgp_id,
            opt_params=params,
        )
    )


def cap_param(*caps):
    """One Optional Parameter of type 2 (Capabilities) carrying `caps`."""
    value = caps[0]
    for c in caps[1:]:
        value = value / c
    return BGPOptParam(param_type=2, param_value=value)


def our_open(local_asn):
    """The OPEN the codec sends: one Capabilities parameter, MP IPv6 unicast first."""
    my_as = local_asn if local_asn <= 0xFFFF else AS_TRANS
    param = cap_param(
        BGPCapMultiprotocol(afi=2, safi=1), BGPCapFourBytesASN(asn=local_asn)
    )
    return open_msg(my_as, HOLD, ROUTER_ID, [param])


def mp_reach(nlri, link_local_only=False, flags=OPTIONAL):
    if link_local_only:
        attr = BGPPAMPReachNLRI(
            afi=2, safi=1, nh_addr_len=16, nh_v6_addr=NH_LINK_LOCAL, nlri=nlri
        )
    else:
        attr = BGPPAMPReachNLRI(
            afi=2,
            safi=1,
            nh_addr_len=32,
            nh_v6_global=NH_GLOBAL,
            nh_v6_link_local=NH_LINK_LOCAL,
            nlri=nlri,
        )
    return BGPPathAttr(type_flags=flags, type_code="MP_REACH_NLRI", attribute=attr)


def v6_nlri(prefixes):
    return [BGPNLRI_IPv6(prefix=p) for p in prefixes]


def origin_igp():
    return BGPPathAttr(
        type_flags=TRANSITIVE, type_code="ORIGIN", attribute=BGPPAOrigin(origin="IGP")
    )


def as_path(asns):
    segments = []
    if asns:
        segments = [BGPPAAS4BytesPath.ASPathSegment(segment_type=2, segment_value=asns)]
    return BGPPathAttr(
        type_flags=TRANSITIVE,
        type_code="AS_PATH",
        attribute=BGPPAAS4BytesPath(segments=segments),
    )


def local_pref_100():
    return BGPPathAttr(
        type_flags=TRANSITIVE,
        type_code="LOCAL_PREF",
        attribute=BGPPALocalPref(local_pref=100),
    )


def our_update(ibgp, local_asn, link_local_only=False):
    """The UPDATE the codec sends: ORIGIN, AS_PATH, LOCAL_PREF (iBGP), MP_REACH_NLRI."""
    attrs = [origin_igp(), as_path([] if ibgp else [local_asn])]
    if ibgp:
        attrs.append(local_pref_100())
    attrs.append(mp_reach(v6_nlri(PREFIXES), link_local_only))
    return bytes(BGPHeader(type=2) / BGPUpdate(path_attr=attrs))


def our_withdraw():
    unreach = BGPPAMPUnreachNLRI(
        afi=2,
        safi=1,
        afi_safi_specific=BGPPAMPUnreachNLRI_IPv6(withdrawn_routes=v6_nlri(PREFIXES)),
    )
    attr = BGPPathAttr(
        type_flags=OPTIONAL, type_code="MP_UNREACH_NLRI", attribute=unreach
    )
    return bytes(BGPHeader(type=2) / BGPUpdate(path_attr=[attr]))


RX_V6 = ["2001:db8:aa::/48", "2001:db8:bb:1::/64", "2001:db8:cc::1/128"]
RX_V4 = ["198.51.100.0/24", "203.0.113.7/32"]


def rx_update():
    """What a router may send: an IPv4 withdrawal, IPv4 NEXT_HOP, a COMMUNITY attribute,
    MP_REACH_NLRI with the extended-length flag (0x90) and 3 IPv6 NLRI, then 2 IPv4 NLRI.
    """
    attrs = [
        origin_igp(),
        as_path([65020, 4200000009]),
        BGPPathAttr(
            type_flags=TRANSITIVE,
            type_code="NEXT_HOP",
            attribute=BGPPANextHop(next_hop="192.0.2.1"),
        ),
        BGPPathAttr(
            type_flags="Optional+Transitive",
            type_code="COMMUNITY",
            attribute=BGPPACommunity(community=0xFDFC0001),
        ),
        mp_reach(v6_nlri(RX_V6), flags="Optional+Extended-Length"),
    ]
    return bytes(
        BGPHeader(type=2)
        / BGPUpdate(
            withdrawn_routes=[BGPNLRI_IPv4(prefix="192.0.2.0/24")],
            path_attr=attrs,
            nlri=[BGPNLRI_IPv4(prefix=p) for p in RX_V4],
        )
    )


def rx_update_attr_overrun():
    """ORIGIN, then LOCAL_PREF whose attribute length (200, the value is 4 bytes) runs past
    the attribute list and the message."""
    raw = bytearray(
        bytes(BGPHeader(type=2) / BGPUpdate(path_attr=[origin_igp(), local_pref_100()]))
    )
    # header 19, withdrawn length 2, attribute length 2, ORIGIN 4, then LOCAL_PREF's
    # flags (27) and type (28): its length byte is raw[29]
    raw[29] = 200
    return bytes(raw)


def rx_update_prefix_129():
    """MP_REACH_NLRI whose only NLRI has prefix length 129, followed by 17 bytes."""
    attr = BGPPathAttr(
        type_flags=OPTIONAL,
        type_code="MP_REACH_NLRI",
        attribute=BGPPAMPReachNLRI(
            afi=2, safi=1, nh_addr_len=16, nh_v6_addr=NH_LINK_LOCAL, nlri=[]
        )
        # NLRI: length byte 129 (0x81, above 128), then ceil(129 / 8) = 17 bytes
        / Raw(b"\x81" + bytes(17)),
    )
    return bytes(BGPHeader(type=2) / BGPUpdate(path_attr=[origin_igp(), attr]))


def header_only(length, msg_type):
    """A 19-byte header whose length field is `length` (patched below 19 or above 4096)."""
    return bytes(BGPHeader(len=length, type=msg_type))


def bad_marker():
    """KEEPALIVE whose 6th marker byte is 0xfe."""
    b = bytearray(bytes(BGPKeepAlive()))
    b[5] = 0xFE
    return bytes(b)


def build():
    """(name, comment, bytes) for every BGP vector."""
    mp6 = BGPCapMultiprotocol(afi=2, safi=1)
    mp4 = BGPCapMultiprotocol(afi=1, safi=1)
    split = [
        cap_param(mp4),
        cap_param(mp6, BGPCapability(code=2)),  # route refresh, length 0
        cap_param(BGPCapFourBytesASN(asn=65020)),
        cap_param(BGPCapGeneric(code=73, cap_data=b"\x03udm\x00")),  # FQDN, unknown
    ]
    auth = BGPOptParam(
        param_type=1,  # Authentication (deprecated): unsupported
        authentication_data=BGPAuthenticationInformation(
            authentication_code=1, authentication_data=b"ab"
        ),
    )
    as4_cap = cap_param(mp6, BGPCapFourBytesASN(asn=65020))
    return [
        (
            "BGP_OPEN_AS4",
            f"OPEN, ASN {ASN4} (My AS {AS_TRANS}), hold 90, id {ROUTER_ID}",
            our_open(ASN4),
        ),
        ("BGP_OPEN_AS2", f"OPEN, ASN {ASN2}, hold 90, id {ROUTER_ID}", our_open(ASN2)),
        ("BGP_KEEPALIVE", "KEEPALIVE", bytes(BGPKeepAlive())),
        (
            "BGP_UPDATE_IBGP",
            f"UPDATE iBGP (ASN {ASN2}), next-hop global + link-local",
            our_update(True, ASN2),
        ),
        (
            "BGP_UPDATE_EBGP",
            f"UPDATE eBGP (ASN {ASN4}), next-hop global + link-local",
            our_update(False, ASN4),
        ),
        (
            "BGP_UPDATE_LL_ONLY",
            f"UPDATE iBGP (ASN {ASN2}), next-hop link-local only",
            our_update(True, ASN2, link_local_only=True),
        ),
        (
            "BGP_WITHDRAW",
            "UPDATE withdrawing both prefixes (MP_UNREACH_NLRI only)",
            our_withdraw(),
        ),
        (
            "BGP_NOTIFICATION_6_2",
            "NOTIFICATION Cease / Administrative Shutdown",
            bytes(BGPHeader(type=3) / BGPNotification(error_code=6, error_subcode=2)),
        ),
        (
            "BGP_RX_UPDATE_5_NLRI",
            "received UPDATE: 3 IPv6 NLRI and 2 IPv4 NLRI",
            rx_update(),
        ),
        (
            "BGP_RX_OPEN_SPLIT_CAPS",
            "received OPEN, ASN 65020, hold 180, capabilities in 4 parameters, one unknown",
            open_msg(65020, 180, "192.168.1.1", split),
        ),
        (
            "BGP_RX_OPEN_NO_AS4",
            "received OPEN, My AS 65020, MP IPv6 only, no 4-octet ASN",
            open_msg(65020, 90, "192.168.1.1", [cap_param(mp6)]),
        ),
        (
            "BGP_RX_OPEN_HOLD0",
            "received OPEN, ASN 65020, hold 0",
            open_msg(65020, 0, "192.168.1.1", [as4_cap]),
        ),
        (
            "BGP_RX_OPEN_HOLD1",
            "received OPEN, ASN 65020, hold 1",
            open_msg(65020, 1, "192.168.1.1", [as4_cap]),
        ),
        (
            "BGP_RX_OPEN_HOLD2",
            "received OPEN, ASN 65020, hold 2",
            open_msg(65020, 2, "192.168.1.1", [as4_cap]),
        ),
        (
            "BGP_RX_OPEN_NO_IPV6",
            "received OPEN, ASN 65020, MP IPv4 unicast only",
            open_msg(
                65020,
                90,
                "192.168.1.1",
                [cap_param(mp4, BGPCapFourBytesASN(asn=65020))],
            ),
        ),
        (
            "BGP_RX_OPEN_VERSION3",
            "received OPEN, version 3",
            open_msg(65020, 90, "192.168.1.1", [as4_cap], version=3),
        ),
        (
            "BGP_RX_OPEN_ID_ZERO",
            "received OPEN, BGP identifier 0.0.0.0",
            open_msg(65020, 90, "0.0.0.0", [as4_cap]),
        ),
        (
            "BGP_RX_OPEN_AUTH_PARAM",
            "received OPEN with an Authentication parameter",
            open_msg(65020, 90, "192.168.1.1", [as4_cap, auth]),
        ),
        ("BGP_RX_BAD_MARKER", "KEEPALIVE with a marker byte 0xfe", bad_marker()),
        ("BGP_RX_LEN_18", "header with length 18", header_only(18, 4)),
        ("BGP_RX_LEN_4097", "header with length 4097", header_only(4097, 2)),
        (
            "BGP_RX_KEEPALIVE_LEN_20",
            "KEEPALIVE header with length 20",
            header_only(20, 4),
        ),
        (
            "BGP_RX_TYPE_5",
            "ROUTE-REFRESH header (type 5, not negotiated)",
            header_only(23, 5),
        ),
        (
            "BGP_RX_UPDATE_ATTR_OVERRUN",
            "received UPDATE, attribute length past the end",
            rx_update_attr_overrun(),
        ),
        (
            "BGP_RX_UPDATE_PREFIX_129",
            "received UPDATE, MP_REACH_NLRI prefix length 129",
            rx_update_prefix_129(),
        ),
    ]


# --- cross-checks: every vector parsed back with scapy ---


def caps_of(pkt):
    """Every capability of a parsed OPEN, across all Capabilities parameters."""
    out = []
    for param in pkt[BGPOpen].opt_params:
        if param.param_type != 2:
            continue
        data = bytes(param.param_value)
        while data:
            out.append(BGPCapability(data))
            data = data[2 + data[1] :]
    return out


def check_open(name, raw, version, my_as, hold, bgp_id, asn4, mp):
    p = BGP(raw)
    o = p[BGPOpen]
    check(p.len == len(raw) and p.type == 1, f"{name}: header")
    check(o.version == version, f"{name}: version")
    check(o.my_as == my_as and o.hold_time == hold, f"{name}: My AS / hold")
    check(o.bgp_id == bgp_id, f"{name}: BGP identifier")
    caps = caps_of(p)
    got_asn4 = [c.asn for c in caps if isinstance(c, BGPCapFourBytesASN)]
    check(got_asn4 == ([asn4] if asn4 is not None else []), f"{name}: 4-octet ASN")
    got_mp = [(c.afi, c.safi) for c in caps if isinstance(c, BGPCapMultiprotocol)]
    check(got_mp == mp, f"{name}: multiprotocol")


def attrs_of(p):
    return {a.type_code: a for a in p[BGPUpdate].path_attr}


def check_our_update(name, raw, ibgp, local_asn, link_local_only):
    p = BGP(raw)
    check(p.len == len(raw) and p.type == 2, f"{name}: header")
    u = p[BGPUpdate]
    check(u.withdrawn_routes_len == 0 and not u.nlri, f"{name}: no IPv4 routes")
    codes = [a.type_code for a in u.path_attr]
    want = [1, 2, 5, 14] if ibgp else [1, 2, 14]
    check(codes == want, f"{name}: attribute order {codes}")
    flags = [int(a.type_flags) for a in u.path_attr]
    check(flags == [0x40] * (len(want) - 1) + [0x80], f"{name}: flags {flags}")
    a = attrs_of(p)
    check(a[1].attribute.origin == 0, f"{name}: ORIGIN IGP")
    segs = a[2].attribute.segments
    if ibgp:
        check(a[2].attr_len == 0 and not segs, f"{name}: empty AS_PATH")
        check(a[5].attribute.local_pref == 100, f"{name}: LOCAL_PREF 100")
    else:
        check(
            len(segs) == 1
            and segs[0].segment_type == 2
            and segs[0].segment_value == [local_asn],
            f"{name}: AS_PATH [{local_asn}]",
        )
    mp = a[14].attribute
    check((mp.afi, mp.safi) == (2, 1), f"{name}: AFI/SAFI")
    if link_local_only:
        check(mp.nh_addr_len == 16 and mp.nh_v6_addr == NH_LINK_LOCAL, f"{name}: nh")
    else:
        check(
            mp.nh_addr_len == 32
            and mp.nh_v6_global == NH_GLOBAL
            and mp.nh_v6_link_local == NH_LINK_LOCAL,
            f"{name}: next-hop",
        )
    check(mp.reserved == 0, f"{name}: reserved")
    check([n.prefix for n in mp.nlri] == PREFIXES, f"{name}: NLRI")


def check_withdraw(name, raw):
    p = BGP(raw)
    u = p[BGPUpdate]
    check(p.len == len(raw) and u.withdrawn_routes_len == 0, f"{name}: header")
    check([a.type_code for a in u.path_attr] == [15], f"{name}: MP_UNREACH only")
    a = u.path_attr[0]
    check(int(a.type_flags) == 0x80, f"{name}: flags")
    un = a.attribute
    check((un.afi, un.safi) == (2, 1), f"{name}: AFI/SAFI")
    got = [n.prefix for n in un.afi_safi_specific.withdrawn_routes]
    check(got == PREFIXES, f"{name}: withdrawn")
    check(not u.nlri, f"{name}: no IPv4 NLRI")


def check_rx_update(name, raw):
    p = BGP(raw)
    u = p[BGPUpdate]
    check(p.len == len(raw), f"{name}: length")
    check([n.prefix for n in u.withdrawn_routes] == ["192.0.2.0/24"], f"{name}: wd")
    a = attrs_of(p)
    mp = a[14]
    check(int(mp.type_flags) == 0x90 and mp.attr_ext_len is not None, f"{name}: 0x90")
    check([n.prefix for n in mp.attribute.nlri] == RX_V6, f"{name}: IPv6 NLRI")
    check([n.prefix for n in u.nlri] == RX_V4, f"{name}: IPv4 NLRI")


def check_header(name, raw, length, msg_type):
    h = BGPHeader(raw)
    check(len(raw) == 19, f"{name}: 19 bytes")
    check(h.marker == (1 << 128) - 1, f"{name}: marker")
    check(h.len == length and h.type == msg_type, f"{name}: length / type")


def check_bad_marker(name, raw):
    """scapy refuses a header with a bad marker (it dissects it as Raw), so the bytes are
    checked directly: a KEEPALIVE with exactly one marker byte changed."""
    check(not isinstance(BGP(raw), BGPHeader), f"{name}: scapy rejects the marker")
    good = bytes(BGPKeepAlive())
    diff = [i for i in range(len(good)) if good[i] != raw[i]]
    check(len(raw) == 19 and diff == [5], f"{name}: one marker byte changed")


def cross_check(vectors):
    v = {name: raw for name, _, raw in vectors}
    rid = ROUTER_ID
    mp6, mp4 = [(2, 1)], [(1, 1)]
    check_open("BGP_OPEN_AS4", v["BGP_OPEN_AS4"], 4, AS_TRANS, 90, rid, ASN4, mp6)
    check_open("BGP_OPEN_AS2", v["BGP_OPEN_AS2"], 4, ASN2, 90, rid, ASN2, mp6)
    for name in ("BGP_OPEN_AS4", "BGP_OPEN_AS2"):
        params = BGP(v[name])[BGPOpen].opt_params
        check(len(params) == 1 and params[0].param_type == 2, f"{name}: 1 parameter")
    peer = "192.168.1.1"
    check_open(
        "BGP_RX_OPEN_SPLIT_CAPS",
        v["BGP_RX_OPEN_SPLIT_CAPS"],
        4,
        65020,
        180,
        peer,
        65020,
        mp4 + mp6,
    )
    check(len(BGP(v["BGP_RX_OPEN_SPLIT_CAPS"])[BGPOpen].opt_params) == 4, "4 params")
    check_open(
        "BGP_RX_OPEN_NO_AS4", v["BGP_RX_OPEN_NO_AS4"], 4, 65020, 90, peer, None, mp6
    )
    for name, hold in (("HOLD0", 0), ("HOLD1", 1), ("HOLD2", 2)):
        check_open(
            f"BGP_RX_OPEN_{name}",
            v[f"BGP_RX_OPEN_{name}"],
            4,
            65020,
            hold,
            peer,
            65020,
            mp6,
        )
    check_open(
        "BGP_RX_OPEN_NO_IPV6", v["BGP_RX_OPEN_NO_IPV6"], 4, 65020, 90, peer, 65020, mp4
    )
    check_open(
        "BGP_RX_OPEN_VERSION3",
        v["BGP_RX_OPEN_VERSION3"],
        3,
        65020,
        90,
        peer,
        65020,
        mp6,
    )
    check_open(
        "BGP_RX_OPEN_ID_ZERO",
        v["BGP_RX_OPEN_ID_ZERO"],
        4,
        65020,
        90,
        "0.0.0.0",
        65020,
        mp6,
    )
    auth = BGP(v["BGP_RX_OPEN_AUTH_PARAM"])[BGPOpen].opt_params
    check([q.param_type for q in auth] == [2, 1], "BGP_RX_OPEN_AUTH_PARAM: params")

    check_header("BGP_KEEPALIVE", v["BGP_KEEPALIVE"], 19, 4)
    n = BGP(v["BGP_NOTIFICATION_6_2"])
    check(n.len == 21 and n.type == 3, "BGP_NOTIFICATION_6_2: header")
    check(
        n[BGPNotification].error_code == 6 and n[BGPNotification].error_subcode == 2,
        "BGP_NOTIFICATION_6_2: code / subcode",
    )
    check_our_update("BGP_UPDATE_IBGP", v["BGP_UPDATE_IBGP"], True, ASN2, False)
    check_our_update("BGP_UPDATE_EBGP", v["BGP_UPDATE_EBGP"], False, ASN4, False)
    check_our_update("BGP_UPDATE_LL_ONLY", v["BGP_UPDATE_LL_ONLY"], True, ASN2, True)
    check_withdraw("BGP_WITHDRAW", v["BGP_WITHDRAW"])
    check_rx_update("BGP_RX_UPDATE_5_NLRI", v["BGP_RX_UPDATE_5_NLRI"])

    check_bad_marker("BGP_RX_BAD_MARKER", v["BGP_RX_BAD_MARKER"])
    check_header("BGP_RX_LEN_18", v["BGP_RX_LEN_18"], 18, 4)
    check_header("BGP_RX_LEN_4097", v["BGP_RX_LEN_4097"], 4097, 2)
    check_header("BGP_RX_KEEPALIVE_LEN_20", v["BGP_RX_KEEPALIVE_LEN_20"], 20, 4)
    check_header("BGP_RX_TYPE_5", v["BGP_RX_TYPE_5"], 23, 5)

    # Malformed UPDATEs: the header and attribute list length are consistent, only
    # the patched field is wrong. Checked on the raw bytes (scapy cannot parse them).
    raw = v["BGP_RX_UPDATE_ATTR_OVERRUN"]
    total = int.from_bytes(raw[21:23], "big")
    check(len(raw) == 23 + total, "BGP_RX_UPDATE_ATTR_OVERRUN: attribute list")
    check(raw[23:27] == bytes([0x40, 1, 1, 0]), "BGP_RX_UPDATE_ATTR_OVERRUN: ORIGIN")
    check(raw[27:30] == bytes([0x40, 5, 200]), "BGP_RX_UPDATE_ATTR_OVERRUN: length")
    raw = v["BGP_RX_UPDATE_PREFIX_129"]
    total = int.from_bytes(raw[21:23], "big")
    check(len(raw) == 23 + total, "BGP_RX_UPDATE_PREFIX_129: attribute list")
    mp = raw[27:]
    check(mp[:3] == bytes([0x80, 14, len(mp) - 3]), "BGP_RX_UPDATE_PREFIX_129: attr")
    check(mp[3:7] == bytes([0, 2, 1, 16]), "BGP_RX_UPDATE_PREFIX_129: AFI / nh len")
    check(mp[7 + 16] == 0 and mp[24] == 129, "BGP_RX_UPDATE_PREFIX_129: NLRI 129")
    check(len(mp) == 25 + 17, "BGP_RX_UPDATE_PREFIX_129: 17 prefix bytes")


# --- output ---


def c_array(name, comment, raw):
    lines = [
        f"/* {comment} ({len(raw)} bytes) */",
        f"static const uint8_t {name}[] = {{",
    ]
    for i in range(0, len(raw), 12):
        lines.append("    " + " ".join(f"0x{b:02x}," for b in raw[i : i + 12]))
    lines.append("};")
    return "\n".join(lines)


def render(vectors):
    head = [
        "#pragma once",
        "",
        f"// Generated by tools/routing_vectors.py with scapy {scapy.VERSION}; "
        + "do not edit.",
        "// Regenerate with `make routing-vectors`.",
        "",
        "#include <stdint.h>",
        "",
    ]
    body = "\n\n".join(c_array(*v) for v in vectors)
    return "\n".join(head) + "\n" + body + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("out", type=Path, help="vectors.h to write")
    args = ap.parse_args()
    vectors = build()
    cross_check(vectors)
    args.out.write_text(render(vectors))
    print(f"wrote {len(vectors)} vectors to {args.out}")


if __name__ == "__main__":
    main()
