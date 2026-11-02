"""Join with dataset TLVs (PATCHES.md items 14 and 15) on the board. Hardware-gated: written with the feature, run in
the batched hardware pass after review. The page logic is covered on the host by tools/tests/test_network_page.py, the
REST handler by esp_ot_br_server/tests/test_rest.c.

Manual check list (browser, board in BR_RUNNING as leader, a second dataset at hand, e.g. from `ot dataset init new` +
`ot dataset tlvs` on another device):
  1. Open http://<board>/network.html: the "Join with Dataset" card sits between Discover and Form; Join is disabled.
  2. Paste garbage: the preview names the error ("Not a hex string"), Join stays disabled. Paste a dataset without the
     network key: "Not a complete active dataset: missing Network Key".
  3. Paste the second dataset split over several lines in upper case: the preview shows "Network: <name> (channel <n>,
     PAN 0x....)", Join is enabled.
  4. Join: the confirm names the current network and the new one. Cancel: nothing changes (`/node/dataset/active` same).
  5. Join and OK: the overlay shows "Dataset stored — attaching...", then "Joined — role: leader" within 60 s;
     `/node/dataset/active` is the pasted dataset. Join back with the original TLVs the same way.
  6. Fresh board (no dataset, `GET /node/state` "disabled"): the confirm names only the new network; after Join the role
     becomes leader (the page enabled Thread).
  7. Split network mode, page opened on the management address: Join ends with the message about the management address.
  8. Home Assistant, Thread panel: move the board onto the preferred network; it succeeds (it failed with 409 before) and
     the board log shows "active dataset replaced while attached, Thread restarted" or the preceding disable.
  9. Stale pending dataset: start a channel change with a long delay (`PUT /node/dataset/pending`, delay 300000), then
     join another network with the card before it expires. After the delay `GET /node/dataset/active` must still be the
     joined network (if not, item 14 has to clear the pending dataset as well).
"""

import asyncio

import aiohttp
import pytest
import python_otbr_api as api
import requests
from conftest import ot, wait_ot_state

LEADER_TIMEOUT_S = 90
REQUEST_TIMEOUT_S = 10
RESTART_LOG = rb"active dataset replaced while attached, Thread restarted"


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


@pytest.fixture
def other_dataset(dut):
    """A second complete active dataset (new keys, PAN and name), generated on the board without committing it."""
    ot(dut, "dataset init new")
    tlvs = ot(dut, "dataset tlvs")[-1]
    ot(dut, "dataset clear")
    return tlvs


def put_active(base_url, tlvs):
    return requests.put(
        f"{base_url}/node/dataset/active",
        data=tlvs,
        headers={"Content-Type": "text/plain"},
        timeout=REQUEST_TIMEOUT_S,
    )


def active_tlvs(base_url):
    response = requests.get(
        f"{base_url}/node/dataset/active",
        headers={"Accept": "text/plain"},
        timeout=REQUEST_TIMEOUT_S,
    )
    return response.text.strip().lower()


def node_state(base_url):
    return requests.get(f"{base_url}/node/state", timeout=REQUEST_TIMEOUT_S).json()


class TestServing:
    """network.html and static/dataset_join.js"""

    def test_network_page_has_the_join_card(self, base_url):
        response = requests.get(f"{base_url}/network.html", timeout=REQUEST_TIMEOUT_S)
        assert response.status_code == 200
        assert 'id="datasetTlvs"' in response.text
        assert '<script src="/static/dataset_join.js"></script>' in response.text

    def test_script_is_served(self, base_url):
        response = requests.get(
            f"{base_url}/static/dataset_join.js", timeout=REQUEST_TIMEOUT_S
        )
        assert response.status_code == 200
        assert "parseDatasetTlvs" in response.text


class TestActiveDatasetWhileAttached:
    """PUT /node/dataset/active while Thread runs (PATCHES.md item 14)"""

    def test_same_dataset_is_accepted_without_a_restart(
        self, dut, stored_dataset, base_url
    ):
        wait_ot_state(dut, {"leader"}, LEADER_TIMEOUT_S)
        assert put_active(base_url, stored_dataset).status_code == 200
        assert node_state(base_url) == "leader"
        assert active_tlvs(base_url) == stored_dataset.lower()

    def test_partial_dataset_is_refused_with_409(self, dut, stored_dataset, base_url):
        wait_ot_state(dut, {"leader"}, LEADER_TIMEOUT_S)
        response = requests.put(
            f"{base_url}/node/dataset/active",
            json={"channel": 20},
            timeout=REQUEST_TIMEOUT_S,
        )
        assert response.status_code == 409
        assert node_state(base_url) == "leader"
        assert active_tlvs(base_url) == stored_dataset.lower()

    def test_other_dataset_restarts_thread_on_it_and_back(
        self, dut, stored_dataset, other_dataset, base_url
    ):
        wait_ot_state(dut, {"leader"}, LEADER_TIMEOUT_S)
        assert put_active(base_url, other_dataset).status_code == 200
        dut.expect(RESTART_LOG, timeout=REQUEST_TIMEOUT_S)
        wait_ot_state(dut, {"leader"}, LEADER_TIMEOUT_S)
        assert active_tlvs(base_url) == other_dataset.lower()
        assert put_active(base_url, stored_dataset).status_code == 200
        dut.expect(RESTART_LOG, timeout=REQUEST_TIMEOUT_S)
        wait_ot_state(dut, {"leader"}, LEADER_TIMEOUT_S)
        assert active_tlvs(base_url) == stored_dataset.lower()

    def test_home_assistant_set_network_sequence_succeeds(
        self, dut, stored_dataset, other_dataset, base_url
    ):
        # HA otbr/set_network: set_enabled(False), set_active_dataset_tlvs, set_enabled(True).
        async def set_network(tlvs):
            async with aiohttp.ClientSession() as session:
                otbr = api.OTBR(base_url, session, timeout=20)
                await otbr.set_enabled(False)
                await otbr.set_active_dataset_tlvs(bytes.fromhex(tlvs))
                await otbr.set_enabled(True)

        wait_ot_state(dut, {"leader"}, LEADER_TIMEOUT_S)
        asyncio.run(set_network(other_dataset))
        wait_ot_state(dut, {"leader"}, LEADER_TIMEOUT_S)
        assert active_tlvs(base_url) == other_dataset.lower()
        asyncio.run(set_network(stored_dataset))
        wait_ot_state(dut, {"leader"}, LEADER_TIMEOUT_S)
        assert active_tlvs(base_url) == stored_dataset.lower()
