"""netcfg: VLAN interfaces, isolation, per-interface policy, confirm-or-revert (spec 5.13; BDD: Task 19).

Hardware-gated: written with Task 19 as the contract, deferred to V3 (never run in Task 19). Needs a human-built rig:
a managed switch with a trunk to the board carrying the VLANs, and Linux hosts on those VLANs. Selected with
`pytest -m vlan_switch` (excluded by default).

Environment:
- VLAN_MGMT, VLAN_THREAD: the VLAN IDs of the two segments (distinct, 1..4094).
- HOST_IF_MGMT, HOST_IF_THREAD: the host interfaces on those VLANs (e.g. `mgmt0`, `thread0`). The test hosts are Docker
  `macvlan` containers on them (the networks `otbr-vlan-mgmt` / `otbr-vlan-thread` are created when missing), IPv6 with
  `accept_ra=2`, so the host itself needs no address there. The board is reached on its IPv6 link-local address, which
  both of its interfaces share: the zone of the container's interface tells them apart.
- HOST_TRUNK_IF (optional): the untagged parent interface of the trunk, for the unknown-VLAN test.
- MGMT_HOST_ADDR (optional): an IPv6 address of a host on the mgmt segment, the target of the thread-to-mgmt probe.
- The default single-mode board needs the usual rig (DHCP on the untagged LAN, `board_ip`).

The board starts every test in single mode and is put back with the console command `net reset`.
"""

import ipaddress
import json
import os
import re
import subprocess
import textwrap
import time
from pathlib import Path

import pytest
import requests
from conftest import BOOT_TIMEOUT_S, CONSOLE_READY, ROM_BANNER, wait_ot_state
from test_br_boot import MDNS_IMAGE
from test_latency import thread_peer_address  # noqa: F401  (fixture)

pytestmark = pytest.mark.vlan_switch

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
OTBR_COMPAT = REPO / "tools" / "otbr_compat.py"
NETWORK = {"mgmt": "otbr-vlan-mgmt", "thread": "otbr-vlan-thread"}
CONTAINER_SUBNET = {"mgmt": "fd5a:7e57:a::/64", "thread": "fd5a:7e57:b::/64"}
CONFIRM_WINDOW_S = 300
UNKNOWN_VLAN_FRAMES = 20
SNIFF_SECONDS = 12
RENAME_WATCH_S = 60
LEADER_TIMEOUT_S = 90
PEER_ATTACH_TIMEOUT_S = 120
SYSCTLS = [
    f"net.ipv6.conf.{scope}.{key}={value}"
    for scope in ("all", "default")
    for key, value in (
        ("disable_ipv6", 0),
        ("accept_ra", 2),
        ("accept_ra_rt_info_max_plen", 64),
    )
]
MODE_LOG = r"netcfg: mode (single|split)( \(trial\))?"

# Runs in a container: one raw HTTP request, so that a closed connection is seen as EOF. Prints one JSON line.
HTTP_SNIPPET = textwrap.dedent("""
    import json, socket, sys
    host, method, path, body = sys.argv[1:5]
    head = f"{method} {path} HTTP/1.1\\r\\nHost: board\\r\\nContent-Length: {len(body)}\\r\\n"
    head += "Content-Type: application/json\\r\\n" if body else ""
    try:
        # A fresh container has a tentative link-local address for a second (it cannot answer the board's neighbour
        # solicitation yet), and the board gives up resolving after 3 s: connect again instead of waiting.
        for attempt in range(4):
            try:
                sock = socket.create_connection((host, 80), timeout=4)
                break
            except socket.timeout:
                if attempt == 3:
                    raise
        sock.sendall((head + "\\r\\n" + body).encode())
        sock.settimeout(3)
        data, closed = b"", False
        try:
            while True:
                chunk = sock.recv(4096)
                if not chunk:
                    closed = True
                    break
                data += chunk
        except socket.timeout:
            pass
        raw_head, _, payload = data.partition(b"\\r\\n\\r\\n")
        status = int(raw_head.split()[1]) if raw_head else None
        print(json.dumps({"status": status, "closed": closed, "body": payload.decode(errors="replace")}))
    except OSError as error:
        print(json.dumps({"status": None, "closed": True, "body": str(error)}))
""")

# Runs in a container: count ICMPv6 echo requests to an address seen on the wire for a while. Prints the count.
SNIFF_SNIPPET = textwrap.dedent("""
    import socket, sys, time
    target = socket.inet_pton(socket.AF_INET6, sys.argv[1])
    seconds = float(sys.argv[2])
    sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(3))
    sock.settimeout(0.5)
    seen, end = 0, time.time() + seconds
    while time.time() < end:
        try:
            frame = sock.recv(2048)
        except socket.timeout:
            continue
        if frame[12:14] == b"\\x86\\xdd" and frame[20] == 58 and frame[38:54] == target and frame[54] == 128:
            seen += 1
    print(seen)
""")

