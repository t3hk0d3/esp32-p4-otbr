"""Ask for the AAAA and A records of a .local name over multicast DNS and print the answers, one per line.

Standard library only: the tests run it inside a Docker `macvlan` container that sits on the board's layer-2
segment (the host itself has no address there), `python:3.14-alpine`. Usage: mdns_query.py <name.local> [seconds]
"""

import ipaddress
import socket
import struct
import sys
import time

MDNS_GROUP_V6 = "ff02::fb"
MDNS_PORT = 5353
TYPE_A = 1
TYPE_AAAA = 28
TYPE_PTR = 12
TYPE_SRV = 33


def build_query(name, qtype):
    labels = b"".join(
        bytes([len(label)]) + label.encode() for label in name.rstrip(".").split(".")
    )
    return (
        struct.pack(">HHHHHH", 0, 0, 1, 0, 0, 0)
        + labels
        + b"\x00"
        + struct.pack(">HH", qtype, 1)
    )


def skip_name(packet, offset):
    while True:
        length = packet[offset]
        if length == 0:
            return offset + 1
        if length & 0xC0 == 0xC0:
            return offset + 2
        offset += 1 + length


def answers(packet):
    """(type, rdata) of every answer record in a DNS packet."""
    _, _, questions, answer_count, _, _ = struct.unpack(">HHHHHH", packet[:12])
    offset = 12
    for _ in range(questions):
        offset = skip_name(packet, offset) + 4
    found = []
    for _ in range(answer_count):
        offset = skip_name(packet, offset)
        rtype, _, _, length = struct.unpack(">HHIH", packet[offset : offset + 10])
        offset += 10
        found.append((rtype, packet[offset : offset + length]))
        offset += length
    return found


def read_name(packet, offset):
    """(name, offset after it) of a possibly compressed DNS name."""
    labels, end = [], None
    while True:
        length = packet[offset]
        if length == 0:
            return ".".join(labels), end if end is not None else offset + 1
        if length & 0xC0 == 0xC0:
            end = end if end is not None else offset + 2
            offset = ((length & 0x3F) << 8) | packet[offset + 1]
            continue
        labels.append(packet[offset + 1 : offset + 1 + length].decode(errors="replace"))
        offset += 1 + length


def records(packet):
    """Every record of a DNS packet: (section, name, type, value).

    section is "an", "ns" or "ar"; value is the address for A/AAAA, the target name for PTR and SRV,
    otherwise the raw rdata bytes."""
    _, _, questions, *counts = struct.unpack(">HHHHHH", packet[:12])
    offset = 12
    for _ in range(questions):
        offset = skip_name(packet, offset) + 4
    found = []
    for section, count in zip(("an", "ns", "ar"), counts):
        for _ in range(count):
            name, offset = read_name(packet, offset)
            rtype, _, _, length = struct.unpack(">HHIH", packet[offset : offset + 10])
            offset += 10
            rdata = packet[offset : offset + length]
            if rtype == TYPE_A and length == 4:
                value = str(ipaddress.IPv4Address(rdata))
            elif rtype == TYPE_AAAA and length == 16:
                value = str(ipaddress.IPv6Address(rdata))
            elif rtype == TYPE_PTR:
                value = read_name(packet, offset)[0]
            elif rtype == TYPE_SRV:
                value = read_name(packet, offset + 6)[0]
            else:
                value = rdata
            found.append((section, name, rtype, value))
            offset += length
    return found


def main():
    name = sys.argv[1]
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
    sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    sock.bind(("::", 0))
    sock.settimeout(0.5)
    interface = socket.if_nametoindex("eth0")
    found = set()
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        for qtype in (TYPE_AAAA, TYPE_A):
            sock.sendto(
                build_query(name, qtype), (MDNS_GROUP_V6, MDNS_PORT, 0, interface)
            )
        try:
            packet, _ = sock.recvfrom(1500)
        except TimeoutError:
            continue
        for rtype, rdata in answers(packet):
            if rtype == TYPE_AAAA and len(rdata) == 16:
                found.add(str(ipaddress.IPv6Address(rdata)))
            elif rtype == TYPE_A and len(rdata) == 4:
                found.add(str(ipaddress.IPv4Address(rdata)))
        if found:
            break
    for address in sorted(found):
        print(address)


if __name__ == "__main__":
    main()
