"""DHCPv6 prefix delegation (spec 5.9; BDD: Task 21, DHCPv6-PD). Marked pd_router: excluded unless selected.

The user's router does not delegate prefixes, so the delegating router is tools/phase0/pdserver.py (a minimal DHCPv6
server that answers only clients asking for IA_PD) in a Docker macvlan container on the board's LAN. The test builds
the Docker network on top of the host interface PD_HOST_IF (default mgmt0, untagged like the board in single mode) and
removes the containers and the network afterwards. No host network change.

Run: PD_HOST_IF=mgmt0 pytest p4/pytest/test_pd.py -m pd_router --port /dev/ttyACM0
The no-delegation test runs on the LAN as it is (its router answers PD solicitations without a prefix).
"""

import ipaddress
import os
import subprocess
import time
from pathlib import Path

import pytest
import requests
from conftest import ot, wait_ot_state

pytestmark = pytest.mark.pd_router

REPO = Path(__file__).resolve().parents[2]
PD_SERVER = REPO / "tools" / "phase0" / "pdserver.py"
IMAGE = "python:3.14-alpine"
NETWORK = "otbr-pd-lan"
SERVER_NAME = "otbr-pd-server"
POOL = ipaddress.ip_network("fd5d:a5d0:100::/48")
DELEGATED_LENGTH = 56
SERVER_RUNTIME_S = 3000
LIFETIMES = (
    "1800,2880,3600,7200"  # T1,T2,preferred,valid: above OpenThread's 300 s minimum
)
CONTAINER_SUBNET = "fd5a:7e57:c::/64"  # unrelated to the board's prefixes; docker needs one to keep IPv6 on
SYSCTLS = [
    f"net.ipv6.conf.{scope}.{key}={value}"
    for scope in ("all", "default")
    for key, value in (
        ("disable_ipv6", 0),
        ("accept_ra", 2),
        ("accept_ra_rt_info_max_plen", 64),
    )
]
LEADER_TIMEOUT_S = 90
ATTACH_TIMEOUT_S = 120
ROUTER_UPGRADE_TIMEOUT_S = 240
DELEGATION_TIMEOUT_S = 150
SETTLE_S = 60
PING_TIMEOUT_S = 90
ADDRESS_WAIT_S = 90


def docker(*args, check=True, timeout=120):
    return subprocess.run(
        ["docker", *args],
        capture_output=True,
        text=True,
        timeout=timeout,
        check=check,
    )


@pytest.fixture(scope="module")
def lan_network():
    """Docker macvlan network on the board's LAN, created for the module and removed afterwards."""
    parent = os.environ.get("PD_HOST_IF", "mgmt0")
    if not Path("/sys/class/net", parent).exists():
        pytest.skip(f"host interface {parent} (PD_HOST_IF) does not exist")
    docker("network", "rm", NETWORK, check=False)
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


@pytest.fixture
def delegating_server(lan_network):
    """pdserver.py in a container on the LAN: delegates one /56 out of fd5d:a5d0:100::/48."""
    docker("rm", "-f", SERVER_NAME, check=False)
    docker(
        "run",
        "-d",
        "--name",
        SERVER_NAME,
        "--network",
        lan_network,
        "-v",
        f"{PD_SERVER}:/pdserver.py:ro",
        IMAGE,
        "python",
        "/pdserver.py",
        str(POOL),
        str(DELEGATED_LENGTH),
        str(SERVER_RUNTIME_S),
        LIFETIMES,
    )
    yield SERVER_NAME
    log = docker("logs", SERVER_NAME, check=False)
    print(log.stdout, log.stderr)  # shown by pytest when the test fails
    docker("rm", "-f", SERVER_NAME, check=False)


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


def device_status(base_url):
    return requests.get(f"{base_url}/device/status", timeout=10).json()


def wait_pd(base_url, predicate, timeout):
    deadline = time.monotonic() + timeout
    pd = None
    while time.monotonic() < deadline:
        try:
            pd = device_status(base_url)["pd"]
            if predicate(pd):
                return pd
        except requests.RequestException:
            pass
        time.sleep(3)
    pytest.fail(f"pd never reached the expected state, last: {pd}")


def prefixes(lines):
    """All IPv6 prefixes (`addr/len`) in console output lines, as networks."""
    found = []
    for line in lines:
        for word in line.replace(",", " ").split():
            if "/" in word:
                try:
                    found.append(ipaddress.ip_network(word, strict=False))
                except ValueError:
                    continue
    return found


def omr_prefixes(dut):
    """Prefixes printed by the CLI `br omrprefix` (local and favored)."""
    return prefixes(ot(dut, "br omrprefix"))


@pytest.fixture
def leader(dut, stored_dataset, board_ip):
    """The stored dataset of a board that is the Thread leader. board_ip is requested after stored_dataset: it waits
    for the address of the final boot, which also means that the console is up."""
    wait_ot_state(dut, {"leader"}, LEADER_TIMEOUT_S)
    return stored_dataset


