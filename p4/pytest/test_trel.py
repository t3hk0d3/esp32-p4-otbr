"""TREL switch on the board (docs/superpowers/specs/2026-11-03-trel-design.md). Hardware-gated: written with the feature,
run in the batched hardware pass. The JSON, config and isolation logic is host-tested (netcfg, device_api, metrics).

Manual check list (needs a second TREL border router of the same Thread network on the backbone link, e.g. an
ot-br-posix built with OTBR_TREL or another ESP Thread BR):
  1. `avahi-browse -rt _trel._udp` lists this board (TXT xa, xp) while Thread runs.
  2. Ethernet page, "Thread over Ethernet" card: Active, peers 1 or more, the peer table shows the other BR.
     `ot trel peers` on the console agrees; `ot neighbor table` lists the other BR.
  3. Off: the card shows Off within a poll; the other BR marks it removed (its log; `trel peers` keeps it); Thread
     devices stay reachable. On again.
  4. Split mode: from the mgmt VLAN, `nc -u <mgmt address> 12390` traffic never reaches OpenThread (the rx counter
     does not move).
  5. Sniffer start and stop: TREL is active again afterwards.

The second border router can be the TREL test peer (tools/trel_peer, docs/development.md): ot-br-posix in Docker on the
backbone link with the C6 devkit as its RCP. With it running, the peer tests below drive it with `docker exec ot-ctl`;
without it they are skipped.
"""

import os
import shutil
import subprocess
import time

import pytest
import requests
from conftest import ot, wait_ot_state

REQUEST_TIMEOUT_S = 10
# The board formed the network alone. Polled, not read from the log: base_url waits for the boot's address line, which
# comes after "Thread started" and consumes it.
LEADER_TIMEOUT_S = 60
TREL_PEER = os.environ.get(
    "TREL_PEER", "otbr-trel"
)  # container of tools/trel_peer/up.sh
ATTACH_TIMEOUT_S = 120  # the peer attaches as a child, then may become a router
LINK_TIMEOUT_S = 120  # DNS-SD discovery plus the first MLE exchange over TREL
TRAFFIC_TIMEOUT_S = 90  # MLE advertisements alone carry packets every 30 s or so
ROUTER_TIMEOUT_S = (
    240  # a REED becomes a router after a random delay of up to 120 s, plus the attach
)
SCAN_TIMEOUT_S = 120  # the Topology scan of a two-router mesh
POLL_INTERVAL_S = 1


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


def status(base_url):
    reply = requests.get(f"{base_url}/device/status", timeout=REQUEST_TIMEOUT_S)
    reply.raise_for_status()
    return reply.json()["trel"]


def active_config(base_url):
    reply = requests.get(f"{base_url}/device/network", timeout=REQUEST_TIMEOUT_S)
    reply.raise_for_status()
    return reply.json()["active"]


def put_trel(base_url, enabled):
    config = active_config(base_url)
    config["trel"] = enabled
    return requests.put(
        f"{base_url}/device/network", json=config, timeout=REQUEST_TIMEOUT_S
    )


def test_status_carries_trel(base_url):
    trel = status(base_url)
    assert trel is not None
    assert set(trel) >= {
        "enabled",
        "active",
        "peers",
        "tx_packets",
        "tx_failures",
        "rx_packets",
        "peer_list",
    }


def test_network_config_carries_trel(base_url):
    assert isinstance(active_config(base_url)["trel"], bool)


def test_trel_switches_live_without_a_reboot(dut, stored_dataset, base_url):
    wait_ot_state(dut, {"leader"}, LEADER_TIMEOUT_S)
    try:
        reply = put_trel(base_url, False)
        assert reply.status_code == 200, reply.text
        assert reply.json()["status"] == "applied"
        assert active_config(base_url)["trel"] is False
        trel = status(base_url)
        assert trel["enabled"] is False
        assert trel["active"] is False
        assert (
            requests.get(
                f"{base_url}/device/network", timeout=REQUEST_TIMEOUT_S
            ).json()["trial"]
            is None
        )
    finally:
        reply = put_trel(base_url, True)
    assert reply.status_code == 200, reply.text
    trel = status(base_url)
    assert trel["enabled"] is True
    assert trel["active"] is True


def test_trel_change_without_other_changes_is_not_a_trial(base_url):
    config = active_config(base_url)
    reply = put_trel(base_url, config["trel"] is not True)
    try:
        assert reply.status_code == 200, reply.text
        assert reply.json()["status"] != "trial"
    finally:
        put_trel(base_url, config["trel"])


def test_trel_that_is_not_a_boolean_is_refused(base_url):
    config = active_config(base_url)
    config["trel"] = "on"
    reply = requests.put(
        f"{base_url}/device/network", json=config, timeout=REQUEST_TIMEOUT_S
    )
    assert reply.status_code == 400
    assert "trel" in reply.text


