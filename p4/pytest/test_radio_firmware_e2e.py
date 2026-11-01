"""Radio firmware upload end to end (spec 5.6.1; BDD: Task 21, radio firmware upload end-to-end).

Uses the real C6 over the SPI link, the maintenance jumper and the Thread peer:
  - a C6 reset injected mid-transfer (test console command `c6_inject_reset_at`),
  - a C6 whose RCP slots were erased (test console command `c6_erase ota0` and `ota1`),
  - images of an older and a newer version (c6_rcp.bin re-stamped by rcp_image.retag, checksum and SHA-256 fixed).
Every test that changes the installed firmware puts the build's own c6_rcp.bin back afterwards.
"""

# ruff: noqa: F811  (fixtures re-exported from test_device_api are used as parameters)
import subprocess

import pytest
import requests
from conftest import GOT_IP, wait_ot_state, wait_reachable
from rcp_image import retag, version_of
from test_device_api import (
    RCP_OFFSET_MID,
    assert_error_body,
    rcp_image,  # noqa: F401  (fixture re-export)
    status,
    wait_for_mode,
)

BOOT_TIMEOUT_S = 60
PUSH_TIMEOUT_S = 240
ERASE_TIMEOUT_S = 240
LEADER_TIMEOUT_S = 120
ATTACH_TIMEOUT_S = 120
OLDER = "20200101T000000Z-0000000"
NEWER = "29990101T000000Z-fffffff"
NEW_VERSION = "20300101T000000Z-1234567"
ENDPOINT = "/device/radio/firmware"


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


def upload(base_url, image):
    return requests.post(base_url + ENDPOINT, data=image, timeout=PUSH_TIMEOUT_S)


@pytest.fixture
def running_br(dut, stored_dataset, base_url, rcp_image):
    """Border router with the stored dataset, leader; the build's own firmware is put back afterwards."""
    wait_ot_state(dut, {"leader"}, LEADER_TIMEOUT_S)
    yield base_url
    current = status(base_url)
    if current["rcp"]["installed"] != version_of(rcp_image):
        assert upload(base_url, rcp_image).status_code == requests.codes.ok
        wait_for_mode(base_url, "BR_RUNNING", PUSH_TIMEOUT_S)


@pytest.fixture
def c6_without_rcp(dut, running_br, reboot, rcp_image):
    """Erase the image start of both RCP slots of the C6 over the jumper and reboot: the C6 boots its updater only."""
    for region in ("ota0", "ota1"):
        dut.write(f"c6_erase {region}")
        dut.expect(rb"c6_erase: ok", timeout=ERASE_TIMEOUT_S)
    reboot()
    address = dut.expect(GOT_IP, timeout=BOOT_TIMEOUT_S).group(1).decode()
    wait_reachable(address)
    url = f"http://{address}"
    wait_for_mode(url, "RADIO_FAULT", BOOT_TIMEOUT_S)
    yield url
    if status(url)["mode"] == "RADIO_FAULT":
        assert upload(url, rcp_image).status_code == requests.codes.ok
        wait_for_mode(url, "BR_RUNNING", PUSH_TIMEOUT_S)


class TestRetaggedImage:
    """rcp_image.retag (test helper)"""

    def test_retagged_image_is_valid_for_esptool_and_carries_the_version(
        self, rcp_image, tmp_path
    ):
        path = tmp_path / "newer.bin"
        path.write_bytes(retag(rcp_image, NEWER))
        info = subprocess.run(
            ["esptool", "image-info", str(path)],
            capture_output=True,
            text=True,
            check=True,
        ).stdout
        assert f"App version: {NEWER}" in info
        assert "Checksum:" in info
        assert "(invalid)" not in info
        assert version_of(retag(rcp_image, NEWER)) == NEWER