def global_addresses(peer):
    found = []
    for line in peer.command("ipaddr"):
        try:
            address = ipaddress.ip_address(line.strip())
        except ValueError:
            continue
        if address.is_global or address.is_private and not address.is_link_local:
            found.append(address)
    return found


def peer_address_in(peer, network, timeout=ADDRESS_WAIT_S):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for address in global_addresses(peer):
            if address in network:
                return address
        time.sleep(3)
    pytest.fail(
        f"the Thread peer has no address in {network}: {global_addresses(peer)}"
    )


def attach_peer_as_router(peer, dataset):
    """Attach the peer and wait until it is a router: the board resolves the peer's OMR address with an address query
    that only routers answer (a child of the board is reachable on its mesh-local address only)."""
    peer.restart()  # a stopped and restarted peer silently ignores echo requests to its SLAAC address
    peer.command("routereligible enable")
    peer.command(f"dataset set active {dataset}")
    peer.command("ifconfig up")
    peer.command("thread start")
    peer.wait_state({"child", "router", "leader"}, ATTACH_TIMEOUT_S)
    peer.wait_state({"router", "leader"}, ROUTER_UPGRADE_TIMEOUT_S)


def ping_from_lan(network, address):
    """Ping `address` from a fresh container on the LAN until it answers (the container first learns the route from
    the board's router advertisement).

    The echo carries a source address that the board can answer: one that the LAN router's advertisement gave the
    container (SLAAC, on-link). Left alone, the kernel picks the source by longest common prefix with the destination
    (RFC 6724 rule 8): for a destination in fd5d:a5d0:100::/48 that is docker's static address in CONTAINER_SUBNET,
    which is off-link for the board, so the reply has nowhere to go (the Task 21 "no answer": the peer did reply, the
    board did not send the reply onto the LAN). See pd-investigation-report.md."""
    script = (
        f"for i in $(seq {PING_TIMEOUT_S // 3}); do "
        "src=$(ip -6 -o addr show dev eth0 scope global | grep -v ' fd5a:7e57:c:' | sed -n 's/.* inet6 \\([0-9a-f:]*\\)\\/.*/\\1/p' | head -n 1); "
        f'[ -n "$src" ] && ping6 -c 2 -W 1 -I "$src" {address} && exit 0; sleep 1; done; exit 1'
    )
    command = ["run", "--rm", "--network", network, "--cap-add", "NET_ADMIN"]
    for sysctl in SYSCTLS:
        command += ["--sysctl", sysctl]
    return docker(
        *command, IMAGE, "sh", "-c", script, check=False, timeout=PING_TIMEOUT_S + 60
    )


class TestDelegatingRouter:
    """DHCPv6-PD, router delegating a prefix"""

    def test_status_shows_pd_running_with_the_delegated_prefix(
        self, dut, delegating_server, leader, base_url
    ):
        # On router delegating a prefix: /device/status pd.state is running and pd.prefix is set.
        pd = wait_pd(base_url, lambda p: p["prefix"], DELEGATION_TIMEOUT_S)
        assert pd["state"] == "running"
        assert ipaddress.ip_network(pd["prefix"], strict=False).subnet_of(POOL)

    def test_omr_prefix_is_within_the_delegated_prefix(
        self, dut, delegating_server, leader, base_url
    ):
        # On router delegating a prefix: the OMR prefix (CLI "br omrprefix") is within the delegated prefix.
        pd = wait_pd(base_url, lambda p: p["prefix"], DELEGATION_TIMEOUT_S)
        delegated = ipaddress.ip_network(pd["prefix"], strict=False)
        omr = omr_prefixes(dut)
        assert any(prefix.subnet_of(delegated) for prefix in omr), (delegated, omr)

    def test_thread_peer_omr_address_answers_ping_from_the_lan(
        self, dut, delegating_server, leader, base_url, peer, lan_network
    ):
        # On router delegating a prefix: the Thread peer's OMR address answers ping from the LAN.
        pd = wait_pd(base_url, lambda p: p["prefix"], DELEGATION_TIMEOUT_S)
        delegated = ipaddress.ip_network(pd["prefix"], strict=False)
        attach_peer_as_router(peer, leader)
        address = peer_address_in(peer, delegated)
        result = ping_from_lan(lan_network, address)
        assert result.returncode == 0, result.stdout + result.stderr


class TestNoDelegatingRouter:
    """DHCPv6-PD, no delegating router (the LAN router answers without a prefix)"""

    def test_pd_has_no_prefix_and_the_local_ula_omr_prefix_is_used(
        self, dut, leader, base_url
    ):
        # On no delegating router: pd.state is running (it mirrors OpenThread's DHCPv6-PD client, which keeps
        # soliciting; ruling R34), pd.prefix is null and the ULA OMR prefix is used.
        time.sleep(
            SETTLE_S
        )  # a late delegation would show up within the first solicitations
        pd = device_status(base_url)["pd"]
        assert pd["prefix"] is None
        assert pd["state"] == "running"
        omr = omr_prefixes(dut)
        assert omr, "no OMR prefix"
        assert not any(prefix.subnet_of(POOL) for prefix in omr)
        assert all(prefix.network_address.is_private for prefix in omr)
