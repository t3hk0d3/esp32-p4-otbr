"""Mesh routing: BGP, RIPng and the custom OMR prefix (spec docs/superpowers/specs/2026-11-03-mesh-routing-design.md,
sections 7 and 9). Marked routing_rig: excluded unless selected.

The routers are FRR and BIRD 2 containers on a Docker macvlan network built on the host interface ROUTING_HOST_IF
(default mgmt0, the board's untagged LAN in single mode), like test_pd.py. The containers take their addresses from
the LAN router's advertisements, as the board does. Every test configures the board with PUT /device/routing and
puts back the settings it found (GET before, PUT after). No host network change.

Flash once (make build-test && make flash), then run without the per-test autoflash (the module refuses to run
without --skip-autoflash y; in a worktree point TARGET_PYTEST at the main checkout's .venv):
Run: make test-target TEST=p4/pytest/test_routing.py MARK=routing_rig TIMEOUT=1800
         TARGET_PYTEST=".venv/bin/pytest --skip-autoflash y"
The route-feed test sends ROUTING_FEED_ROUTES routes (default 10k). The custom-prefix test needs the Thread peer (PEER_PORT) and joins it
to the board's current Thread network as a child. The sniffer test stops Thread on the board for a few seconds.

Recovery after a killed run (timeout, SIGTERM): each test writes the settings it found to
p4/pytest/.routing-restore.json (gitignored) before changing them and deletes it once they are back. If the file is
left over, put them back and delete it (the next run refuses to start while it exists):
    curl -X PUT http://<board>/device/routing -H 'Content-Type: application/json' -d @p4/pytest/.routing-restore.json
    rm p4/pytest/.routing-restore.json

The PUT validation tests of spec section 9 are in test_routing_api.py (no rig, normal on-board suite).

The link-local next-hop tests need a backbone without IPv6 prefixes in its router advertisements: the board sends the
16-byte next-hop only while it has no global address there. On a LAN with SLAAC prefixes they skip.

Images: FRR from quay.io/frrouting/frr (the FRR project's registry; Docker Hub's frrouting/frr stops at 8.4), BIRD 2
built once from debian:12.8-slim with the bookworm bird2 package (2.0.12).
"""

import ipaddress
import json
import os
import re
import subprocess
import time
from pathlib import Path

import pytest
import requests
from conftest import start_capture, stop_capture

pytestmark = pytest.mark.routing_rig

RESTORE_FILE = Path(__file__).resolve().parent / ".routing-restore.json"

FRR_IMAGE = "quay.io/frrouting/frr:10.4.1"
BIRD_BASE_IMAGE = "debian:12.8-slim"
BIRD_IMAGE = "otbr-test-bird2:bookworm"
BIRD_DOCKERFILE = (
    f"FROM {BIRD_BASE_IMAGE}\n"
    "RUN apt-get update && apt-get install -y --no-install-recommends bird2 iproute2 "
    "&& rm -rf /var/lib/apt/lists/*\n"
)
BIRD_SOCKET = "/run/bird.ctl"
NETWORK = "otbr-routing-lan"
CONTAINER_SUBNET = "fd5a:7e57:d::/64"  # docker needs one to keep IPv6 on; unrelated to the board's prefixes
DOCKER_GATEWAY = (
    "fd5a:7e57:d::1"  # on-link in every container: the next-hop of the generated routes
)
SYSCTLS = [
    f"net.ipv6.conf.{scope}.{key}={value}"
    for scope in ("all", "default")
    for key, value in (("disable_ipv6", 0), ("accept_ra", 2))
]
ROUTER_CAPS = ["NET_ADMIN", "NET_RAW", "SYS_ADMIN"]

CUSTOM = ipaddress.ip_network("2001:db8:42:100::/64")
CUSTOM_2 = ipaddress.ip_network("2001:db8:42:101::/64")
BOARD_ASN = 4200000001
ROUTER_ASN = 4200000002  # eBGP
ROUTER_ID = "192.0.2.1"
FEED_PREFIX = "2001:db8:f000"  # the feed is FEED_PREFIX:<n>::/64, n < 65536
# The received table the board must drain (a full IPv6 table is about 250k routes).
FEED_ROUTES = int(os.environ.get("ROUTING_FEED_ROUTES", "10000"))
UNREACHABLE_PEER = (
    "2001:db8:ffff::1"  # a configured peer for method bgp that never answers
)