# Runs in a container: multicast DNS query, one JSON line per packet heard: {"source": ..., "records": [[section, name, type, value]]}
# (value: address for A/AAAA, target name for PTR/SRV, hex otherwise).
# Usage: name type_number (12 = PTR, 1 = A, 28 = AAAA, 33 = SRV) full_window (1: listen the whole 8 s, 0: stop at the first answer).
MDNS_SNIPPET = textwrap.dedent("""
    import json, socket, sys, time
    sys.path.insert(0, "/t")
    import mdns_query
    name, qtype, full = sys.argv[1], int(sys.argv[2]), sys.argv[3] == "1"
    sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    sock.bind(("::", 0))
    sock.settimeout(0.5)
    interface = socket.if_nametoindex("eth0")
    end, found, last = time.monotonic() + 8, 0, 0.0
    while time.monotonic() < end and (full or not found):
        if time.monotonic() - last >= 0.5:
            sock.sendto(mdns_query.build_query(name, qtype), ("ff02::fb", 5353, 0, interface))
            last = time.monotonic()
        try:
            packet, source = sock.recvfrom(1500)
        except socket.timeout:
            continue
        records = [[s, n, t, v if isinstance(v, str) else v.hex()] for s, n, t, v in mdns_query.records(packet)]
        if records:
            found += 1
            print(json.dumps({"source": source[0].split("%")[0], "records": records}), flush=True)
""")

# Runs in a container: answers multicast DNS queries for one AAAA name with a fixed address for N seconds (legacy unicast
# reply to the asker). Prints "ready" once it listens. Usage: name address seconds
RESPONDER_SNIPPET = textwrap.dedent("""
    import socket, struct, sys, time
    sys.path.insert(0, "/t")
    import mdns_query
    name, address, seconds = sys.argv[1], sys.argv[2], float(sys.argv[3])
    sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("::", 5353))
    interface = socket.if_nametoindex("eth0")
    sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_JOIN_GROUP,
                    socket.inet_pton(socket.AF_INET6, "ff02::fb") + struct.pack("@I", interface))
    sock.settimeout(0.5)
    labels = b"".join(bytes([len(x)]) + x.encode() for x in name.split("."))
    question = labels + b"\\x00" + struct.pack(">HH", 28, 1)
    answer = labels + b"\\x00" + struct.pack(">HHIH", 28, 0x8001, 120, 16) + socket.inet_pton(socket.AF_INET6, address)
    print("ready", flush=True)
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            packet, source = sock.recvfrom(1500)
        except socket.timeout:
            continue
        if question in packet[12:] and not packet[2] & 0x80:
            header = packet[:2] + struct.pack(">HHHHH", 0x8400, 0, 1, 0, 0)
            sock.sendto(header + answer, source)
""")


def sh(command, *args):
    return ["python", "-c", command, *map(str, args)]


def docker_run(side, argv, setup="", timeout=90, wait=True, name=None):
    """Run `argv` in a container on the VLAN segment of `side`; `setup` is a shell prefix (ip addr, route)."""
    sysctls = [x for s in SYSCTLS for x in ("--sysctl", s)]
    command = [
        "docker",
        "run",
        "--rm",
        *(["--name", name] if name else []),
        "--network",
        NETWORK[side],
        "--cap-add",
        "NET_ADMIN",
        "--cap-add",
        "NET_RAW",
    ]
    command += [
        *sysctls,
        "-v",
        f"{HERE}:/t:ro",
        "-v",
        f"{REPO}:/repo:ro",
        MDNS_IMAGE,
        "sh",
        "-c",
        f'{setup}"$@"',
        "sh",
        *argv,
    ]
    if not wait:
        return subprocess.Popen(command, stdout=subprocess.PIPE, text=True)
    return subprocess.run(
        command, capture_output=True, text=True, timeout=timeout, check=False
    )


def http_from(side, host, method, path, body=""):
    """(status, connection_closed_by_board, body) of one request from the segment `side` to `host` (address with zone)."""
    result = docker_run(side, sh(HTTP_SNIPPET, host, method, path, body))
    assert result.returncode == 0, result.stderr
    reply = json.loads(result.stdout.strip().splitlines()[-1])
    return reply["status"], reply["closed"], reply["body"]


