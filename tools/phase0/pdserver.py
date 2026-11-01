"""Minimal DHCPv6 prefix-delegation server (Phase 0 test only). Delegates one /56 per client, no IA_NA."""
import ipaddress, os, socket, struct, sys, time
POOL = ipaddress.IPv6Network(sys.argv[1] if len(sys.argv) > 1 else "fd5d:a5d0:100::/48")
PLEN = int(sys.argv[2]) if len(sys.argv) > 2 else 56
RUN = float(sys.argv[3]) if len(sys.argv) > 3 else 600
T1, T2, PREF, VALID = (int(x) for x in (sys.argv[4] if len(sys.argv) > 4 else "1800,2880,3600,7200").split(","))
NAMES = {1: "SOLICIT", 3: "REQUEST", 5: "RENEW", 6: "REBIND", 8: "RELEASE", 11: "INFO-REQ"}
mac = open("/sys/class/net/eth0/address").read().strip().replace(":", "")
SERVER_DUID = struct.pack("!HH", 3, 1) + bytes.fromhex(mac)
leases = {}
subnets = POOL.subnets(new_prefix=PLEN)
def opt(code, data): return struct.pack("!HH", code, len(data)) + data
def parse(data):
    out, i = {}, 0
    while i + 4 <= len(data):
        c, l = struct.unpack("!HH", data[i:i+4]); out.setdefault(c, []).append(data[i+4:i+4+l]); i += 4 + l
    return out
s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("::", 547))
idx = socket.if_nametoindex("eth0")
s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_JOIN_GROUP, socket.inet_pton(socket.AF_INET6, "ff02::1:2") + struct.pack("@I", idx))
s.settimeout(1)
print(f"pdserver: pool {POOL} /{PLEN}, server DUID {SERVER_DUID.hex()}", flush=True)
end = time.time() + RUN
while time.time() < end:
    try:
        data, src = s.recvfrom(4096)
    except socket.timeout:
        continue
    mtype, xid, opts = data[0], data[1:4], parse(data[4:])
    if mtype not in NAMES or 1 not in opts:
        continue
    cid = opts[1][0]
    if mtype != 8 and 25 not in opts:
        continue  # PD-only test server: never answer clients that do not ask for a prefix
    if mtype in (3, 5) and 2 in opts and opts[2][0] != SERVER_DUID:
        continue  # addressed to another server
    reply = opt(1, cid) + opt(2, SERVER_DUID)
    pd_note = "no IA_PD requested"
    for ia in opts.get(25, []):
        iaid = ia[:4]
        if cid not in leases:
            leases[cid] = next(subnets)
        net = leases[cid]
        prefix = struct.pack("!IIB", PREF, VALID, net.prefixlen) + net.network_address.packed
        reply += opt(25, iaid + struct.pack("!II", T1, T2) + opt(26, prefix))
        pd_note = f"IA_PD {net}"
    for ia in opts.get(3, []):
        reply += opt(3, ia[:4] + struct.pack("!II", 0, 0) + opt(13, struct.pack("!H", 2) + b"no addresses (PD-only test server)"))
    if mtype == 1:
        out_type = 7 if 14 in opts else 2   # rapid commit -> REPLY
        if out_type == 7: reply += opt(14, b"")
        reply += opt(7, b"\xff")
    elif mtype == 8:
        out_type = 7; leases.pop(cid, None); pd_note = "released"
    else:
        out_type = 7
    s.sendto(bytes([out_type]) + xid + reply, src)
    print(f"{time.strftime('%H:%M:%S')} {NAMES[mtype]} from {src[0]} -> {'ADVERTISE' if out_type == 2 else 'REPLY'} {pd_note}", flush=True)
