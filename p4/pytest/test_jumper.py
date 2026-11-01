"""rcp_sync maintenance jumper and device_api recover/log (BDD: rcp_sync jumper, device_api radio/recover and radio/log).

Written with Task 16, deferred to V3. `jumper` tests need the jumper fitted (the default rig); `no_jumper` tests need it
removed and are excluded unless selected: pytest -m no_jumper. Test build only (console command c6_erase).
"""

# ruff: noqa: F811  (fixtures re-exported from test_device_api are used as parameters)
import re
import time

import pexpect
import pytest
import requests
from conftest import send_until_ack, start_capture, stop_capture
from rcp_image import retag
from test_br_boot import br_status
from test_device_api import (
    rcp_image,  # noqa: F401  (fixture re-export)
    wait_for_mode,
)

RECOVERY_TIMEOUT_S = 180
PUSH_TIMEOUT_S = 240
BOOT_TIMEOUT_S = 90
ROM_BANNER_LINE = rb"c6: .*ESP-ROM:esp32c6"
RECOVERED = rb"rcp_sync: ROM recovery done"
ERASED = rb"c6_erase: ok"


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


def status(base_url):
    return requests.get(base_url + "/device/status", timeout=10).json()


def post_recover(base_url):
    return requests.post(base_url + "/device/radio/recover", timeout=RECOVERY_TIMEOUT_S)


def erase(dut, region):
    send_until_ack(dut, f"c6_erase {region}", ERASED)


def wait_status(base_url, predicate, timeout=BOOT_TIMEOUT_S):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            current = status(base_url)
            if predicate(current):
                return current
        except requests.RequestException:
            pass
        time.sleep(2)
    pytest.fail("status never reached the expected state")


