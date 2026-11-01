"""App validity: the image is marked valid after 60 s with the HTTP server listening (BDD: br_core app validity).

The tests boot a pending-verify image in ota_1. A test build confirms every image at start-up so that tests can
reboot inside either slot; `keep_pending` (set before `boot_slot`) makes exactly the next boot skip that, so the
production path in validity.c is what confirms the image. otatool.py is not used: the same app is written to
ota_1 through the test session's esptool and selected with esp_ota_set_boot_partition, as otatool's slot switch does.
"""

from pathlib import Path

import pytest
import requests
from conftest import CONSOLE_READY

CONSOLE_TIMEOUT_S = 10
VALID_TIMEOUT_S = 100
BOOT_TIMEOUT_S = 60
OTA_STATE = rb"ota_state: running=(\d) state=(\w+)"


def ota_state(dut):
    dut.write("ota_state")
    match = dut.expect(OTA_STATE, timeout=CONSOLE_TIMEOUT_S)
    return int(match.group(1)), match.group(2).decode()


@pytest.fixture
def pending_image(dut, boot_slot, write_partition, send):
    """Boot ota_1 with its image pending-verify; return to ota_0 afterwards."""
    write_partition("ota_1", Path(dut.app.binary_path) / "p4.bin")
    send("keep_pending", rb"keep_pending: set")
    boot_slot(1)
    yield
    boot_slot(0)
    send(
        "keep_pending 0", rb"keep_pending: cleared"
    )  # in case the boot never consumed it


class TestValidity:
    """br_core.validity_start"""

    def test_pending_image_with_listening_server_is_marked_valid_after_60_s(
        self, dut, pending_image
    ):
        # On pending-verify image, HTTP server listening, 60 s uptime: marks the app valid.
        assert ota_state(dut) == (1, "PENDING_VERIFY")
        dut.expect(rb"validity: app marked valid", timeout=VALID_TIMEOUT_S)
        assert ota_state(dut) == (1, "VALID")

    def test_pending_image_in_radio_fault_is_still_marked_valid_after_60_s(
        self, dut, c6_hold, pending_image
    ):
        # On pending-verify image in RADIO_FAULT: still marks the app valid after 60 s.
        dut.expect(rb"mode_fsm: mode RADIO_FAULT", timeout=BOOT_TIMEOUT_S)
        assert ota_state(dut) == (1, "PENDING_VERIFY")
        dut.expect(rb"validity: app marked valid", timeout=VALID_TIMEOUT_S)

    def test_reset_before_60_s_boots_the_previous_slot(
        self, dut, boot_slot, write_partition, send
    ):
        # On reset before 60 s: the bootloader boots the previous slot.
        write_partition("ota_1", Path(dut.app.binary_path) / "p4.bin")
        send("keep_pending", rb"keep_pending: set")
        boot_slot(1)
        assert ota_state(dut) == (1, "PENDING_VERIFY")
        dut.write("crash")
        dut.expect(rb"crash: aborting", timeout=CONSOLE_TIMEOUT_S)
        dut.expect(CONSOLE_READY, timeout=BOOT_TIMEOUT_S)
        assert ota_state(dut)[0] == 0

    def test_update_while_the_running_image_is_pending_is_refused_with_409_before_anything_is_erased(
        self, dut, pending_image, board_ip
    ):
        # On a pending-verify running image (first 60 s after an update): an update is refused with 409 "busy" and the
        # retry detail, and web_M (the rollback target's web slot) is not erased.
        assert ota_state(dut) == (1, "PENDING_VERIFY")
        dut.write("ota_probe")
        dut.expect(rb"ota_probe: .*\(0x7b01\)", timeout=CONSOLE_TIMEOUT_S)
        response = requests.post(
            f"http://{board_ip}/device/ota", data=b"x" * 64, timeout=30
        )
        assert response.status_code == requests.codes.conflict
        body = response.json()
        assert body["error"] == "busy"
        assert (
            body["detail"]
            == "the running update is not confirmed yet; retry in up to 60 s"
        )
        dut.write("ota_probe")
        dut.expect(rb"ota_probe: .*\(0x7b01\)", timeout=CONSOLE_TIMEOUT_S)
        dut.expect(rb"validity: app marked valid", timeout=VALID_TIMEOUT_S)
        dut.write("ota_probe")
        dut.expect(rb"ota_probe: ESP_OK", timeout=CONSOLE_TIMEOUT_S)


@pytest.mark.manual_cable
class TestValidityWithoutLink:
    """br_core.validity_start, Ethernet link absent"""

    def test_pending_image_without_ethernet_link_is_marked_valid_after_60_s(
        self, dut, boot_slot, write_partition, send
    ):
        # On pending-verify image without Ethernet link: still marks the app valid after 60 s.
        write_partition("ota_1", Path(dut.app.binary_path) / "p4.bin")
        send("keep_pending", rb"keep_pending: set")
        input("Unplug the Ethernet cable from the board, then press Enter...")
        try:
            boot_slot(1)
            assert ota_state(dut) == (1, "PENDING_VERIFY")
            dut.expect(rb"validity: app marked valid", timeout=VALID_TIMEOUT_S)
        finally:
            input("Plug the Ethernet cable back in, then press Enter...")
            boot_slot(0)
