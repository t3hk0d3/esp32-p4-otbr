"""REST guard (Task 14): Thread-changing requests are refused by mode (409 in SNIFFER, 503 in RADIO_FAULT).

Hardware-gated: written with Task 14 as the contract, deferred to V2 (never run in Task 14). The sniffer is started
through GET /device/sniffer.pcap, a radio fault through the board
booting with the C6 held in reset (`c6_held_in_reset`) or `rcp_kill`.
"""

import threading

import pytest
import requests
from conftest import ot, start_capture, stop_capture
from test_br_boot import br_status

CHANNEL = 15
RACE_ROUNDS = 50
BACK_TO_BR_S = 60
WRITE_OK = {200, 201, 204}
REFUSED = {
    409,
    503,
}  # 503 while the transition has switched Thread off but SNIFFER is not entered yet
DISCOVERY = "/.well-known/thread/br-rest"
GUI_FORM = {
    "networkName": "guardnet",
    "extPanId": "1111111122222222",
    "panId": "0x1234",
    "passphrase": "j01Nme",
    "networkKey": "00112233445566778899aabbccddeeff",
    "channel": 15,
    "prefix": "fd11:22::/64",
    "defaultRoute": True,
}


def dataset_body(name):
    """Dataset in the Thread REST format (what python-otbr-api sends)."""
    return {
        "networkName": name,
        "networkKey": "00112233445566778899aabbccddeeff",
        "channel": CHANNEL,
        "panId": 0x1234,
        "extPanId": "1111111122222222",
        "activeTimestamp": {"seconds": 1, "ticks": 0, "authoritative": False},
    }


def active_network_name(dut):
    """Network name of the stored active dataset, read through the OpenThread CLI ("" without a dataset)."""
    for line in ot(dut, "dataset active"):
        if line.startswith("Network Name:"):
            return line.split(":", 1)[1].strip()
    return ""


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


@pytest.fixture
def radio_fault_boot(c6_hold, reboot):
    """Reboot with the C6 held in reset: RADIO_FAULT from boot on."""
    reboot()


@pytest.fixture
def sniffing(dut, send, base_url):
    """The board in SNIFFER (a capture stream is open); back to BR_RUNNING afterwards."""
    stream = start_capture(base_url, CHANNEL)
    yield
    stop_capture(base_url, stream)


class TestRestGuard:
    """REST guard (esp_ot_br_server patch)"""

    def test_thread_dependent_endpoint_on_sniffer_returns_409(
        self, dut, base_url, sniffing
    ):
        # Thread-dependent endpoint, On SNIFFER: returns 409 (Thread REST and Web GUI, upstream error formats).
        rest = requests.put(
            base_url + "/node/dataset/active", json=dataset_body("sniffed"), timeout=10
        )
        assert rest.status_code == 409
        assert rest.json()["detail"] == "sniffer active"
        gui = requests.post(base_url + "/form_network", json=GUI_FORM, timeout=10)
        assert gui.status_code == 409
        assert gui.json()["message"] == "sniffer active"
        resource = requests.get(base_url + "/diagnostics", timeout=10)
        assert resource.status_code == 409
        assert br_status(dut)["mode"] == "SNIFFER"

    def test_thread_dependent_endpoint_on_radio_fault_returns_503(
        self, dut, radio_fault_boot, base_url
    ):
        # Thread-dependent endpoint, On RADIO_FAULT: returns 503.
        rest = requests.put(
            base_url + "/node/dataset/active", json=dataset_body("faulty"), timeout=10
        )
        assert rest.status_code == 503
        assert rest.json()["detail"] == "radio not available"
        gui = requests.post(base_url + "/form_network", json=GUI_FORM, timeout=10)
        assert gui.status_code == 503

    def test_thread_dependent_endpoint_on_br_running_behaves_as_upstream(
        self, dut, fresh_nvs, base_url
    ):
        # Thread-dependent endpoint, On BR_RUNNING: behaves as upstream (the write is applied).
        assert br_status(dut)["mode"] == "BR_RUNNING"
        response = requests.put(
            base_url + "/node/dataset/active", json=dataset_body("allowed"), timeout=10
        )
        assert response.status_code in WRITE_OK
        assert active_network_name(dut) == "allowed"

    def test_read_only_status_endpoint_answers_200_on_any_mode(
        self, dut, send, stored_dataset, base_url
    ):
        # read-only status endpoint, On any mode: returns 200. The endpoints that need no OpenThread access are the
        # discovery document and the static pages (Thread state reads answer 503 while OpenThread is switched off,
        # see PATCHES.md, REST guard classification).
        stream = None
        for mode in ("BR_RUNNING", "SNIFFER", "RADIO_FAULT"):
            if mode == "SNIFFER":
                stream = start_capture(base_url, CHANNEL)
            elif mode == "RADIO_FAULT":
                stop_capture(base_url, stream)
                send("rcp_kill", rb"rcp_kill: ESP_OK")
                # The recovery reset "works" first (the hold only keeps the C6 down), the fault comes when it stays silent.
                dut.expect(rb"br_core: radio fault: RCP did not answer", timeout=30)
            assert br_status(dut)["mode"] == mode
            for path in (DISCOVERY, "/index.html"):
                response = requests.get(base_url + path, timeout=10)
                assert response.status_code == 200, (mode, path)

    def test_guard_rejects_dataset_write_during_sniffer_transition(
        self, dut, fresh_nvs, base_url
    ):
        # mode change racing a write, On PUT /node/dataset/active issued while sniffer starts: either the write
        # completes before SNIFFER or it is refused (409, or 503 within the transition); dataset unchanged during SNIFFER (Review Focus 3).
        # The console refuses `ot` in SNIFFER, so the stored dataset is read once the board is back in BR_RUNNING.
        for round_no in range(RACE_ROUNDS):
            name = f"race{round_no:03d}"
            before = active_network_name(dut)
            result = {}

            def write(name=name, result=result):
                result["status"] = requests.put(
                    base_url + "/node/dataset/active",
                    json=dataset_body(name),
                    timeout=30,
                ).status_code

            writer = threading.Thread(target=write)
            writer.start()
            stream = start_capture(base_url, CHANNEL)
            writer.join(timeout=60)
            assert result["status"] in WRITE_OK | {409, 503}, (round_no, result)
            # Whatever happened, the sniffer now refuses every further write.
            late = requests.put(
                base_url + "/node/dataset/active", json=dataset_body("late"), timeout=10
            )
            assert late.status_code == 409, round_no
            stop_capture(base_url, stream)
            dut.expect(rb"mode_fsm: mode SNIFFER", timeout=BACK_TO_BR_S)
            dut.expect(rb"mode_fsm: mode BR_RUNNING", timeout=BACK_TO_BR_S)
            after = active_network_name(dut)
            if result["status"] in REFUSED:
                assert after == before, (round_no, before, after)
            else:
                assert after == name, (round_no, name, after)