@pytest.fixture(scope="session")
def rig():
    """The VLAN rig from the environment; the Docker networks exist for the session."""
    names = ("VLAN_MGMT", "VLAN_THREAD", "HOST_IF_MGMT", "HOST_IF_THREAD")
    missing = [n for n in names if not os.environ.get(n)]
    if missing:
        pytest.skip(f"VLAN rig not configured, missing: {', '.join(missing)}")
    created = []
    for side, parent in (
        ("mgmt", os.environ["HOST_IF_MGMT"]),
        ("thread", os.environ["HOST_IF_THREAD"]),
    ):
        if subprocess.run(
            ["docker", "network", "inspect", NETWORK[side]],
            capture_output=True,
            check=False,
        ).returncode:
            subprocess.run(
                [
                    "docker",
                    "network",
                    "create",
                    "-d",
                    "macvlan",
                    "-o",
                    f"parent={parent}",
                    # Docker disables IPv6 on the interface of a network that has no IPv6 subnet (the board is
                    # reached on its link-local address); the subnet is unrelated to the board's prefixes.
                    "--ipv6",
                    "--subnet",
                    CONTAINER_SUBNET[side],
                    NETWORK[side],
                ],
                check=True,
            )
            created.append(NETWORK[side])
    yield {
        "mgmt": int(os.environ["VLAN_MGMT"]),
        "thread": int(os.environ["VLAN_THREAD"]),
    }
    for network in created:
        subprocess.run(["docker", "network", "rm", network], check=False)


def split_config(rig, mgmt=None):
    return {
        "mode": "split",
        "mgmt": mgmt or {"vlan": rig["mgmt"], "ipv4": {"dhcp": True}},
        "thread_bb": {"vlan": rig["thread"], "ipv4": {"dhcp": True}},
    }


def net_show(send):
    """`net show` on the board's console, parsed."""
    match = send("net show", rb"net: (\{[^\r\n]*\})\r?\n")
    return json.loads(match.group(1))


def net_show_settled(send, timeout=60):
    """`net show` once every interface has its link and a link-local address (they follow the boot by a few seconds)."""
    deadline = time.monotonic() + timeout
    while True:
        state = net_show(send)
        if state["interfaces"] and all(
            i["link"] and any(a.startswith("fe80") for a in i["ipv6"])
            for i in state["interfaces"]
        ):
            return state
        assert time.monotonic() < deadline, f"interfaces not ready: {state}"
        time.sleep(2)


def hostname_of(state):
    """`esp-otbr-<last three MAC bytes>`: the log line that names it is gone by now, the EUI-64 link-local address has the MAC."""
    groups = link_local(state, state["interfaces"][0]["name"]).split("%")[0].split(":")
    return f"esp-otbr-{groups[-2][2:]}{groups[-1]}"


def link_local(state, name):
    """Link-local address of the interface `name` with the zone of the container's interface."""
    for iface in state["interfaces"]:
        if iface["name"] == name:
            return next(a for a in iface["ipv6"] if a.startswith("fe80")) + "%eth0"
    raise AssertionError(f"no interface {name}: {state}")


def wait_mode(dut, mode, trial):
    """Wait for the boot log of `mode`; `trial` is whether the trial marker must be there."""
    match = dut.expect(re.compile(MODE_LOG.encode()), timeout=BOOT_TIMEOUT_S)
    assert match.group(1).decode() == mode
    assert bool(match.group(2)) == trial


@pytest.fixture
def to_split_trial(dut, board_ip, rig, send):
    """to_split_trial(config=None): PUT a split config on the single-mode board and wait for the trial boot."""

    def apply(config=None):
        reply = requests.put(
            f"http://{board_ip}/device/network",
            json=config or split_config(rig),
            timeout=10,
        )
        assert reply.status_code == requests.codes.ok, reply.text
        dut.expect(ROM_BANNER, timeout=BOOT_TIMEOUT_S)
        wait_mode(dut, "split", True)
        dut.expect(CONSOLE_READY, timeout=BOOT_TIMEOUT_S)

    yield apply
    send("net reset", rb"net: defaults restored")
    dut.expect(ROM_BANNER, timeout=BOOT_TIMEOUT_S)


@pytest.fixture
def split_board(to_split_trial, send, rig):
    """The board in a confirmed split config; yields the parsed `net show`."""
    to_split_trial()
    state = net_show_settled(send)
    status, _, body = http_from(
        "mgmt", link_local(state, "mgmt"), "POST", "/device/network/confirm"
    )
    assert status == requests.codes.ok, body
    return net_show(send)


@pytest.fixture
def split_with_peer(thread_peer_address, split_board, dut, peer):  # noqa: F811
    """Split mode with the Thread peer attached (the peer fixture chain runs first: it erases and reboots the board).

    Yields (parsed `net show`, global address of the peer)."""
    wait_ot_state(dut, {"leader", "router"}, LEADER_TIMEOUT_S)
    peer.wait_state({"child", "router", "leader"}, PEER_ATTACH_TIMEOUT_S)
    return split_board, thread_peer_address


class TestSingleModeInterface:
    """netcfg, interfaces, single mode untagged"""

    def test_one_interface_receives_untagged_frames(self, http, send):
        # On single mode untagged: one interface receives untagged frames.
        state = net_show(send)
        assert state["mode"] == "single"
        assert [(i["name"], i["vlan"]) for i in state["interfaces"]] == [("eth", 0)]
        assert http.get("/device/status").status_code == requests.codes.ok


