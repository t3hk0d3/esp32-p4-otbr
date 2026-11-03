"""TREL switch on the board (docs/superpowers/specs/2026-11-03-trel-design.md). Hardware-gated: written with the feature,
run in the batched hardware pass. The JSON, config and isolation logic is host-tested (netcfg, device_api, metrics).

Manual check list (needs a second TREL border router of the same Thread network on the backbone link, e.g. an
ot-br-posix built with OTBR_TREL or another ESP Thread BR):
  1. `avahi-browse -rt _trel._udp` lists this board (TXT xa, xp) while Thread runs.
  2. Ethernet page, "Thread over Ethernet" card: Active, peers 1 or more, the peer table shows the other BR.
     `ot trel peers` on the console agrees; `ot neighbor table` lists the other BR.
  3. Off: the card shows Off within a poll; the other BR loses the TREL peer; Thread devices stay reachable. On again.
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
from conftest import BOOT_TIMEOUT_S, ot

REQUEST_TIMEOUT_S = 10
THREAD_UP = rb"br_core: Thread started with the stored dataset"
TREL_PEER = os.environ.get(
    "TREL_PEER", "otbr-trel"
)  # container of tools/trel_peer/up.sh
ATTACH_TIMEOUT_S = 120  # the peer attaches as a child, then may become a router
LINK_TIMEOUT_S = 120  # DNS-SD discovery plus the first MLE exchange over TREL
TRAFFIC_TIMEOUT_S = 90  # MLE advertisements alone carry packets every 30 s or so


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
    dut.expect(THREAD_UP, timeout=BOOT_TIMEOUT_S)
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
    dut.expect(THREAD_UP, timeout=BOOT_TIMEOUT_S)
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


def test_board_links_with_a_trel_border_router(
    dut, stored_dataset, base_url, trel_peer
):
    # On a second border router of the same network on the backbone link: both find each other over DNS-SD, the
    # peer's link to the board uses TREL next to 15.4, and TREL packets flow in both directions on the board.
    linked_over_trel(dut, base_url, trel_peer, stored_dataset)
    before = packets(status(base_url))

    def both_grew():
        now = packets(status(base_url))
        return now[0] > before[0] and now[1] > before[1]

    wait_for(
        both_grew, TRAFFIC_TIMEOUT_S, "TREL packets sent and received by the board"
    )
    peer_entry = status(base_url)["peer_list"][0]
    assert peer_entry["ext_address"] and peer_entry["address"]


def test_trel_off_ends_the_link_and_on_brings_it_back(
    dut, stored_dataset, base_url, trel_peer
):
    # On TREL switched off on the board: the board leaves the peer's TREL peer table and the peer stops using TREL
    # towards it; Thread stays up over 15.4. Switched on again: the TREL link returns, all without a reboot.
    board = linked_over_trel(dut, base_url, trel_peer, stored_dataset)
    try:
        assert put_trel(base_url, False).status_code == 200
        wait_for(
            lambda: board not in trel_peer.trel_peers(),
            LINK_TIMEOUT_S,
            "the board to leave the peer's TREL peers",
        )
        assert trel_peer.command("state")[-1] in ("child", "router", "leader")
    finally:
        assert put_trel(base_url, True).status_code == 200
    wait_for(
        lambda: board in trel_peer.trel_peers(),
        LINK_TIMEOUT_S,
        "the board back in the peer's TREL peers",
    )
    wait_for(
        lambda: "TREL" in trel_peer.radios_of(board),
        LINK_TIMEOUT_S,
        "the TREL link back",
    )