class TrelPeer:
    """The TREL test peer: ot-br-posix in Docker (tools/trel_peer), driven with `docker exec ot-ctl`."""

    def __init__(self, name):
        self.name = name

    def command(self, line, timeout=10):
        reply = subprocess.run(
            ["docker", "exec", self.name, "ot-ctl", *line.split()],
            capture_output=True,
            check=False,
            text=True,
            timeout=timeout,
        )
        lines = [text.strip() for text in reply.stdout.splitlines() if text.strip()]
        if reply.returncode != 0 or not lines or lines[-1] != "Done":
            pytest.fail(f"TREL peer: `{line}` failed: {reply.stdout}{reply.stderr}")
        return lines[:-1]

    def join(self, tlvs):
        self.leave()
        self.command(f"dataset set active {tlvs}")
        self.command("ifconfig up")
        self.command("thread start")

    def leave(self):
        self.command("thread stop")
        self.command("ifconfig down")

    def trel_peers(self):
        return "\n".join(self.command("trel peers"))

    def radios_of(self, ext_address):
        """Radios the peer uses towards a neighbour, e.g. `[15.4(255), TREL(255)]`; "" when it is not a neighbour."""
        for line in self.command("multiradio neighbor list"):
            if line.startswith(f"ExtAddr:{ext_address}"):
                return line.split("Radios:", 1)[-1]
        return ""


@pytest.fixture
def trel_peer():
    """The TREL test peer, off the Thread network before and after the test; skipped when its container is not up."""
    if shutil.which("docker") is None:
        pytest.skip(
            "no docker: the TREL test peer runs in a container (tools/trel_peer)"
        )
    running = subprocess.run(
        ["docker", "inspect", "-f", "{{.State.Running}}", TREL_PEER],
        capture_output=True,
        check=False,
        text=True,
    )
    if running.stdout.strip() != "true":
        pytest.skip(
            f"TREL test peer container {TREL_PEER} is not running (tools/trel_peer/up.sh)"
        )
    peer = TrelPeer(TREL_PEER)
    peer.leave()
    yield peer
    peer.leave()


def wait_for(check, timeout, what):
    """Poll `check` every 3 s until it returns something truthy; fail naming `what` after `timeout`."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(3)
    pytest.fail(f"timed out after {timeout} s waiting for {what}")


def packets(trel):
    """(tx, rx) TREL packet counters of a /device/status `trel` object; 0 while OpenThread's state was not readable."""
    return trel["tx_packets"] or 0, trel["rx_packets"] or 0


def linked_over_trel(dut, base_url, trel_peer, tlvs):
    """Join the peer to the board's network and wait until both use TREL towards each other; the board's ext address."""
    wait_ot_state(dut, {"leader"}, LEADER_TIMEOUT_S)
    board = ot(dut, "extaddr")[-1]
    trel_peer.join(tlvs)
    wait_for(
        lambda: trel_peer.command("state")[-1] in ("child", "router", "leader"),
        ATTACH_TIMEOUT_S,
        "the TREL peer to attach",
    )
    wait_for(
        lambda: board in trel_peer.trel_peers(),
        LINK_TIMEOUT_S,
        f"the board ({board}) in the peer's TREL peers",
    )
    wait_for(
        lambda: (status(base_url)["peers"] or 0) >= 1,
        LINK_TIMEOUT_S,
        "a TREL peer on the board",
    )
    wait_for(
        lambda: "TREL" in trel_peer.radios_of(board),
        LINK_TIMEOUT_S,
        "a TREL link from the peer to the board",
    )
    return board


def wait_for_traffic(base_url):
    """Wait until the board has both sent and received TREL packets since the call."""
    before = packets(status(base_url))

    def both_grew():
        now = packets(status(base_url))
        return now[0] > before[0] and now[1] > before[1]

    wait_for(
        both_grew, TRAFFIC_TIMEOUT_S, "TREL packets sent and received by the board"
    )


def test_board_links_with_a_trel_border_router(
    dut, stored_dataset, base_url, trel_peer
):
    # On a second border router of the same network on the backbone link: both find each other over DNS-SD, the
    # peer's link to the board uses TREL next to 15.4, and TREL packets flow in both directions on the board.
    linked_over_trel(dut, base_url, trel_peer, stored_dataset)
    wait_for_traffic(base_url)
    trel = status(base_url)
    entry = peer_entry(trel, trel_peer)
    assert entry["address"]
    # Same network, a Thread neighbour: its extended PAN ID and both radio preferences are reported.
    assert entry["same_network"] is True
    assert entry["ext_pan_id"] == trel_peer.command("extpanid")[-1]
    assert isinstance(entry["preference"]["trel"], int)
    assert trel["peers"] >= 1