class TestSplitInterfaces:
    """netcfg, interfaces, split mode"""

    def test_each_interface_answers_on_its_own_vlan(self, split_board):
        # On split mode: mgmt and thread_bb receive frames with their VLAN ID.
        for side, name in (("mgmt", "mgmt"), ("thread", "thread_bb")):
            host = link_local(split_board, name)
            status, _, _ = http_from(side, host, "GET", "/node/state")
            assert status is not None and status != requests.codes.forbidden, (
                name,
                status,
            )

    def test_an_interface_does_not_answer_on_the_other_vlan(self, split_board):
        # On split mode: each interface receives only frames with its VLAN ID (the IPv4 address of the other one is silent here;
        # the link-local address cannot tell, both interfaces share it).
        ipv4 = {i["name"]: i["ipv4"] for i in split_board["interfaces"]}
        for side, other in (("mgmt", "thread_bb"), ("thread", "mgmt")):
            if not ipv4[other]:
                pytest.skip(f"{other} got no IPv4 address")
            status, _, _ = http_from(side, ipv4[other], "GET", "/node/state")
            assert status is None, (side, other, status)

    def test_frames_with_an_unknown_vlan_id_are_dropped(self, split_board, send):
        # On split mode: frames with an unknown VLAN ID are dropped (counted in vlan_rx_dropped).
        trunk = os.environ.get("HOST_TRUNK_IF")
        if not trunk:
            pytest.skip("HOST_TRUNK_IF (untagged trunk parent) not set")
        unknown = next(
            v for v in range(2, 4094) if v not in (split_board_vlans(split_board))
        )
        before = net_show(send)["vlan_rx_dropped"]
        send_tagged_frames(trunk, unknown, UNKNOWN_VLAN_FRAMES)
        assert net_show(send)["vlan_rx_dropped"] >= before + UNKNOWN_VLAN_FRAMES

    def test_openthread_backbone_is_thread_bb(self, split_with_peer):
        # On split mode: the OpenThread backbone netif is thread_bb (router advertisements with route information only there).
        assert ra_seen("thread") and not ra_seen("mgmt")

    def test_hostname_answers_on_thread_bb(self, split_board):
        # On split mode: the mDNS hostname answers on thread_bb.
        assert mdns_answers("thread", hostname_of(split_board) + ".local", 28) > 0

    def test_hostname_is_not_served_on_mgmt(self, split_board):
        # On split mode: the mDNS hostname is never answered by the mgmt interface (no record from or about a mgmt address).
        name = hostname_of(split_board) + ".local"
        addresses = mgmt_addresses(split_board)
        for qtype in (1, 28):
            records = mdns_records("mgmt", name, qtype, full_window=True)
            assert not [r for r in records if comes_from_mgmt(r, addresses)], records

    def test_meshcop_service_is_advertised_on_thread_bb(self, split_with_peer):
        # On split mode: _meshcop._udp is answered on thread_bb.
        assert mdns_answers("thread", "_meshcop._udp.local", 12) > 0

    def test_no_service_is_advertised_on_mgmt(self, split_with_peer):
        # On split mode: no service (_meshcop._udp or any other, found through the service type list) is answered by mgmt.
        state = split_with_peer[0]
        addresses = mgmt_addresses(state)
        for name in ("_meshcop._udp.local", "_services._dns-sd._udp.local"):
            records = mdns_records("mgmt", name, 12, full_window=True)
            assert not [r for r in records if comes_from_mgmt(r, addresses)], (
                name,
                records,
            )

    def test_service_answers_never_carry_a_mgmt_address(self, split_with_peer):
        # On split mode: the SRV target and every address record of the _meshcop._udp answers, on either VLAN, are never a
        # mgmt address (a PTR answer carries only a name, so the source address alone would not show a leak).
        state = split_with_peer[0]
        addresses = mgmt_addresses(state)
        assert addresses, state
        records, targets = service_targets("thread", "_meshcop._udp.local")
        assert targets, f"no _meshcop._udp service seen on thread_bb: {records}"
        for side in ("thread", "mgmt"):
            side_records, side_targets = service_targets(side, "_meshcop._udp.local")
            leaked = [
                r
                for r in side_records
                if r[2] in (1, 28) and is_mgmt_address(r[3], addresses)
            ]
            assert not leaked, (side, leaked)
            for target in side_targets:
                assert not host_addresses(side, target) & addresses, (side, target)
        thread_side = host_addresses("thread", next(iter(targets)))
        assert thread_side, "the service target has no address on thread_bb"
        thread_bb = next(i for i in state["interfaces"] if i["name"] == "thread_bb")
        board_addresses = {a.split("%")[0] for a in thread_bb["ipv6"]} | {
            thread_bb["ipv4"]
        }
        assert thread_side & board_addresses

    def test_boot_log_names_the_mdns_interfaces(self, split_board, dut):
        # On split mode: the boot log says mDNS is active on thread_bb and not on mgmt.
        log = console_log(dut)
        assert "mDNS active on thread_bb" in log
        assert "mDNS not on mgmt" in log
        assert "mDNS active on mgmt" not in log

    def test_a_name_on_the_querying_segment_is_answered(self, split_board, rig):
        # Control for the mgmt-silence tests: the query path on mgmt does return answers that exist there.
        assert control_answer("mgmt", "mgmt") == [CONTROL_ADDRESS]

    def test_a_name_announced_on_thread_bb_is_reflected_to_mgmt(self, split_board, rig):
        # Control for the reflector: a record announced on the thread VLAN is visible from the mgmt VLAN, so the
        # gateway reflects and the empty mgmt results of the other tests mean "not announced by the board".
        answer = control_answer("thread", "mgmt")
        if not answer:
            pytest.skip("this gateway does not reflect mDNS between the VLANs")
        assert answer == [CONTROL_ADDRESS]

    def test_a_conflicting_announcement_from_another_host_renames_the_host(
        self, split_board, dut
    ):
        # Control for the next test: the spoofed announcement does conflict (a bigger address for our host name).
        name = hostname_of(split_board)
        spoof_announcement("thread", "fd5a:7e57:b::77", name)
        assert mdns_answers("thread", f"{name}-2.local", 28) > 0, (
            "the control announcement did not conflict"
        )

    def test_an_announcement_sourced_from_the_other_interface_is_ignored(
        self, split_board, dut
    ):
        # On split mode: a conflicting announcement of our host name that arrives on thread_bb with a source address of mgmt
        # (a reflector echoing mgmt's own announcement) is ignored: no rename.
        name = hostname_of(split_board)
        state = split_board
        mgmt_global = next(
            a.split("%")[0]
            for a in next(i for i in state["interfaces"] if i["name"] == "mgmt")["ipv6"]
            if not a.startswith("fe80")
        )
        spoof_announcement("thread", str(ipaddress.ip_address(mgmt_global)), name)
        assert not mdns_answers("thread", f"{name}-2.local", 28)
        assert mdns_answers("thread", f"{name}.local", 28) > 0

    def test_an_announcement_from_the_shared_link_local_address_is_ignored(
        self, split_board, dut
    ):
        # On split mode: both interfaces share the link-local address; a conflicting announcement from it is suppressed by the
        # library's own-source check, so no rename.
        name = hostname_of(split_board)
        link_local_address = link_local(split_board, "mgmt").split("%")[0]
        spoof_announcement(
            "thread", str(ipaddress.ip_address(link_local_address)), name
        )
        assert not mdns_answers("thread", f"{name}-2.local", 28)
        assert mdns_answers("thread", f"{name}.local", 28) > 0

    def test_host_is_not_renamed_while_mdns_is_reflected(self, split_board, dut):
        # On split mode (also behind a gateway that reflects mDNS between the VLANs): the host keeps its name for 60 s.
        name = hostname_of(split_board)
        deadline = time.monotonic() + RENAME_WATCH_S
        while time.monotonic() < deadline:
            assert not mdns_records("thread", f"{name}-2.local", 28), (
                "the host was renamed"
            )
        assert mdns_answers("thread", f"{name}.local", 28) > 0
        log = console_log(dut)
        assert f"{name}-" not in log
        assert not re.search(r"(?i)conflict|renam", log), (
            "the board logged a name conflict"
        )


