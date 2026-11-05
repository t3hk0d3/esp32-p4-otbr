"""topology: POST/GET /device/topology, the mesh scan behind the Topology page (spec 2026-11-02-topology-page, 3.1-3.3).
Hardware-gated: run in the batched on-board pass, never inside a task.

Rig: the board (border router and leader) and one Thread peer (ESP32 devkit running ot_cli). The peer attaches as a
child and may later become a router, so the checks read the peer's state first and assert what fits its role: the
end-device clause only when the peer is a child, the router clause when it is a router.
"""

import ipaddress
import time

import pytest
import requests
from conftest import ot, start_capture, stop_capture

# The peer and its address come from the latency tests' fixture (attach, wait for an OMR address).
from test_latency import thread_peer_address  # noqa: F401  (pytest fixture)

SCAN_TIMEOUT_S = 320  # the sleepy cap (300 s from the start) plus slack
SCAN_CAP_S = 60  # the cap of a scan without sleepy children
POLL_INTERVAL_S = 0.5
RUNNING_WAIT_S = 5
BR_RUNNING_WAIT_S = 30


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


def start_scan(base_url):
    return requests.post(f"{base_url}/device/topology", timeout=10)


def read_scan(base_url):
    response = requests.get(f"{base_url}/device/topology", timeout=10)
    assert response.status_code == requests.codes.ok, response.text
    return response.json()


def run_scan(base_url, timeout=SCAN_TIMEOUT_S):
    """POST, then poll GET until the scan is no longer running; return the final state object."""
    response = start_scan(base_url)
    assert response.status_code == requests.codes.accepted, response.text
    deadline = time.monotonic() + timeout
    state = response.json()
    while state["state"] == "running" and time.monotonic() < deadline:
        time.sleep(POLL_INTERVAL_S)
        state = read_scan(base_url)
    assert state["state"] != "running", f"the scan still runs after {timeout} s"
    return state


def board_router_ids(dut):
    """Router IDs in the board's own router table (`ot router list`)."""
    lines = ot(dut, "router list")
    return {int(item) for line in lines for item in line.split() if item.isdigit()}


def wait_for_mode(base_url, mode, timeout):
    deadline = time.monotonic() + timeout
    current = None
    while time.monotonic() < deadline:
        try:
            current = requests.get(f"{base_url}/device/status", timeout=10).json()[
                "mode"
            ]
        except (requests.RequestException, ValueError):
            current = None
        if current == mode:
            return
        time.sleep(0.5)
    pytest.fail(f"mode is {current}, expected {mode}")


def in_prefix(address, prefixes):
    ip = ipaddress.ip_address(address)
    return any(ip in ipaddress.ip_network(prefix, strict=False) for prefix in prefixes)


@pytest.fixture
def scan(request, dut, peer):
    """One finished scan of the rig, with what the checks compare it against.

    Returns a dict: result (the `result` object), peer_state, peer_ext (hex, lower case), router_ids (the board's
    router table, read just before the scan).

    thread_peer_address comes before base_url: it orders stored_dataset before board_ip, and the dataset store may
    reboot the board, so board_ip must wait for the address of the final boot (see its docstring)."""
    request.getfixturevalue(
        "thread_peer_address"
    )  # the peer is attached, with an OMR address
    base_url = request.getfixturevalue("base_url")
    peer_state = peer.wait_state({"child", "router", "leader"}, 30)
    peer_ext = peer.command("extaddr")[-1].lower()
    router_ids = board_router_ids(dut)
    state = run_scan(base_url)
    assert state["state"] == "done", state
    after = peer.command("state")[-1]
    if after != peer_state:
        # A child can promote to router during the scan: the result may or may not show the new role.
        pytest.skip(f"the peer changed role during the scan ({peer_state} -> {after})")
    return {
        "result": state["result"],
        "peer_state": peer_state,
        "peer_ext": peer_ext,
        "router_ids": router_ids,
    }


