"""First install without wires: the factory ESP-Hosted firmware on the C6 is replaced through its own OTA
(BDD: rcp_sync probe/push on ESP-Hosted, br_core boot_radio, POST /device/radio/firmware on a stock C6).

The test sets up the factory state itself, over the maintenance jumper: OTBR_C6_STOCK_URL is the HTTP base URL of an
ESP-Hosted slave build directory (its flash_args and the files it names, served unchanged; the stand-in build is not in
this repository). With the C6 held in reset (no OpenThread resets meanwhile), the RCP slot ota_1 is erased (a factory C6
has nothing there) and the build is written as-is (c6_stock_restore). Then c6_jumper_ignore hides the jumper from the probe
and the push, so the wireless path runs; without it the boot probe would repair the C6 over the jumper at once.
OTBR_C6_INSTALLER and OTBR_RCP_IMAGE point to c6_installer.bin and c6_rcp.bin of the same build as the P4 firmware.

Test build only. Skips without the three variables. Each run rewrites the C6 three times (stock, installer, RCP).
"""

import os
from pathlib import Path

import pytest
import requests
from conftest import GOT_IP, wait_reachable
from test_br_boot import br_status

BOOT_TIMEOUT_S = 90
RESTORE_TIMEOUT_S = 600  # 1 MB over the ROM loader link, verified
INSTALL_TIMEOUT_S = 300  # ESP-Hosted erases its slot, the installer waits 30 s and writes, the updater boots
PUSH_TIMEOUT_S = 240
ENDPOINT = "/device/radio/firmware"


def image_from(env):
    path = os.environ.get(env)
    if not path or not Path(path).is_file():
        pytest.skip(f"{env} does not point to an image")
    return Path(path).read_bytes()


@pytest.fixture
def stock_url():
    url = os.environ.get("OTBR_C6_STOCK_URL")
    if not url:
        pytest.skip(
            "OTBR_C6_STOCK_URL (an ESP-Hosted slave build served over HTTP) is not set"
        )
    return url.rstrip("/")


def post(base_url, image, timeout):
    return requests.post(base_url + ENDPOINT, data=image, timeout=timeout)


def status(base_url):
    return requests.get(base_url + "/device/status", timeout=10).json()


def boot_address(dut):
    address = dut.expect(GOT_IP, timeout=BOOT_TIMEOUT_S).group(1).decode()
    wait_reachable(address)
    return f"http://{address}"


class TestFirstInstallThroughEspHosted:
    """rcp_sync on a factory C6: detect, refuse the RCP, install c6_installer.bin, then the RCP as usual"""

    def test_factory_c6_to_br_running_without_wires(self, dut, send, reboot, stock_url):
        installer = image_from("OTBR_C6_INSTALLER")
        rcp = image_from("OTBR_RCP_IMAGE")

        # Factory state: the C6 held (OpenThread cannot reset it), then the stand-in written as-is over the jumper.
        send("c6_hold 1", rb"c6_hold: ok")
        send("c6_jumper_ignore 1", rb"c6_jumper_ignore: ok")
        reboot()
        boot_address(dut)
        send("c6_erase ota1", rb"c6_erase: ok")
        dut.write(f"c6_stock_restore {stock_url}")
        dut.expect(rb"c6_stock_restore: ok", timeout=RESTORE_TIMEOUT_S)
        send("c6_hold 0", rb"c6_hold: ok")
        reboot()
        base_url = boot_address(dut)

        # On a factory C6 after a reboot: RADIO_FAULT "esp-hosted firmware", nothing written.
        dut.expect(rb"mode_fsm: mode RADIO_FAULT", timeout=BOOT_TIMEOUT_S)
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