def split_board_vlans(state):
    return {i["vlan"] for i in state["interfaces"]}


def send_tagged_frames(trunk, vlan, count):
    """Send `count` broadcast ARP-sized frames tagged with `vlan` on the trunk parent interface (host side, needs root)."""
    code = textwrap.dedent(f"""
        import socket, struct
        sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
        sock.bind(("{trunk}", 0))
        frame = b"\\xff" * 6 + b"\\x02\\x00\\x00\\x00\\x00\\x01" + struct.pack(">HHH", 0x8100, {vlan}, 0x0806) + bytes(46)
        for _ in range({count}):
            sock.send(frame)
    """)
    subprocess.run(["python3", "-c", code], check=True, timeout=30)


def ra_seen(side, seconds=SNIFF_SECONDS):
    """Did a router advertisement with a route information option arrive on the segment within `seconds`?"""
    code = textwrap.dedent(f"""
        import socket, time
        sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(3))
        sock.settimeout(0.5)
        end, found = time.time() + {seconds}, False
        while time.time() < end and not found:
            try:
                frame = sock.recv(2048)
            except socket.timeout:
                continue
            found = frame[12:14] == b"\\x86\\xdd" and frame[20] == 58 and frame[54] == 134 and 24 in frame[70:]
        print(found)
    """)
    result = docker_run(side, sh(code), timeout=seconds + 60)
    return result.stdout.strip().endswith("True")


