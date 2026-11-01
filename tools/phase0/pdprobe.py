"""Send a DHCPv6 SOLICIT with IA_PD on eth0 and print any ADVERTISE/REPLY with its IA_PD prefixes."""
import os, socket, struct, time
def opt(code, data): return struct.pack("!HH", code, len(data)) + data
mac = open("/sys/class/net/eth0/address").read().strip().replace(":", "")
duid = struct.pack("!HH", 3, 1) + bytes.fromhex(mac)  # DUID-LL
xid = os.urandom(3)
iapd = opt(25, struct.pack("!III", 0xBEEF, 0, 0))
msg = bytes([1]) + xid + opt(1, duid) + opt(8, b"\x00\x00") + opt(6, struct.pack("!H", 23)) + iapd
s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("::", 546))
ifidx = socket.if_nametoindex("eth0")
s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_MULTICAST_IF, ifidx)
s.settimeout(2)
for attempt in range(3):
    s.sendto(msg, ("ff02::1:2", 547, 0, ifidx))
    end = time.time() + 4
    while time.time() < end:
        try:
            data, src = s.recvfrom(4096)
        except socket.timeout:
            continue
        if data[1:4] != xid:
            continue
        print("reply type", data[0], "from", src[0])
        i = 4
        while i + 4 <= len(data):
            code, ln = struct.unpack("!HH", data[i:i+4]); body = data[i+4:i+4+ln]
            if code == 25:
                j = 12
                while j + 4 <= len(body):
                    c2, l2 = struct.unpack("!HH", body[j:j+4]); b2 = body[j+4:j+4+l2]
                    if c2 == 26:
                        pref, valid, plen = struct.unpack("!IIB", b2[:9])
                        print("  IA_PD prefix", socket.inet_ntop(socket.AF_INET6, b2[9:25]) + f"/{plen}", "valid", valid)
                    elif c2 == 13:
                        print("  IA_PD status", struct.unpack("!H", b2[:2])[0], b2[2:].decode(errors="replace"))
                    j += 4 + l2
            elif code == 13:
                print("  status", struct.unpack("!H", body[:2])[0], body[2:].decode(errors="replace"))
            i += 4 + ln
        raise SystemExit(0)
print("no ADVERTISE received (3 attempts)")
