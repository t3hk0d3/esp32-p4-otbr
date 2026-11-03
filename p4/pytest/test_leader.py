"""leader weight: GET/PUT /device/thread, the weight adjustment and the take-over (spec 2026-11-03-leader-weight).
Hardware-gated: run in the batched on-board pass, never inside a task.

Rig: the board (border router and leader, default weight 65) and one Thread peer (ESP32 devkit running ot_cli). The peer
joins the board's network like the latency tests' peer, is made leader at weight 70 (OpenThread only takes over from a
strictly lower weight), and the board then takes the role back through PUT /device/thread.
"""

import time

import pytest
import requests
from conftest import BOOT_TIMEOUT_S, GOT_IP, wait_reachable

# The peer joins the board's network here (stored_dataset before board_ip: the dataset store may reboot the board).
from test_latency import thread_peer_address  # noqa: F401  (pytest fixture)

PEER_WEIGHT = 70
PEER_HIGH_WEIGHT = 82
ADJUSTMENT = 6
MAX_ADJUSTMENT = 16
BASE_WEIGHT = 65
TAKE_OVER_WAIT_S = 30
PARTITION_WAIT_S = 90
POLL_S = 2


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


@pytest.fixture
def adjustment_reset(thread_peer_address, base_url):  # noqa: ARG001  (orders the rig first)
    """Yield the base URL; afterwards put the adjustment back to 0 (the board may be mid-reboot: failures are ignored)."""
    yield base_url
    try:
        requests.put(
            f"{base_url}/device/thread",
            json={"leader_weight_adjustment": 0},
            timeout=10,
        )
    except requests.RequestException:
        pass


def get_thread(base_url):
    response = requests.get(f"{base_url}/device/thread", timeout=10)
    assert response.status_code == 200
    return response.json()


def put_thread(base_url, body):
    return requests.put(f"{base_url}/device/thread", json=body, timeout=10)


def board_state(base_url):
    return requests.get(f"{base_url}/node", timeout=10).json()["state"]


def wait_board_state(base_url, states, timeout):
    deadline = time.monotonic() + timeout
    state = None
    while time.monotonic() < deadline:
        try:
            state = board_state(base_url)
        except (requests.RequestException, KeyError, ValueError):
            state = None
        if state in states:
            return state
        time.sleep(POLL_S)
    pytest.fail(f"board state is {state!r}, expected one of {states}")


def peer_takes_over(peer, weight):
    """Make the peer leader at `weight`; the board's own state follows as the partitions merge."""
    peer.command(f"leaderweight {weight}")
    peer.command("state leader")
    assert peer.wait_state({"leader"}, TAKE_OVER_WAIT_S) == "leader"


def wait_thread_weight(base_url, timeout=BOOT_TIMEOUT_S):
    """GET /device/thread until the weight fields are there (OpenThread is running)."""
    deadline = time.monotonic() + timeout
    body = {}
    while time.monotonic() < deadline:
        try:
            body = get_thread(base_url)
        except (requests.RequestException, ValueError, AssertionError):
            body = {}
        if "leader_weight" in body:
            return body
        time.sleep(POLL_S)
    pytest.fail(f"no leader_weight after {timeout} s: {body}")


class TestLeaderWeight:
    """device_api GET/PUT /device/thread"""

    def test_a_peer_leader_with_a_higher_weight_is_replaced_by_the_board(
        self, adjustment_reset, peer, reboot, dut
    ):
        # On a peer leader at 70: refused at 65, taken over at 71 (+6); then refused again at 81 against a peer at 82,
        # and the stored adjustment survives a reboot.
        base_url = adjustment_reset
        peer_takes_over(peer, PEER_WEIGHT)
        wait_board_state(base_url, {"router", "child"}, PARTITION_WAIT_S)
        before = get_thread(base_url)
        assert before["leader_weight_adjustment"] == 0
        assert before["leader_weight"] == BASE_WEIGHT

        refused = put_thread(base_url, {"become_leader": True})
        assert refused.status_code == 409
        assert refused.json()["error"] == "cannot_take_over"
        assert refused.json()["detail"] == "weight not higher"

        raised = put_thread(base_url, {"leader_weight_adjustment": ADJUSTMENT})
        assert raised.status_code == 200
        assert raised.json()["applied"] is True
        assert get_thread(base_url)["leader_weight"] == BASE_WEIGHT + ADJUSTMENT

        started = put_thread(base_url, {"become_leader": True})
        assert started.status_code == 200
        assert started.json()["become_leader"] == "started"
        wait_board_state(base_url, {"leader"}, TAKE_OVER_WAIT_S)
        assert peer.wait_state({"router", "child"}, PARTITION_WAIT_S) in (
            "router",
            "child",
        )

        peer_takes_over(peer, PEER_HIGH_WEIGHT)
        wait_board_state(base_url, {"router", "child"}, PARTITION_WAIT_S)
        capped = put_thread(
            base_url,
            {"leader_weight_adjustment": MAX_ADJUSTMENT, "become_leader": True},
        )
        assert capped.status_code == 409
        assert capped.json()["detail"] == "weight not higher"

        reboot()
        address = dut.expect(GOT_IP, timeout=BOOT_TIMEOUT_S).group(1).decode()
        wait_reachable(address)
        after = wait_thread_weight(f"http://{address}")
        assert after["leader_weight_adjustment"] == MAX_ADJUSTMENT
        assert after["leader_weight"] == BASE_WEIGHT + MAX_ADJUSTMENT
