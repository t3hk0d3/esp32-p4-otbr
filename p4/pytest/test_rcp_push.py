"""br_core.br_core_push_radio_firmware through POST /device/radio/firmware (BDD: br_core push).

Needs a good RCP image (env OTBR_RCP_IMAGE, e.g. c6_rcp/build/c6_rcp.bin); the tests skip without it. A 404 from the
endpoint fails them.
"""

import os
import time
from pathlib import Path

import pytest
import requests
from conftest import GOT_IP, start_capture, stop_capture, wait_reachable
from test_br_boot import br_status

BOOT_TIMEOUT_S = 60
PUSH_TIMEOUT_S = 240
ENDPOINT = "/device/radio/firmware"


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


@pytest.fixture
def good_image():
    path = os.environ.get("OTBR_RCP_IMAGE")
    if not path or not Path(path).is_file():
        pytest.skip("OTBR_RCP_IMAGE does not point to an RCP image")
    return Path(path).read_bytes()


def post_image(base_url, image):
    response = requests.post(base_url + ENDPOINT, data=image, timeout=PUSH_TIMEOUT_S)
    assert response.status_code != requests.codes.not_found, (
        "POST /device/radio/firmware does not exist"
    )
    return response


class TestPush:
    """br_core.br_core_push_radio_firmware"""

    def test_healthy_push_restarts_openthread_and_reopens_the_thread_rest_api(
        self, dut, base_url, good_image
    ):
        # On BR_RUNNING and a healthy push: stops OpenThread, pushes, resets the C6 and restarts OpenThread;
        # rcp_installed shows the pushed version; GET /node/state answers non-503 afterwards.
        response = post_image(base_url, good_image)
        assert response.status_code == requests.codes.ok, response.text
        dut.expect(rb"mode_fsm: mode BR_RUNNING", timeout=PUSH_TIMEOUT_S)
        status = br_status(dut)
        assert status["mode"] == "BR_RUNNING"
        assert status["installed"]
        assert status["compatible"]
        state = requests.get(base_url + "/node/state", timeout=10)
        assert state.status_code != requests.codes.service_unavailable

    def test_push_in_sniffer_is_refused_with_409(self, dut, base_url, good_image):
        # On SNIFFER: returns ESP_ERR_INVALID_STATE (409) and starts nothing.
        stream = start_capture(base_url, 15)
        try:
            response = post_image(base_url, good_image)
            assert response.status_code == requests.codes.conflict
            assert br_status(dut)["mode"] == "SNIFFER"
        finally:
            stop_capture(base_url, stream)

    def test_failing_push_restarts_openthread_with_the_previous_firmware(
        self, dut, base_url, good_image
    ):
        # On push failure: restarts OpenThread with the previous firmware and returns the push error.
        before = br_status(dut)["installed"]
        response = post_image(base_url, good_image[: len(good_image) // 2])
        assert response.status_code != requests.codes.ok
        dut.expect(rb"mode_fsm: mode BR_RUNNING", timeout=PUSH_TIMEOUT_S)
        status = br_status(dut)
        assert status["mode"] == "BR_RUNNING"
        assert status["installed"] == before
        state = requests.get(base_url + "/node/state", timeout=10)
        assert state.status_code != requests.codes.service_unavailable


class TestPushFromIncompatibleRcp:
    """br_core.br_core_push_radio_firmware, fault "rcp incompatible" found by the C6's boot steps"""

    def test_push_recovers_the_board_to_br_running_without_a_reboot(
        self, dut, send, reboot, good_image
    ):
        # On RADIO_FAULT "rcp incompatible" from the boot steps (c6_hold: the C6 stays in reset, as an RCP that never
        # answers; the C6 is listed unavailable): the push lists the C6 again and starts OpenThread on it in place:
        # BR_RUNNING, rcp compatible, no reboot.
        # The push after an abandoned OpenThread start (a reboot follows it) is not reachable from a test hook since the
        # RCP probe runs before OpenThread's init: c6_hold faults in the boot steps, before any init. It needs an RCP
        # that answers the probe and then fails esp_openthread_init (another OpenThread build's RCP).
        send("c6_hold 1", rb"c6_hold: ok")
        reboot()
        address = dut.expect(GOT_IP, timeout=BOOT_TIMEOUT_S).group(1).decode()
        wait_reachable(address)
        base_url = f"http://{address}"
        deadline = time.monotonic() + BOOT_TIMEOUT_S
        while (status := br_status(dut))["mode"] != "RADIO_FAULT":
            assert time.monotonic() < deadline, f"no RADIO_FAULT: {status}"
            time.sleep(1)
        assert status["fault"] == "rcp incompatible"
        assert not status["compatible"]
        uptime = requests.get(f"{base_url}/device/diag", timeout=10).json()["uptime_s"]
        send("c6_hold 0", rb"c6_hold: ok")  # read by the push's own boot steps
        response = post_image(base_url, good_image)
        assert response.status_code == requests.codes.ok, response.text
        dut.expect(rb"mode_fsm: mode BR_RUNNING", timeout=PUSH_TIMEOUT_S)
        status = br_status(dut)
        assert status["mode"] == "BR_RUNNING"
        assert status["compatible"]
        assert (
            requests.get(f"{base_url}/device/diag", timeout=10).json()["uptime_s"]
            > uptime
        ), "the board rebooted"