@pytest.mark.jumper
class TestJumperPresent:
    """rcp_sync jumper detection and ROM recovery; device_api with the jumper fitted"""

    def test_detection_reports_the_jumper_present(self, dut, base_url):
        # On C6 ROM boot banner received on JUMPER_RX during reset: reports jumper present.
        assert status(base_url)["jumper"]["present"] is True

    def test_log_endpoint_returns_the_c6_console_text(self, dut, base_url):
        # GET /device/radio/log on jumper present: returns the captured C6 console text.
        response = requests.get(base_url + "/device/radio/log", timeout=10)
        assert response.status_code == requests.codes.ok
        assert response.headers["Content-Type"].startswith("text/plain")
        assert "ESP-ROM" in response.text

    def test_c6_console_lines_are_forwarded_to_the_p4_log_with_a_prefix(
        self, dut, base_url
    ):
        # On C6 console output while not in ROM recovery: forwards complete lines to the P4 log prefixed "c6:".
        dut.write("rcp_glitch")
        dut.expect(ROM_BANNER_LINE, timeout=30)

    def test_recovery_after_erasing_the_test_partition(self, dut, base_url):
        # On jumper present and updater silent after retries: ROM recovery on the next boot; the C6 then answers HELLO
        # with the embedded base version and the BR reports no RCP or the surviving one.
        erase(dut, "test")
        requests.post(base_url + "/device/reboot", timeout=10)
        dut.expect(RECOVERED, timeout=RECOVERY_TIMEOUT_S)
        current = wait_status(base_url, lambda s: s["jumper"]["base_installed"])
        assert current["jumper"]["base_installed"] == current["jumper"]["base_expected"]

    def test_recovery_after_erasing_the_bootloader(self, dut, base_url):
        # Same, with the C6 bootloader gone (the C6 does not boot at all).
        erase(dut, "bootloader")
        requests.post(base_url + "/device/reboot", timeout=10)
        dut.expect(RECOVERED, timeout=RECOVERY_TIMEOUT_S)
        current = wait_status(base_url, lambda s: s["jumper"]["base_installed"])
        assert current["jumper"]["base_installed"] == current["jumper"]["base_expected"]

    def test_recovery_writes_no_rcp_firmware_and_erases_otadata(self, dut, base_url):
        # On ROM recovery: writes no RCP firmware; the boot afterwards reports "no rcp firmware" when no slot survived,
        # and POST /device/radio/firmware brings the BR up (needs OTBR_RCP_IMAGE; see test_rcp_push).
        erase(dut, "otadata")
        response = post_recover(base_url)
        assert response.status_code == requests.codes.ok, response.text
        dut.expect(RECOVERED, timeout=RECOVERY_TIMEOUT_S)
        current = br_status(dut)
        assert current["mode"] in {"BR_RUNNING", "RADIO_FAULT"}
        if current["mode"] == "RADIO_FAULT":
            assert current["fault"] == "no rcp firmware"
        else:
            assert current["fault"] == ""

    def test_recover_endpoint_restarts_openthread(self, dut, base_url):
        # POST /device/radio/recover on BR_RUNNING or RADIO_FAULT: recovers the base image and restarts OpenThread.
        response = post_recover(base_url)
        assert response.status_code == requests.codes.ok, response.text
        assert status(base_url)["mode"] in {"BR_RUNNING", "RADIO_FAULT"}

    def test_recover_in_sniffer_is_refused_with_409(self, dut, base_url):
        # POST /device/radio/recover on SNIFFER: returns 409.
        stream = start_capture(base_url, 15)
        try:
            assert post_recover(base_url).status_code == requests.codes.conflict
        finally:
            stop_capture(base_url, stream)

    @pytest.mark.parametrize("pushes", [1, 2])
    def test_rcp_survives_a_recovery_that_blanks_otadata_whichever_slot_holds_it(
        self, dut, base_url, rcp_image, pushes
    ):
        # On ROM recovery with RCP firmware in either slot (a push after a fresh install lands in ota_1, a second one
        # in ota_0): otadata is blank afterwards and the C6 bootloader boots the first valid slot; the updater must
        # report that slot's RCP, so OpenThread starts instead of "no rcp firmware".
        try:
            for index in range(pushes):
                image = retag(rcp_image, f"2030010{index + 1}T000000Z-000000{index}")
                response = requests.post(
                    base_url + "/device/radio/firmware",
                    data=image,
                    timeout=PUSH_TIMEOUT_S,
                )
                assert response.status_code == requests.codes.ok, response.text
                wait_for_mode(base_url, "BR_RUNNING", PUSH_TIMEOUT_S)
            erase(dut, "otadata")
            assert post_recover(base_url).status_code == requests.codes.ok
            dut.expect(RECOVERED, timeout=RECOVERY_TIMEOUT_S)
            wait_for_mode(base_url, "BR_RUNNING", BOOT_TIMEOUT_S)
            assert status(base_url)["rcp"]["installed"]
        finally:
            requests.post(
                base_url + "/device/radio/firmware",
                data=rcp_image,
                timeout=PUSH_TIMEOUT_S,
            )
            wait_for_mode(base_url, "BR_RUNNING", PUSH_TIMEOUT_S)

    def test_unchanged_base_version_across_p4_builds_triggers_no_recovery(
        self, dut, base_url
    ):
        # On updater_version equal to the embedded base version (content-addressed, independent of the P4 build):
        # a normal boot runs no ROM recovery. The base version only changes with the base image sources.
        current = status(base_url)["jumper"]
        assert current["base_installed"] == current["base_expected"]
        assert re.fullmatch(r"base-[0-9a-f]{8}", current["base_expected"])
        with pytest.raises(pexpect.TIMEOUT):
            dut.expect(RECOVERED, timeout=15)


@pytest.mark.no_jumper
class TestJumperAbsent:
    """With the maintenance jumper removed (re-wire first, run with -m no_jumper)"""

    def test_detection_reports_the_jumper_absent(self, dut, base_url):
        # On no bytes received during reset: reports jumper absent.
        assert status(base_url)["jumper"]["present"] is False

    def test_recover_returns_412(self, dut, base_url):
        # POST /device/radio/recover on jumper absent: returns 412.
        assert post_recover(base_url).status_code == requests.codes.precondition_failed

    def test_log_returns_412(self, dut, base_url):
        # GET /device/radio/log on jumper absent: returns 412.
        response = requests.get(base_url + "/device/radio/log", timeout=10)
        assert response.status_code == requests.codes.precondition_failed

    def test_erase_command_refuses_to_drive_the_pins(self, dut):
        # On jumper absent: ROM recovery is not attempted, JUMPER_TX and JUMPER_BOOT are never driven.
        dut.write("c6_erase test")
        dut.expect(rb"c6_erase: ESP_ERR_NOT_SUPPORTED", timeout=10)