def mdns_packets(side, name, qtype, full_window=False):
    """Packets heard for a multicast DNS query on `side`: [(source address, [(section, name, type, value)])]."""
    result = docker_run(
        side, sh(MDNS_SNIPPET, name, qtype, int(full_window)), timeout=90
    )
    packets = []
    for line in result.stdout.strip().splitlines():
        if line.startswith("{"):
            packet = json.loads(line)
            packets.append(
                (packet["source"], [tuple(record) for record in packet["records"]])
            )
    return packets


def mdns_records(side, name, qtype, full_window=False):
    """Answer-section records heard for a query on `side`: (source address, record type, value)."""
    return [
        (source, rtype, value)
        for source, records in mdns_packets(side, name, qtype, full_window)
        for section, _, rtype, value in records
        if section == "an"
    ]


def mdns_answers(side, name, qtype):
    return len(mdns_records(side, name, qtype))


def mgmt_addresses(state):
    """Addresses of the mgmt interface (IPv4 and global IPv6) as an mDNS answer would carry them.

    The link-local address is left out: both interfaces share the MAC and so the interface identifier."""
    mgmt = next(i for i in state["interfaces"] if i["name"] == "mgmt")
    addresses = [mgmt["ipv4"]] if mgmt["ipv4"] else []
    addresses += [a.split("%")[0] for a in mgmt["ipv6"] if not a.startswith("fe80")]
    return {str(ipaddress.ip_address(a)) for a in addresses}


def is_mgmt_address(value, addresses):
    try:
        return str(ipaddress.ip_address(value)) in addresses
    except ValueError:
        return False


def comes_from_mgmt(record, addresses):
    """Was the record sent by the board's mgmt interface, or does it announce a mgmt address?

    Behind an mDNS reflector the thread_bb answer is also heard on the mgmt VLAN; it has thread_bb's source and addresses."""
    source, rtype, value = record
    return is_mgmt_address(source, addresses) or (
        rtype in (1, 28) and is_mgmt_address(value, addresses)
    )


def service_targets(side, service):
    """(all records, SRV target host names) of a PTR query for `service` on `side`, following the PTR to its SRV.

    The PTR answer carries a name only; the SRV and the address records of the target are in the additional section or need
    their own query."""
    records, targets = [], set()
    for _, packet_records in mdns_packets(side, service, 12, full_window=True):
        records += packet_records
    for instance in {v for s, _, t, v in records if t == 12}:
        for _, packet_records in mdns_packets(side, instance, 33, full_window=False):
            records += packet_records
    targets = {v for _, _, t, v in records if t == 33}
    return records, targets


def host_addresses(side, host):
    """Addresses (A and AAAA, from any source) that queries for `host` get answered with on `side`."""
    return {
        value
        for qtype in (1, 28)
        for _, rtype, value in mdns_records(side, host, qtype, full_window=True)
        if rtype in (1, 28)
    }


CONTROL_NAME = "v3b-control.local"
CONTROL_ADDRESS = "fd5a:7e57:c::1"


def console_log(dut):
    """Everything the board has printed in this test so far."""
    return Path(dut.logfile).read_text(errors="replace")


def control_answer(announce_on, ask_on):
    """AAAA values answered to a query on `ask_on` for a name that a throwaway responder serves on `announce_on`."""
    container = f"otbr-control-{announce_on}-{ask_on}-{os.getpid()}"
    responder = docker_run(
        announce_on,
        sh(RESPONDER_SNIPPET, CONTROL_NAME, CONTROL_ADDRESS, 60),
        wait=False,
        name=container,
    )
    try:
        assert responder.stdout.readline().strip() == "ready"
        time.sleep(1)
        return sorted(
            {
                value
                for _, rtype, value in mdns_records(ask_on, CONTROL_NAME, 28, True)
                if rtype == 28
            }
        )
    finally:
        subprocess.run(["docker", "kill", container], capture_output=True, check=False)
        responder.wait()


def spoof_announcement(side, source, host):
    """Send a response that gives `host` an IPv4 and IPv6 address bigger than the board's, from the IPv6 `source`, then wait for it to act."""
    result = docker_run(
        side,
        [
            "python",
            "/t/mdns_spoof.py",
            source,
            f"{host}.local",
            "255.255.255.254",
            "fe80::ffff:ffff:ffff:ffff",
            "12",
        ],
        timeout=60,
    )
    assert result.returncode == 0, result.stderr
    time.sleep(4)


def omr_route_setup(board_link_local, omr_address):
    return f"ip -6 route add {omr_address}/128 via {board_link_local.split('%')[0]} dev eth0 && "