class TestResetInjectedMidTransfer:
    """POST /device/radio/firmware, C6 reset injected mid-transfer"""

    def test_upload_fails_cleanly_with_a_json_error(
        self, dut, send, running_br, rcp_image
    ):
        # On C6 reset injected mid-transfer: the upload fails cleanly with a JSON error.
        send(f"c6_inject_reset_at {RCP_OFFSET_MID}", rb"c6_inject_reset_at: ok")
        response = upload(running_br, rcp_image)
        assert response.status_code >= requests.codes.bad_request
        assert_error_body(response)

    def test_previous_firmware_still_boots_and_the_border_router_runs(
        self, dut, send, running_br, rcp_image
    ):
        # On C6 reset injected mid-transfer: the previous firmware still boots and the BR is running.
        before = status(running_br)["rcp"]["installed"]
        send(f"c6_inject_reset_at {RCP_OFFSET_MID}", rb"c6_inject_reset_at: ok")
        upload(running_br, retag(rcp_image, NEW_VERSION))
        wait_for_mode(running_br, "BR_RUNNING", PUSH_TIMEOUT_S)
        wait_ot_state(dut, {"leader"}, LEADER_TIMEOUT_S)
        current = status(running_br)
        assert current["rcp"]["installed"] == before
        assert current["rcp"]["compatible"]

    def test_repeated_upload_succeeds_and_status_shows_the_new_version(
        self, dut, send, running_br, rcp_image
    ):
        # On C6 reset injected mid-transfer: a repeated upload succeeds, /device/status rcp.installed shows the version.
        image = retag(rcp_image, NEW_VERSION)
        send(f"c6_inject_reset_at {RCP_OFFSET_MID}", rb"c6_inject_reset_at: ok")
        assert upload(running_br, image).status_code >= requests.codes.bad_request
        wait_for_mode(running_br, "BR_RUNNING", PUSH_TIMEOUT_S)
        response = upload(running_br, image)
        assert response.status_code == requests.codes.ok, response.text
        wait_for_mode(running_br, "BR_RUNNING", PUSH_TIMEOUT_S)
        wait_ot_state(dut, {"leader"}, LEADER_TIMEOUT_S)
        assert status(running_br)["rcp"]["installed"] == NEW_VERSION


class TestC6WithoutRcpFirmware:
    """POST /device/radio/firmware, C6 without RCP firmware"""

    def test_status_shows_fault_reason_no_rcp_firmware(self, c6_without_rcp):
        # On a C6 without RCP firmware: status shows fault_reason "no rcp firmware".
        current = status(c6_without_rcp)
        assert current["mode"] == "RADIO_FAULT"
        assert current["fault_reason"] == "no rcp firmware"

    def test_uploading_c6_rcp_brings_the_border_router_to_leader_with_the_peer(
        self, dut, c6_without_rcp, rcp_image, peer, stored_dataset
    ):
        # On a C6 without RCP firmware: uploading c6_rcp.bin brings the BR to leader, the Thread peer attaches.
        response = upload(c6_without_rcp, rcp_image)
        assert response.status_code == requests.codes.ok, response.text
        wait_for_mode(c6_without_rcp, "BR_RUNNING", PUSH_TIMEOUT_S)
        wait_ot_state(dut, {"leader"}, LEADER_TIMEOUT_S)
        peer.command(f"dataset set active {stored_dataset}")
        peer.command("ifconfig up")
        peer.command("thread start")
        peer.wait_state({"child", "router"}, ATTACH_TIMEOUT_S)
        assert status(c6_without_rcp)["rcp"]["installed"] == version_of(rcp_image)


class TestFirmwareOfAnotherVersion:
    """POST /device/radio/firmware, image of a different version"""

    @pytest.mark.parametrize("direction, version", [("older", OLDER), ("newer", NEWER)])
    def test_upload_succeeds_and_the_border_router_runs_with_it(
        self, dut, running_br, rcp_image, direction, version
    ):
        # On firmware of a different version, older or newer: the upload succeeds in both directions.
        response = upload(running_br, retag(rcp_image, version))
        assert response.status_code == requests.codes.ok, response.text
        assert response.json()["installed"] == version
        wait_for_mode(running_br, "BR_RUNNING", PUSH_TIMEOUT_S)
        wait_ot_state(dut, {"leader"}, LEADER_TIMEOUT_S)
        current = status(running_br)
        assert current["rcp"]["installed"] == version
        assert current["rcp"]["compatible"]
