"""First install without wires: the factory ESP-Hosted firmware on the C6 is replaced through its own OTA
(BDD: rcp_sync probe/push on ESP-Hosted, br_core boot_radio, POST /device/radio/firmware on a stock C6).

Preconditions, set up from the host before the run (nothing here writes the stock firmware):
- the C6 holds an ESP-Hosted slave build flashed as-is over the jumper (its own flash_args, esptool), so it boots like a
  factory board; set OTBR_C6_STOCK=1 to confirm. Each run consumes it: the test ends with our firmware on the C6.
- OTBR_C6_INSTALLER points to c6_installer.bin and OTBR_RCP_IMAGE to c6_rcp.bin of the same build as the P4 firmware.

Test build only (console c6_jumper_ignore: the jumper stays fitted, the probe and the push ignore it). The tests skip
without the preconditions.
"""

import os
from pathlib import Path

import pytest
import requests
from conftest import GOT_IP, wait_reachable
from test_br_boot import br_status

BOOT_TIMEOUT_S = 90
INSTALL_TIMEOUT_S = 300  # ESP-Hosted erases its slot, the installer writes the base image, the updater boots
PUSH_TIMEOUT_S = 240
ENDPOINT = "/device/radio/firmware"


def image_from(env):
    path = os.environ.get(env)
    if not path or not Path(path).is_file():
        pytest.skip(f"{env} does not point to an image")
    return Path(path).read_bytes()


@pytest.fixture
def stock_c6():
    if os.environ.get("OTBR_C6_STOCK") != "1":
        pytest.skip(
            "the C6 does not hold the ESP-Hosted stand-in (set OTBR_C6_STOCK=1 once it does)"
        )


@pytest.fixture
def jumper_ignored(send):
    """From the next boot on the probe and the push treat the jumper as absent; cleared afterwards."""
    send("c6_jumper_ignore 1", rb"c6_jumper_ignore: ok")
    yield
    send("c6_jumper_ignore 0", rb"c6_jumper_ignore: ok")


def post(base_url, image, timeout):
    return requests.post(base_url + ENDPOINT, data=image, timeout=timeout)


def status(base_url):
    return requests.get(base_url + "/device/status", timeout=10).json()


class TestFirstInstallThroughEspHosted:
    """rcp_sync on a factory C6: detect, refuse the RCP, install c6_installer.bin, then the RCP as usual"""

    def test_factory_c6_to_br_running_without_wires(
        self, dut, reboot, stock_c6, jumper_ignored
    ):
        installer = image_from("OTBR_C6_INSTALLER")
        rcp = image_from("OTBR_RCP_IMAGE")
        reboot()
        address = dut.expect(GOT_IP, timeout=BOOT_TIMEOUT_S).group(1).decode()
        wait_reachable(address)
        base_url = f"http://{address}"

        # On a factory C6 after a reboot: RADIO_FAULT "esp-hosted firmware", nothing written.
        assert br_status(dut)["fault"] == "esp-hosted firmware"

        # On c6_rcp.bin uploaded to ESP-Hosted: refused, asks for c6_installer.bin, nothing sent.
        response = post(base_url, rcp, PUSH_TIMEOUT_S)
        assert response.status_code == requests.codes.bad_request, response.text
        assert "c6_installer.bin" in response.text
        assert br_status(dut)["fault"] == "esp-hosted firmware"

        # On c6_installer.bin: 200; the C6 has the updater and no RCP (RADIO_FAULT "no rcp firmware").
        response = post(base_url, installer, INSTALL_TIMEOUT_S)
        assert response.status_code == requests.codes.ok, response.text
        assert br_status(dut)["fault"] == "no rcp firmware"
        s = status(base_url)
        assert s["jumper"]["base_installed"] == s["jumper"]["base_expected"]

        # On c6_rcp.bin afterwards: the usual push, BR_RUNNING.
        response = post(base_url, rcp, PUSH_TIMEOUT_S)
        assert response.status_code == requests.codes.ok, response.text
        dut.expect(rb"mode_fsm: mode BR_RUNNING", timeout=PUSH_TIMEOUT_S)
        assert br_status(dut)["compatible"]