ADDRESS_TIMEOUT_S = 60
SESSION_TIMEOUT_S = 120
ROUTE_TIMEOUT_S = 60
SNIFFER_WITHDRAW_S = 1.0
FRR_SLACK_S = 0.2  # a withdrawal read later than this after the deadline is the board's
RIPNG_CHANGE_S = 5.0
APPLY_SLACK_S = 1.0  # PUT, apply (about 100 ms) and the poll that sees it
RIPNG_STOP_S = 5.0  # "at once": far below the 180 s RIPng timeout
FEED_TIMEOUT_S = max(300, FEED_ROUTES // 100)
HEAP_TOLERANCE_BYTES = 32 * 1024
STATUS_TIMEOUT_S = 15
MODE_TIMEOUT_S = 120
ULA_TIMEOUT_S = 60
PEER_ATTACH_TIMEOUT_S = 120
PEER_ADDRESS_TIMEOUT_S = 120
POLL_S = 0.2


def docker(*args, check=True, timeout=120, stdin=None):
    return subprocess.run(
        ["docker", *args],
        capture_output=True,
        text=True,
        timeout=timeout,
        check=check,
        input=stdin,
    )


# ---- board ----


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


def device_status(base_url):
    return requests.get(f"{base_url}/device/status", timeout=10).json()


def get_routing(base_url):
    response = requests.get(f"{base_url}/device/routing", timeout=10)
    assert response.status_code == requests.codes.ok, response.text
    return response.json()


def put_routing(base_url, body):
    return requests.put(f"{base_url}/device/routing", json=body, timeout=10)


def settings_of(routing):
    """The PUT body of a GET /device/routing answer (everything but status)."""
    return {key: routing[key] for key in ("method", "omr", "announce_ula", "bgp")}


@pytest.fixture(scope="module", autouse=True)
def autoflash_skipped(request):
    """The rig tests run on a board flashed once: without --skip-autoflash y every test would flash it again."""
    value = request.config.getoption("skip_autoflash", default=None)
    if str(value).lower() not in ("y", "yes", "true"):
        pytest.fail(
            "run with --skip-autoflash y after flashing once "
            '(TARGET_PYTEST=".venv/bin/pytest --skip-autoflash y"; see the module docstring)'
        )


@pytest.fixture
def original_routing(base_url):
    """The board's routing settings before the test; put back afterwards. Also kept in RESTORE_FILE until they are back,
    for recovery after a killed run (module docstring)."""
    if RESTORE_FILE.exists():
        pytest.fail(
            f"{RESTORE_FILE} is left over from a killed run: put those settings back first (module docstring)"
        )
    original = settings_of(get_routing(base_url))
    RESTORE_FILE.write_text(json.dumps(original))
    print(f"routing settings to restore saved in {RESTORE_FILE}")
    yield original
    response = put_routing(base_url, original)
    assert response.status_code == requests.codes.ok, response.text
    RESTORE_FILE.unlink()


def configure(base_url, original, method, omr=None, announce_ula=False, bgp=None):
    """PUT the settings; BGP settings left out keep the original ones. Returns the new GET shape."""
    body = {
        "method": method,
        "omr": omr or {"mode": "auto"},
        "announce_ula": announce_ula,
        "bgp": bgp or original["bgp"],
    }
    response = put_routing(base_url, body)
    assert response.status_code == requests.codes.ok, response.text
    return response.json()


def custom(prefix):
    return {"mode": "custom", "prefix": str(prefix)}


def bgp_settings(peer_address, peer_asn, local_asn=BOARD_ASN):
    return {
        "local_asn": local_asn,
        "router_id": None,
        "hold_time": 90,
        "peers": [{"address": str(peer_address), "asn": peer_asn}],
    }


def board_addresses(base_url):
    """The board's backbone addresses (single mode: eth), as (link-local, [globals])."""
    addresses = [
        ipaddress.ip_address(a) for a in device_status(base_url)["eth"]["ipv6"]
    ]
    link_local = [a for a in addresses if a.is_link_local]
    if not link_local:
        pytest.fail(f"the board has no link-local address: {addresses}")
    return link_local[0], [a for a in addresses if not a.is_link_local]


def ula_prefix(base_url):
    """The board's self-generated ULA (status.ula_prefix), once the publisher learned it."""
    deadline = time.monotonic() + ULA_TIMEOUT_S
    while time.monotonic() < deadline:
        ula = get_routing(base_url)["status"]["ula_prefix"]
        if ula:
            return ipaddress.ip_network(ula)
        time.sleep(1)
    pytest.fail("status.ula_prefix stays null: the border router has not started")


def wait_mode(base_url, mode, timeout=MODE_TIMEOUT_S):
    deadline = time.monotonic() + timeout
    current = None
    while time.monotonic() < deadline:
        try:
            current = device_status(base_url)["mode"]
        except requests.RequestException:
            current = None
        if current == mode:
            return
        time.sleep(1)
    pytest.fail(f"mode is {current}, expected {mode}")


def board_peer(base_url):
    """status.bgp.peers[0] of GET /device/routing."""
    return get_routing(base_url)["status"]["bgp"]["peers"][0]


def heap(base_url):
    return requests.get(f"{base_url}/device/diag", timeout=10).json()["memory"]


# ---- containers ----


@pytest.fixture(scope="module")
def lan_network():
    """Docker macvlan network on the board's LAN, created for the module and removed afterwards."""
    parent = os.environ.get("ROUTING_HOST_IF", "mgmt0")
    if not Path("/sys/class/net", parent).exists():
        pytest.skip(f"host interface {parent} (ROUTING_HOST_IF) does not exist")
    try:
        docker("network", "rm", NETWORK, check=False)
    except (FileNotFoundError, subprocess.TimeoutExpired) as error:
        pytest.skip(f"docker is not available: {error}")
    created = docker(
        "network",
        "create",
        "-d",
        "macvlan",
        "-o",
        f"parent={parent}",
        "--ipv6",
        "--subnet",
        CONTAINER_SUBNET,
        NETWORK,
        check=False,
    )
    if created.returncode:
        pytest.skip(f"cannot create the macvlan network: {created.stderr.strip()}")
    yield NETWORK
    docker("network", "rm", NETWORK, check=False)


@pytest.fixture(scope="module")
def bird_image():
    """The BIRD 2 image, built once (the build runs on docker's default network, not on the LAN)."""
    built = docker(
        "build", "-t", BIRD_IMAGE, "-", check=False, timeout=600, stdin=BIRD_DOCKERFILE
    )
    if built.returncode:
        pytest.skip(f"cannot build the BIRD image: {built.stderr.strip()[-500:]}")
    return BIRD_IMAGE


@pytest.fixture
def containers(lan_network, tmp_path):
    """run(name, image, files, command): start a router container on the LAN with `files` ({path: text}) copied in;
    every one is removed afterwards (its log is printed first, shown by pytest when the test fails)."""
    started = []

    def run(name, image, files, command=()):
        docker("rm", "-f", name, check=False)
        args = ["create", "--name", name, "--network", lan_network]
        for cap in ROUTER_CAPS:
            args += ["--cap-add", cap]
        for sysctl in SYSCTLS:
            args += ["--sysctl", sysctl]
        docker(*args, image, *command, timeout=300)  # the first run pulls the image
        started.append(name)
        for path, text in files.items():
            source = tmp_path / f"{name}-{Path(path).name}"
            source.write_text(text)
            docker("cp", str(source), f"{name}:{path}")
        docker("start", name)
        return name

    yield run
    for name in started:
        log = docker("logs", name, check=False)
        print(log.stdout, log.stderr)
        docker("rm", "-f", name, check=False)


def container_addresses(name):
    """The container's addresses on the LAN, as (link-local, [addresses from the LAN router's prefixes])."""
    out = docker("exec", name, "ip", "-6", "-o", "addr", "show", "dev", "eth0").stdout
    found = [ipaddress.ip_address(a) for a in re.findall(r"inet6 ([0-9a-f:]+)/", out)]
    docker_subnet = ipaddress.ip_network(CONTAINER_SUBNET)
    link_local = next((a for a in found if a.is_link_local), None)
    return link_local, [
        a for a in found if not a.is_link_local and a not in docker_subnet
    ]


def container_address_in(name, board_address):
    """The container's address in the /64 of `board_address`, once it has autoconfigured one."""
    link = ipaddress.ip_network(f"{board_address}/64", strict=False)
    deadline = time.monotonic() + ADDRESS_TIMEOUT_S
    while time.monotonic() < deadline:
        _, mine = container_addresses(name)
        for address in mine:
            if address in link:
                return address
        time.sleep(2)
    pytest.fail(f"{name} has no address in {link}: {container_addresses(name)}")


def container_link_local(name):
    deadline = time.monotonic() + ADDRESS_TIMEOUT_S
    while time.monotonic() < deadline:
        link_local, _ = container_addresses(name)
        if link_local:
            return link_local
        time.sleep(1)
    pytest.fail(f"{name} has no link-local address")


# ---- FRR ----

FRR_DAEMONS = """\
bgpd=yes
ripngd=yes
vtysh_enable=yes
zebra_options="  -A 127.0.0.1 -s 90000000"
bgpd_options="   -A 127.0.0.1"
ripngd_options=" -A ::1"
"""


def frr_bgp_config(
    neighbor, remote_asn, local_asn, prefixes, interface=None, feed=False
):
    """frr.conf: one BGP neighbor (the board) with the documented safeguards: an inbound prefix-list limited to the
    announced /64s, maximum-prefix, and no export to the board. feed: export the kernel routes the feed test installs instead."""
    lines = [
        "frr defaults traditional",
        "hostname otbr-test-frr",
        "log stdout",
        "!",
    ]
    lines += [
        f"ipv6 prefix-list from-board seq {5 * (i + 1)} permit {p}"
        for i, p in enumerate(prefixes)
    ]
    lines += [
        "!",
        "route-map nothing deny 10",
        "route-map everything permit 10",
        "!",
        f"router bgp {local_asn}",
        f" bgp router-id {ROUTER_ID}",
        " no bgp default ipv4-unicast",
        f" neighbor {neighbor} remote-as {remote_asn}",
        f" neighbor {neighbor} timers connect 5",
    ]
    if interface:
        lines.append(f" neighbor {neighbor} interface {interface}")
    lines += [
        " address-family ipv6 unicast",
        f"  neighbor {neighbor} activate",
        f"  neighbor {neighbor} prefix-list from-board in",
        f"  neighbor {neighbor} maximum-prefix 10",
        f"  neighbor {neighbor} route-map {'everything' if feed else 'nothing'} out",
    ]
    if feed:
        lines.append("  redistribute kernel")
    lines += [" exit-address-family", "!"]
    return "\n".join(lines) + "\n"


def frr_ripng_config(prefixes):
    """frr.conf: ripngd on eth0, accepting only the board's /64s and announcing nothing to it."""
    lines = ["frr defaults traditional", "hostname otbr-test-frr", "log stdout", "!"]
    lines += [
        f"ipv6 prefix-list from-board seq {5 * (i + 1)} permit {p}"
        for i, p in enumerate(prefixes)
    ]
    lines += [
        "ipv6 prefix-list nothing seq 5 deny any",
        "!",
        "router ripng",
        " network eth0",
        " distribute-list prefix from-board in eth0",
        " distribute-list prefix nothing out eth0",
        "!",
    ]
    return "\n".join(lines) + "\n"


@pytest.fixture
def frr(containers):
    """frr(config): start FRR with that frr.conf; returns the container name."""

    def start(config, name="otbr-routing-frr"):
        files = {
            "/etc/frr/daemons": FRR_DAEMONS,
            "/etc/frr/vtysh.conf": "service integrated-vtysh-config\n",
            "/etc/frr/frr.conf": config,
        }
        return containers(name, FRR_IMAGE, files)

    return start


def vtysh(name, command):
    return docker("exec", name, "vtysh", "-c", command).stdout


def vtysh_json(name, command):
    text = vtysh(name, command).strip()
    return json.loads(text) if text else {}


def frr_bgp_routes(name):
    """{prefix: best path} of FRR's IPv6 unicast BGP table."""
    routes = vtysh_json(name, "show bgp ipv6 unicast json").get("routes", {})
    best = {}
    for prefix, paths in routes.items():
        chosen = [p for p in paths if p.get("bestpath")] or paths
        best[ipaddress.ip_network(prefix)] = chosen[0]
    return best


def frr_neighbor(name):
    """The only BGP neighbor of `show bgp neighbors json`."""
    neighbors = vtysh_json(name, "show bgp neighbors json")
    return next(iter(neighbors.values()), {})


def wait_established(name, timeout=SESSION_TIMEOUT_S):
    deadline = time.monotonic() + timeout
    state = None
    while time.monotonic() < deadline:
        state = frr_neighbor(name).get("bgpState")
        if state == "Established":
            return
        time.sleep(1)
    pytest.fail(f"the BGP session to the board is {state}, not Established")


def wait_routes(read, wanted, timeout=ROUTE_TIMEOUT_S):
    """Poll read() until its keys are exactly `wanted`; return what it read."""
    deadline = time.monotonic() + timeout
    routes = {}
    while time.monotonic() < deadline:
        routes = read()
        if set(routes) == set(wanted):
            return routes
        time.sleep(POLL_S)
    pytest.fail(
        f"routes are {sorted(map(str, routes))}, expected {sorted(map(str, wanted))}"
    )


def frr_ripng_routes(name):
    """{prefix: (metric, [next-hops])} of the RIPng routes in FRR's RIB."""
    routes = vtysh_json(name, "show ipv6 route ripng json")
    found = {}
    for prefix, entries in routes.items():
        entry = entries[0]
        hops = [
            ipaddress.ip_address(h["ip"])
            for h in entry.get("nexthops", [])
            if "ip" in h
        ]
        found[ipaddress.ip_network(prefix)] = (entry.get("metric"), hops)
    return found


def next_hops(path):
    return {ipaddress.ip_address(h["ip"]) for h in path.get("nexthops", [])}


# ---- BIRD ----


def bird_bgp_config(neighbor, remote_asn, local_asn, prefixes):
    nets = ", ".join(str(p) for p in prefixes)
    return (
        f"router id {ROUTER_ID};\n"
        "log stderr all;\n"
        "protocol device { scan time 2; }\n"
        "protocol bgp board {\n"
        f"  local as {local_asn};\n"
        f"  neighbor {neighbor} % 'eth0' as {remote_asn};\n"
        f"  ipv6 {{ import where net ~ [ {nets} ]; import limit 10 action disable; export none; }};\n"
        "}\n"
    )


def bird_ripng_config(prefixes):
    nets = ", ".join(str(p) for p in prefixes)
    return (
        f"router id {ROUTER_ID};\n"
        "log stderr all;\n"
        "protocol device { scan time 2; }\n"
        "protocol rip ng board {\n"
        f"  ipv6 {{ import where net ~ [ {nets} ]; export none; }};\n"
        '  interface "eth0" { };\n'
        "}\n"
    )


@pytest.fixture
def bird(containers, bird_image):
    """bird(config): start BIRD 2 with that bird.conf; returns the container name."""

    def start(config, name="otbr-routing-bird"):
        command = ("bird", "-f", "-c", "/etc/bird/bird.conf", "-s", BIRD_SOCKET)
        return containers(name, bird_image, {"/etc/bird/bird.conf": config}, command)

    return start


BIRD_ROUTE = re.compile(r"^(\S+/\d+)\s+unicast\s.*?\((\d+)(?:/(\d+))?\)", re.MULTILINE)
BIRD_VIA = re.compile(r"\bvia (\S+) on ")


def bird_routes(name, protocol="board"):
    """{prefix: (metric or None, [next-hops])} of the unicast routes the protocol imported (`birdc show route all
    protocol`; a BGP route shows no metric)."""
    out = docker(
        "exec",
        name,
        "birdc",
        "-s",
        BIRD_SOCKET,
        "show",
        "route",
        "all",
        "protocol",
        protocol,
    ).stdout
    found = {}
    blocks = re.split(r"\n(?=\S+/\d+\s)", out)
    for block in blocks:
        match = BIRD_ROUTE.search(block)
        if not match:
            continue
        metric = int(match.group(3)) if match.group(3) else None
        hops = [ipaddress.ip_address(h.split("%")[0]) for h in BIRD_VIA.findall(block)]
        found[ipaddress.ip_network(match.group(1))] = (metric, hops)
    return found


# ---- tests ----


@pytest.fixture
def bgp_board(base_url, original_routing, frr):
    """bgp_board(router_asn, prefixes, feed): FRR as the board's only BGP peer on a shared /64, accepting `prefixes`
    (default CUSTOM and the ULA), the board announcing CUSTOM and its ULA. Returns (frr name, board link-local, board
    globals, ULA) once the session is established."""

    def setup(router_asn, prefixes=None, feed=False):
        ula = ula_prefix(base_url)
        link_local, globals_ = board_addresses(base_url)
        if not globals_:
            pytest.skip("the board has no global address on the backbone")
        board = globals_[0]
        name = frr(
            frr_bgp_config(
                board, BOARD_ASN, router_asn, prefixes or [CUSTOM, ula], feed=feed
            )
        )
        address = container_address_in(name, board)
        configure(
            base_url,
            original_routing,
            "bgp",
            omr=custom(CUSTOM),
            announce_ula=True,
            bgp=bgp_settings(address, router_asn),
        )
        wait_established(name)
        return name, link_local, globals_, ula

    return setup


class TestBgp:
    """routing, BGP against FRR"""

    def test_ibgp_router_learns_both_prefixes_with_the_boards_next_hop(self, bgp_board):
        # bgp ibgp: router learns both prefixes with the board's next-hop
        name, link_local, globals_, ula = bgp_board(BOARD_ASN)
        routes = wait_routes(lambda: frr_bgp_routes(name), [CUSTOM, ula])
        for prefix, path in routes.items():
            hops = next_hops(path)
            assert link_local in hops, (prefix, path)
            assert hops & set(globals_), (prefix, path)
            assert path.get("path", "") == "", (prefix, path)  # iBGP: empty AS_PATH
            assert path.get("locPrf") == 100, (prefix, path)

    def test_ebgp_as_path_is_the_boards_asn(self, bgp_board):
        # bgp ebgp: as path is the board's ASN
        name, _, _, ula = bgp_board(ROUTER_ASN)
        routes = wait_routes(lambda: frr_bgp_routes(name), [CUSTOM, ula])
        for prefix, path in routes.items():
            assert path.get("path") == str(BOARD_ASN), (prefix, path)

    def test_sniffer_mode_withdraws_within_1_s(self, base_url, bgp_board):
        # bgp: sniffer mode withdraws within 1 s
        name, _, _, ula = bgp_board(BOARD_ASN)
        wait_routes(lambda: frr_bgp_routes(name), [CUSTOM, ula])
        capture = None
        try:
            capture = start_capture(base_url)
            entered = time.monotonic()
            deadline = entered + SNIFFER_WITHDRAW_S + FRR_SLACK_S
            while True:
                polled = time.monotonic()
                routes = frr_bgp_routes(name)
                if not routes:
                    break
                # Still there in a read that began after the deadline: the board withdrew too late.
                assert polled < deadline, (
                    f"still announced {polled - entered:.2f} s after sniffer mode: {routes}"
                )
        finally:
            stop_capture(base_url, capture)
        wait_mode(base_url, "BR_RUNNING")

    def test_omr_change_causes_no_session_reset(
        self, base_url, original_routing, bgp_board
    ):
        # bgp: omr change causes no session reset
        ula = ula_prefix(base_url)
        name, _, _, _ = bgp_board(BOARD_ASN, prefixes=[CUSTOM, CUSTOM_2, ula])
        wait_routes(lambda: frr_bgp_routes(name), [CUSTOM, ula])
        before = frr_neighbor(name)
        bgp = get_routing(base_url)["bgp"]
        configure(
            base_url,
            original_routing,
            "bgp",
            omr=custom(CUSTOM),
            announce_ula=False,
            bgp=bgp,
        )
        wait_routes(lambda: frr_bgp_routes(name), [CUSTOM])
        configure(
            base_url,
            original_routing,
            "bgp",
            omr=custom(CUSTOM_2),
            announce_ula=True,
            bgp=bgp,
        )
        wait_routes(lambda: frr_bgp_routes(name), [CUSTOM_2, ula])
        after = frr_neighbor(name)
        assert after["bgpState"] == "Established"
        assert after["connectionsEstablished"] == before["connectionsEstablished"]
        assert after["connectionsDropped"] == before["connectionsDropped"]
        assert board_peer(base_url)["state"] == "established"

    def test_route_feed_leaves_free_heap_and_thread_running(self, base_url, bgp_board):
        # bgp: a received route feed (ROUTING_FEED_ROUTES, default 10k) leaves free heap and Thread running
        name, _, _, _ = bgp_board(BOARD_ASN, feed=True)
        before = heap(base_url)
        # The FRR image has no sharpd: install kernel blackhole routes, which zebra learns and bgpd redistributes.
        docker(
            "exec",
            name,
            "sh",
            "-c",
            f'i=0; while [ "$i" -lt {FEED_ROUTES} ]; do '
            f'printf "route add blackhole {FEED_PREFIX}:%x::/64\\n" "$i"; i=$((i + 1)); '
            "done > /tmp/feed && ip -6 -batch /tmp/feed",
            timeout=600,
        )
        deadline = time.monotonic() + FEED_TIMEOUT_S
        dropped = 0
        while time.monotonic() < deadline:
            assert device_status(base_url)["mode"] == "BR_RUNNING"
            peer = board_peer(base_url)
            dropped = peer["received_dropped"]
            if dropped >= FEED_ROUTES:
                break
            time.sleep(5)
        assert dropped >= FEED_ROUTES, (
            f"received_dropped is {dropped} after {FEED_TIMEOUT_S} s"
        )
        after = heap(base_url)
        for region in ("internal", "psram"):
            for key in ("free", "min_free"):
                fell = before[region][key] - after[region][key]
                assert fell <= HEAP_TOLERANCE_BYTES, (
                    region,
                    key,
                    before[region],
                    after[region],
                )
        assert device_status(base_url)["mode"] == "BR_RUNNING"
        state = requests.get(f"{base_url}/node/state", timeout=10).json()
        assert state in ("leader", "router", "child"), state
        assert board_peer(base_url)["state"] == "established"
        assert frr_neighbor(name)["bgpState"] == "Established"


class TestBgpLinkLocal:
    """routing, BGP to a link-local peer: the 16-byte next-hop (link-local only, outside RFC 2545)"""

    @pytest.fixture
    def link_local_only(self, base_url):
        link_local, globals_ = board_addresses(base_url)
        if globals_:
            pytest.skip(
                f"the board has global addresses on the backbone ({', '.join(map(str, globals_))}), so it sends "
                "the 32-byte next-hop; run on a backbone without SLAAC prefixes"
            )
        return link_local

    def test_frr_accepts_the_16_byte_next_hop(
        self, base_url, original_routing, link_local_only, frr
    ):
        # bgp link-local peer: FRR accepts the 16-byte next-hop
        ula = ula_prefix(base_url)
        name = frr(
            frr_bgp_config(
                link_local_only, BOARD_ASN, ROUTER_ASN, [CUSTOM, ula], interface="eth0"
            )
        )
        peer = container_link_local(name)
        configure(
            base_url,
            original_routing,
            "bgp",
            omr=custom(CUSTOM),
            announce_ula=True,
            bgp=bgp_settings(peer, ROUTER_ASN),
        )
        wait_established(name)
        routes = wait_routes(lambda: frr_bgp_routes(name), [CUSTOM, ula])
        for prefix, path in routes.items():
            assert path.get("valid"), (prefix, path)
            assert link_local_only in next_hops(path), (prefix, path)

    def test_bird_accepts_the_16_byte_next_hop(
        self, base_url, original_routing, link_local_only, bird
    ):
        # bgp link-local peer: BIRD accepts the 16-byte next-hop (the result goes into docs/networking.md)
        ula = ula_prefix(base_url)
        name = bird(
            bird_bgp_config(link_local_only, BOARD_ASN, ROUTER_ASN, [CUSTOM, ula])
        )
        peer = container_link_local(name)
        configure(
            base_url,
            original_routing,
            "bgp",
            omr=custom(CUSTOM),
            announce_ula=True,
            bgp=bgp_settings(peer, ROUTER_ASN),
        )
        routes = wait_routes(
            lambda: bird_routes(name), [CUSTOM, ula], timeout=SESSION_TIMEOUT_S
        )
        for prefix, (_, hops) in routes.items():
            assert link_local_only in hops, (prefix, hops)


@pytest.fixture
def ripng_router(request):
    """ripng_router(kind, prefixes): FRR's ripngd or BIRD's RIPng on eth0, accepting only `prefixes`. Returns a
    function that reads the router's RIPng routes."""

    def start(kind, prefixes):
        if kind == "frr":
            name = request.getfixturevalue("frr")(frr_ripng_config(prefixes))
            return lambda: frr_ripng_routes(name)
        name = request.getfixturevalue("bird")(bird_ripng_config(prefixes))
        return lambda: bird_routes(name)

    return start


class TestRipng:
    """routing, RIPng against FRR and BIRD"""

    @pytest.mark.parametrize("kind", ["frr", "bird"])
    def test_router_learns_both_prefixes_via_the_boards_link_local_metric_2(
        self, base_url, original_routing, ripng_router, kind
    ):
        # ripng: router learns both prefixes via the board's link-local, metric 2
        ula = ula_prefix(base_url)
        link_local, _ = board_addresses(base_url)
        read = ripng_router(kind, [CUSTOM, ula])
        configure(
            base_url, original_routing, "ripng", omr=custom(CUSTOM), announce_ula=True
        )
        routes = wait_routes(read, [CUSTOM, ula])
        for prefix, (metric, hops) in routes.items():
            assert metric == 2, (prefix, metric)
            assert hops == [link_local], (prefix, hops)

    def test_switching_to_off_withdraws_at_once(
        self, base_url, original_routing, ripng_router
    ):
        # ripng: switching to off withdraws at once
        ula = ula_prefix(base_url)
        read = ripng_router("frr", [CUSTOM, ula])
        configure(
            base_url, original_routing, "ripng", omr=custom(CUSTOM), announce_ula=True
        )
        wait_routes(read, [CUSTOM, ula])
        switched = time.monotonic()
        configure(
            base_url, original_routing, "off", omr=custom(CUSTOM), announce_ula=True
        )
        wait_routes(read, [], timeout=RIPNG_STOP_S)
        assert time.monotonic() - switched <= RIPNG_STOP_S

    def test_a_changed_set_shows_within_5_s(
        self, base_url, original_routing, ripng_router
    ):
        # ripng: a changed set shows within 5 s
        ula = ula_prefix(base_url)
        read = ripng_router("frr", [CUSTOM, CUSTOM_2, ula])
        configure(
            base_url, original_routing, "ripng", omr=custom(CUSTOM), announce_ula=True
        )
        wait_routes(read, [CUSTOM, ula])
        # The first change goes out at once; the second at most 5 s after the first triggered update.
        configure(
            base_url, original_routing, "ripng", omr=custom(CUSTOM), announce_ula=False
        )
        wait_routes(read, [CUSTOM], timeout=RIPNG_CHANGE_S + APPLY_SLACK_S)
        configure(
            base_url,
            original_routing,
            "ripng",
            omr=custom(CUSTOM_2),
            announce_ula=False,
        )
        wait_routes(read, [CUSTOM_2], timeout=RIPNG_CHANGE_S + APPLY_SLACK_S)


def active_dataset(base_url):
    response = requests.get(
        f"{base_url}/node/dataset/active", headers={"Accept": "text/plain"}, timeout=10
    )
    if response.status_code != requests.codes.ok or not response.text.strip():
        pytest.skip("the board has no active Thread dataset")
    return response.text.strip()


def peer_addresses(peer):
    found = []
    for line in peer.command("ipaddr"):
        try:
            found.append(ipaddress.ip_address(line.strip()))
        except ValueError:
            continue
    return found


class TestCustomPrefix:
    """routing, custom OMR prefix on the mesh"""

    def test_with_ula_the_c6_holds_addresses_from_both(
        self, base_url, original_routing, peer
    ):
        # custom prefix with ULA: the C6 holds addresses from both
        ula = ula_prefix(base_url)
        dataset = active_dataset(base_url)
        configure(
            base_url, original_routing, "off", omr=custom(CUSTOM), announce_ula=True
        )
        peer.restart()
        peer.command(
            "routereligible disable"
        )  # a child: it joins the board's network without changing it
        peer.command(f"dataset set active {dataset}")
        peer.command("ifconfig up")
        peer.command("thread start")
        peer.wait_state({"child", "router", "leader"}, PEER_ATTACH_TIMEOUT_S)
        deadline = time.monotonic() + PEER_ADDRESS_TIMEOUT_S
        addresses = []
        while time.monotonic() < deadline:
            addresses = peer_addresses(peer)
            if any(a in CUSTOM for a in addresses) and any(a in ula for a in addresses):
                return
            time.sleep(3)
        pytest.fail(f"the C6 has no address in both {CUSTOM} and {ula}: {addresses}")