class TestIsolation:
    """netcfg, isolation (split mode)"""

    def test_packet_on_mgmt_for_the_thread_network_is_not_forwarded(
        self, split_with_peer
    ):
        # On packet arriving on mgmt not addressed to the device: is dropped, not forwarded.
        state, peer_address = split_with_peer
        sniffer = docker_run(
            "thread", sh(SNIFF_SNIPPET, peer_address, SNIFF_SECONDS), wait=False
        )
        time.sleep(2)
        setup = omr_route_setup(link_local(state, "mgmt"), peer_address)
        docker_run("mgmt", ["ping6", "-c", "5", "-W", "1", peer_address], setup=setup)
        assert sniffer.communicate(timeout=60)[0].strip() == "0"

    def test_packet_from_the_thread_side_for_mgmt_is_not_forwarded(self, split_board):
        # On packet from thread_bb or Thread whose route resolves to mgmt: is dropped, not forwarded.
        target = os.environ.get("MGMT_HOST_ADDR")
        if not target:
            pytest.skip(
                "MGMT_HOST_ADDR (an IPv6 address of a host on the mgmt segment) not set"
            )
        sniffer = docker_run(
            "mgmt", sh(SNIFF_SNIPPET, target, SNIFF_SECONDS), wait=False
        )
        time.sleep(2)
        setup = omr_route_setup(link_local(split_board, "thread_bb"), target)
        docker_run("thread", ["ping6", "-c", "5", "-W", "1", target], setup=setup)
        assert sniffer.communicate(timeout=60)[0].strip() == "0"

    def test_reply_to_an_off_subnet_management_client_leaves_via_mgmt(
        self, to_split_trial, send, rig
    ):
        # On reply to an off-subnet management client: leaves via mgmt. mgmt is static with the container as its gateway;
        # a client address outside the subnet connects (so the SYN-ACK went to the gateway on mgmt, not to the backbone).
        mgmt = {
            "vlan": rig["mgmt"],
            "ipv4": {
                "dhcp": False,
                "addr": "192.0.2.2",
                "mask": "255.255.255.0",
                "gw": "192.0.2.1",
                "dns": "192.0.2.1",
            },
        }
        to_split_trial(split_config(rig, mgmt))
        net_show_settled(send)
        code = textwrap.dedent("""
            import socket, time
            start = time.monotonic()
            for attempt in range(15):
                sock = socket.socket()
                sock.settimeout(4)
                sock.bind(("198.51.100.7", 0))
                try:
                    sock.connect(("192.0.2.2", 80))
                    print("connected", attempt, round(time.monotonic() - start, 1))
                    break
                except OSError as error:
                    print("attempt", attempt, error)
                    sock.close()
                    time.sleep(2)
        """)
        setup = "ip addr add 192.0.2.1/24 dev eth0 && ip addr add 198.51.100.7/32 dev eth0 && "
        result = docker_run("mgmt", sh(code), setup=setup)
        assert "connected" in result.stdout, (result.stdout, result.stderr)


class TestSplitPolicy:
    """netcfg, request policy (split mode)"""

    def test_device_api_and_gui_are_allowed_on_mgmt(self, split_board):
        # On /device/* or Web GUI request arriving on mgmt: is allowed.
        host = link_local(split_board, "mgmt")
        assert http_from("mgmt", host, "GET", "/device/status")[0] == requests.codes.ok
        assert http_from("mgmt", host, "GET", "/index.html")[0] == requests.codes.ok

    def test_device_api_and_gui_are_refused_on_thread_bb_and_the_connection_closes(
        self, split_board
    ):
        # On /device/* or Web GUI request arriving on thread_bb: returns 403 and closes the connection.
        host = link_local(split_board, "thread_bb")
        for path in ("/device/status", "/index.html"):
            status, closed, _ = http_from("thread", host, "GET", path)
            assert (status, closed) == (requests.codes.forbidden, True), path

    def test_dataset_write_is_refused_on_mgmt(self, split_board):
        # On PUT /node/dataset/active arriving on mgmt: returns 403.
        host = link_local(split_board, "mgmt")
        assert (
            http_from("mgmt", host, "PUT", "/node/dataset/active", "{}")[0]
            == requests.codes.forbidden
        )

    def test_node_state_read_is_allowed_on_mgmt(self, split_board):
        # On GET /node/state arriving on mgmt: is allowed.
        host = link_local(split_board, "mgmt")
        status = http_from("mgmt", host, "GET", "/node/state")[0]
        assert status is not None and status != requests.codes.forbidden

    def test_node_and_api_requests_are_allowed_on_thread_bb(self, split_board):
        # On any /node/* or /api/* request arriving on thread_bb: is allowed.
        host = link_local(split_board, "thread_bb")
        for method, path, body in (
            ("GET", "/node/state", ""),
            ("GET", "/api/node", ""),
            ("PUT", "/node/dataset/active", "{}"),
        ):
            status = http_from("thread", host, method, path, body)[0]
            assert status is not None and status != requests.codes.forbidden, (
                method,
                path,
                status,
            )

    @pytest.mark.skipif(
        not OTBR_COMPAT.exists(), reason="tools/otbr_compat.py is missing"
    )
    def test_home_assistant_rest_works_on_thread_bb_and_is_refused_on_mgmt(
        self, split_board
    ):
        # HA REST: otbr_compat passes against the thread_bb address; against the mgmt address the first write gets 403.
        thread_url = f"http://[{link_local(split_board, 'thread_bb')}]"
        mgmt_url = f"http://[{link_local(split_board, 'mgmt')}]"
        install = "pip install -q python-otbr-api && "
        run = ["python", "/repo/tools/otbr_compat.py", "--url"]
        ok = docker_run("thread", [*run, thread_url], setup=install, timeout=300)
        assert ok.returncode == 0, ok.stdout + ok.stderr
        refused = docker_run("mgmt", [*run, mgmt_url], setup=install, timeout=300)
        assert refused.returncode != 0 and "403" in refused.stdout + refused.stderr