class TestTopologyScan:
    """device_api POST/GET /device/topology"""

    @pytest.mark.usefixtures("thread_peer_address")
    def test_scan_completes_within_60_s(self, base_url):
        # On POST then GET polling with the rig attached: the state ends done with a result and an age, within 60 s
        # when the mesh has no sleepy child (the scan waits for sleepy children up to 300 s).
        started = time.monotonic()
        state = run_scan(base_url)
        elapsed = time.monotonic() - started
        assert state["state"] == "done"
        assert "routers" in state["result"]
        assert state["age_s"] >= 0
        sleepy = [
            child
            for router in state["result"]["routers"]
            for child in router.get("children", [])
            if not child["mode"]["rx_on_when_idle"]
        ]
        if not sleepy:
            assert elapsed <= SCAN_CAP_S + 2 * POLL_INTERVAL_S, f"took {elapsed:.1f} s"

    def test_every_router_of_the_board_appears(self, scan):
        # On the rig after a finished scan: every router in the board's router table is in the result.
        listed = {router["router_id"] for router in scan["result"]["routers"]}
        assert scan["router_ids"] <= listed

    def test_result_has_the_board_as_this_device(self, scan):
        # On the rig after a finished scan: exactly one router is this device, and it is the leader.
        own = [r for r in scan["result"]["routers"] if r["this_device"]]
        assert len(own) == 1
        assert own[0]["leader"] is True
        assert own[0]["rloc16"] == scan["result"]["network"]["this_rloc16"]

    def test_links_need_a_known_router_and_a_positive_lq(self, scan):
        # On the rig after a finished scan: a link is listed only with lq_in > 0 and towards a router of the result;
        # no router links to itself.
        routers = scan["result"]["routers"]
        ids = {router["router_id"] for router in routers}
        for router in routers:
            for link in router.get("links", []):
                assert link["lq_in"] > 0
                assert link["router_id"] in ids
                assert link["router_id"] != router["router_id"]

    def test_router_peer_is_linked_to_the_board(self, scan):
        # On the rig after a finished scan with a router peer: one of the two lists the other with lq_in > 0 (the
        # page draws a link only then). With a child peer there is no second router and nothing to check.
        if scan["peer_state"] != "router":
            pytest.skip("the peer is a child: there is no second router to link")
        routers = {r["router_id"]: r for r in scan["result"]["routers"]}
        listed = {
            frozenset((rid, link["router_id"]))
            for rid, router in routers.items()
            for link in router.get("links", [])
            if link["lq_in"] > 0
        }
        own = next(r["router_id"] for r in routers.values() if r.get("this_device"))
        others = [rid for rid in routers if rid != own]
        assert any(frozenset((own, rid)) in listed for rid in others)

    def test_peer_appears_with_an_omr_address(self, scan):
        # On the rig after a finished scan: a peer that is a child appears under a router's children with at least
        # one address in an OMR prefix; a peer that is a router appears as a router with an OMR address.
        result = scan["result"]
        omr = result["network"]["omr_prefixes"]
        assert omr, "no OMR prefix in the network data"
        if scan["peer_state"] == "child":
            children = [
                child
                for router in result["routers"]
                for child in router.get("children", [])
                if child.get("ext_address", "").lower() == scan["peer_ext"]
            ]
            assert len(children) == 1
            assert any(in_prefix(a, omr) for a in children[0]["addresses"])
        else:
            routers = [
                r
                for r in result["routers"]
                if r.get("ext_address", "").lower() == scan["peer_ext"]
            ]
            assert len(routers) == 1
            assert any(in_prefix(a, omr) for a in routers[0]["addresses"])

    def test_peer_answered_the_info_query(self, scan):
        # On the rig after a finished scan: the peer, as a child or a router, answered its device info query. Its
        # vendor strings are not checked: the rig's ot_cli peer does not promise any.
        result = scan["result"]
        if scan["peer_state"] == "child":
            entries = [
                child
                for router in result["routers"]
                for child in router.get("children", [])
            ]
        else:
            entries = result["routers"]
        peer = [
            e for e in entries if e.get("ext_address", "").lower() == scan["peer_ext"]
        ]
        assert len(peer) == 1
        assert peer[0]["info"]["state"] == "answered", peer[0]["info"]

    @pytest.mark.usefixtures("thread_peer_address")
    def test_post_during_a_running_scan_returns_202(self, base_url):
        # On POST while a scan runs: returns 202 and joins the running scan (no second scan, no error).
        first = start_scan(base_url)
        assert first.status_code == requests.codes.accepted, first.text
        second = start_scan(base_url)
        assert second.status_code == requests.codes.accepted, second.text
        assert second.json()["state"] in {"running", "done"}
        deadline = time.monotonic() + SCAN_TIMEOUT_S
        while read_scan(base_url)["state"] == "running" and time.monotonic() < deadline:
            time.sleep(POLL_INTERVAL_S)
        assert read_scan(base_url)["state"] == "done"

    @pytest.mark.usefixtures("thread_peer_address")
    def test_get_reports_progress_while_running(self, base_url):
        # On GET while a scan runs: the state is running with a progress object until it ends.
        assert start_scan(base_url).status_code == requests.codes.accepted
        deadline = time.monotonic() + RUNNING_WAIT_S
        state = read_scan(base_url)
        while state["state"] == "running" and "progress" not in state:
            assert time.monotonic() < deadline
            time.sleep(POLL_INTERVAL_S / 5)
            state = read_scan(base_url)
        if state["state"] == "running":
            assert state["progress"]["queried"] <= max(state["progress"]["routers"], 0)
        deadline = time.monotonic() + SCAN_TIMEOUT_S
        while read_scan(base_url)["state"] == "running" and time.monotonic() < deadline:
            time.sleep(POLL_INTERVAL_S)

    def test_post_in_sniffer_mode_returns_409(self, base_url):
        # On SNIFFER: returns 409 busy with detail `mode SNIFFER`.
        stream = None
        try:
            stream = start_capture(base_url)
            response = start_scan(base_url)
            assert response.status_code == requests.codes.conflict, response.text
            body = response.json()
            assert body["error"] == "busy"
            assert body["detail"] == "mode SNIFFER"
        finally:
            stop_capture(base_url, stream)
            wait_for_mode(base_url, "BR_RUNNING", BR_RUNNING_WAIT_S)