def test_trel_off_ends_the_link_and_on_brings_it_back(
    dut, stored_dataset, base_url, trel_peer
):
    # On TREL switched off on the board: TREL is inactive with no peers and no TREL packet moves; Thread stays up over
    # 15.4. Switched on again: the board finds the peer, the peer uses TREL towards it and packets flow, all without a
    # reboot. The peer's `trel peers` is no witness: OpenThread keeps a peer whose service was removed (it only marks
    # its DNS-SD state, which the CLI does not show).
    board = linked_over_trel(dut, base_url, trel_peer, stored_dataset)
    try:
        assert put_trel(base_url, False).status_code == 200
        trel = status(base_url)
        assert (trel["active"], trel["peers"]) == (False, 0)
        before = packets(trel)
        time.sleep(TRAFFIC_TIMEOUT_S / 3)
        assert packets(status(base_url)) == before
        assert trel_peer.command("state")[-1] in ("child", "router", "leader")
    finally:
        assert put_trel(base_url, True).status_code == 200
    wait_for(
        lambda: (status(base_url)["peers"] or 0) >= 1,
        LINK_TIMEOUT_S,
        "the peer back on the board",
    )
    wait_for(
        lambda: "TREL" in trel_peer.radios_of(board),
        LINK_TIMEOUT_S,
        "the TREL link back",
    )
    wait_for_traffic(base_url)


def peer_entry(trel, trel_peer):
    """The TREL peer's entry in a /device/status `trel` object (fails when the board does not list it)."""
    ext = trel_peer.command("extaddr")[-1]
    for entry in trel["peer_list"]:
        if entry["ext_address"] == ext:
            return entry
    pytest.fail(
        f"the TREL peer {ext} is not in the board's peer list: {trel['peer_list']}"
    )


def test_peer_of_another_network_is_listed_apart(
    dut, stored_dataset, base_url, trel_peer
):
    # On a TREL border router of another Thread network on the link: the board lists it with same_network false and no
    # preference (never a neighbour), and counts it in peers_other, not in peers.
    wait_ot_state(dut, {"leader"}, LEADER_TIMEOUT_S)
    trel_peer.command("dataset init new")
    trel_peer.command("dataset commit active")
    trel_peer.command("ifconfig up")
    trel_peer.command("thread start")
    ext = trel_peer.command("extaddr")[-1]
    other_pan = trel_peer.command("extpanid")[-1]

    def listed_apart():
        trel = status(base_url)
        entries = [e for e in trel["peer_list"] if e["ext_address"] == ext]
        if entries and entries[0]["ext_pan_id"] == other_pan:
            return trel, entries[0]
        return None

    trel, entry = wait_for(
        listed_apart, LINK_TIMEOUT_S, "the peer listed with its own network"
    )
    assert entry["same_network"] is False
    assert entry["preference"] is None
    assert trel["peers_other"] >= 1
    assert trel["peers"] == 0


def run_scan(base_url):
    """POST /device/topology, then poll GET until the scan is no longer running; the final state object."""
    response = requests.post(f"{base_url}/device/topology", timeout=REQUEST_TIMEOUT_S)
    assert response.status_code == 202, response.text
    deadline = time.monotonic() + SCAN_TIMEOUT_S
    state = response.json()
    while state["state"] == "running" and time.monotonic() < deadline:
        time.sleep(POLL_INTERVAL_S)
        state = requests.get(
            f"{base_url}/device/topology", timeout=REQUEST_TIMEOUT_S
        ).json()
    assert state["state"] == "done", state
    return state


def test_topology_scan_shows_the_trel_link(dut, stored_dataset, base_url, trel_peer):
    # On the TREL peer linked as a router: the scan's entry for this board carries, on its link to the peer, the
    # transport with a TREL preference; the peer's own entry carries none.
    linked_over_trel(dut, base_url, trel_peer, stored_dataset)
    wait_for(
        lambda: trel_peer.command("state")[-1] in ("router", "leader"),
        ROUTER_TIMEOUT_S,
        "the TREL peer to become a router",
    )
    peer_id = int(trel_peer.command("rloc16")[-1], 16) >> 10
    routers = run_scan(base_url)["result"]["routers"]
    board = next(r for r in routers if r.get("this_device"))
    links = [link for link in board["links"] if link["router_id"] == peer_id]
    assert links, f"no link from the board to router {peer_id}: {board['links']}"
    assert isinstance(links[0]["transport"]["trel"], int)
    for router in routers:
        if not router.get("this_device"):
            assert all("transport" not in link for link in router.get("links", []))