class TestSingleModePolicy:
    """netcfg, request policy (single mode)"""

    def test_any_request_is_allowed(self, http):
        # On any request: is allowed.
        assert http.get("/device/status").status_code == requests.codes.ok
        assert http.get("/index.html").status_code == requests.codes.ok
        assert http.get("/node/state").status_code != requests.codes.forbidden
        assert (
            http.put("/node/dataset/active", data="{}").status_code
            != requests.codes.forbidden
        )


class TestConfirmOrRevert:
    """netcfg, confirm-or-revert"""

    def test_put_with_a_valid_config_reboots_into_the_trial(self, to_split_trial, send):
        # On PUT /device/network with valid config: stores it as pending trial and reboots into it.
        to_split_trial()
        state = net_show(send)
        assert state["mode"] == "split"
        assert state["trial"]["mode"] == "split"
        assert 0 < state["trial"]["remaining_s"] <= CONFIRM_WINDOW_S
        assert state["active"]["mode"] == "single"

    def test_put_with_an_invalid_config_names_the_field_and_stores_nothing(
        self, dut, board_ip, send
    ):
        # On PUT /device/network with invalid config: returns 400 naming the field and stores nothing.
        equal = {"vlan": 20, "ipv4": {"dhcp": True}}
        reply = requests.put(
            f"http://{board_ip}/device/network",
            json={"mode": "split", "mgmt": equal, "thread_bb": equal},
            timeout=10,
        )
        assert reply.status_code == requests.codes.bad_request
        assert reply.json() == {"error": "invalid_config", "detail": "vlan"}
        state = net_show(send)
        assert state["trial"] is None and state["mode"] == "single"

    def test_confirm_on_the_mgmt_interface_makes_the_trial_active(
        self, to_split_trial, send
    ):
        # On POST /device/network/confirm on the trial's management interface within 5 min: makes the trial the active config.
        to_split_trial()
        state = net_show_settled(send)
        status, _, body = http_from(
            "mgmt", link_local(state, "mgmt"), "POST", "/device/network/confirm"
        )
        assert status == requests.codes.ok, body
        state = net_show(send)
        assert state["trial"] is None and state["active"]["mode"] == "split"

    def test_confirm_on_the_wrong_interface_is_refused_and_the_trial_stays(
        self, to_split_trial, send
    ):
        # On POST /device/network/confirm on the wrong interface: returns 403 and the trial stays pending.
        to_split_trial()
        state = net_show_settled(send)
        status, _, _ = http_from(
            "thread", link_local(state, "thread_bb"), "POST", "/device/network/confirm"
        )
        assert status == requests.codes.forbidden
        assert net_show(send)["trial"] is not None

    def test_no_confirm_within_five_minutes_reboots_into_the_previous_config(
        self, dut, to_split_trial
    ):
        # On no confirm within 5 min: reboots into the previous active config.
        to_split_trial()
        dut.expect(ROM_BANNER, timeout=CONFIRM_WINDOW_S + 60)
        wait_mode(dut, "single", False)

    def test_reset_during_the_trial_boots_the_previous_config(
        self, dut, to_split_trial, reboot
    ):
        # On reset during the trial: next boot uses the previous active config.
        to_split_trial()
        reboot()
        wait_mode(dut, "single", False)

    def test_console_net_reset_restores_the_defaults(self, dut, split_board, send):
        # On console "net reset": restores defaults (single, untagged, DHCPv4) and reboots.
        assert split_board["active"]["mode"] == "split"
        send("net reset", rb"net: defaults restored")
        dut.expect(ROM_BANNER, timeout=BOOT_TIMEOUT_S)
        wait_mode(dut, "single", False)
        dut.expect(CONSOLE_READY, timeout=BOOT_TIMEOUT_S)
        state = net_show(send)
        assert state["active"]["single"] == {"vlan": 0, "ipv4": {"dhcp": True}}
