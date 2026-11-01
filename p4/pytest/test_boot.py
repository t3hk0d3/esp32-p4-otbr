"""Boot behaviour of the P4 firmware: Ethernet bring-up and flash storage (BDD: eth, storage)."""

import ipaddress
import subprocess
from pathlib import Path

import pexpect
import pytest
from conftest import send_until_ack

CONSOLE_TIMEOUT_S = 10
LINK_TIMEOUT_S = 60


def run_command(dut, command, pattern):
    """Send a console command and return the match for its output pattern."""
    dut.write(command)
    return dut.expect(pattern, timeout=CONSOLE_TIMEOUT_S)


class TestEth:
    """eth.eth_start"""

    def test_cable_connected_obtains_dhcpv4_address(self, dut, board_ip):
        # On cable connected: obtains a DHCPv4 address (and the host can reach it).
        address = ipaddress.ip_address(board_ip)
        assert address.version == 4 and not address.is_unspecified
        ping = subprocess.run(
            ["ping", "-c", "3", "-W", "2", board_ip], check=False, capture_output=True
        )
        assert ping.returncode == 0, ping.stdout.decode()

    def test_cable_connected_obtains_link_local_and_slaac_addresses(self, dut):
        # On cable connected: obtains an IPv6 link-local and a SLAAC address.
        link_local, slaac = dut.expect(
            [
                rb"eth: got ip6 (fe80:[0-9a-f:]+) link-local",
                rb"eth: got ip6 ([0-9a-f:]+) slaac",
            ],
            expect_all=True,
            timeout=LINK_TIMEOUT_S,
        )
        assert ipaddress.ip_address(link_local.group(1).decode()).is_link_local
        assert not ipaddress.ip_address(slaac.group(1).decode()).is_link_local

    @pytest.mark.manual_cable
    def test_cable_disconnected_at_boot_returns_ok_and_netif_comes_up_on_plug(
        self, dut
    ):
        # On cable disconnected at boot: returns ESP_OK and the netif comes up when plugged in.
        input("Unplug the Ethernet cable from the board, then press Enter...")
        dut.serial.hard_reset()
        dut.expect(rb"eth: started, waiting for link", timeout=CONSOLE_TIMEOUT_S)
        with pytest.raises(pexpect.TIMEOUT):
            dut.expect(rb"eth: got ip ", timeout=15)
        input("Plug the Ethernet cable back in, then press Enter...")
        dut.expect(rb"eth: got ip (\d+\.\d+\.\d+\.\d+)", timeout=LINK_TIMEOUT_S)


class TestStorage:
    """storage.storage_mount"""

    @pytest.fixture(autouse=True)
    def restore_slots(self, dut, write_partition):
        """These tests overwrite or erase the web slots; later tests need the flashed image back."""
        yield
        build = Path(dut.app.binary_path)
        write_partition("web_0", build / "web_0.bin")

    def mounted_slot_serves_marker(
        self, dut, boot_slot, write_partition, spiffs_image, slot
    ):
        for n in (0, 1):
            write_partition(f"web_{n}", spiffs_image({"slot.txt": f"web_{n}"}))
        write_partition(
            "ota_1", Path(dut.app.binary_path) / "p4.bin"
        )  # same app in both slots
        boot_slot(slot)
        content = run_command(
            dut, "cat /web/slot.txt", rb"cat: /web/slot.txt: (web_\d)"
        )
        return content.group(1).decode()

    def test_running_ota_0_serves_web_files_from_web_0(
        self, dut, boot_slot, write_partition, spiffs_image
    ):
        assert (
            self.mounted_slot_serves_marker(
                dut, boot_slot, write_partition, spiffs_image, 0
            )
            == "web_0"
        )

    def test_running_ota_1_serves_web_files_from_web_1(
        self, dut, boot_slot, write_partition, spiffs_image
    ):
        try:
            assert (
                self.mounted_slot_serves_marker(
                    dut, boot_slot, write_partition, spiffs_image, 1
                )
                == "web_1"
            )
        finally:
            boot_slot(0)

    def test_no_rcp_partitions_or_mounts(self, dut):
        # On boot: /rcp_0 and /rcp_1 do not exist, and the partition table has no rcp_* entry (R14).
        for mount in ("/rcp_0", "/rcp_1"):
            send_until_ack(
                dut, f"fsinfo {mount}", rf"fsinfo: {mount} not mounted".encode()
            )
        names = set(dut.app.partition_table)
        assert not {n for n in names if n.startswith("rcp_")}
        assert {
            "nvs",
            "otadata",
            "coredump",
            "ota_0",
            "ota_1",
            "web_0",
            "web_1",
        } <= names

    def keeps_unmountable_data(
        self, dut, write_partition, erase_partition, tmp_path, partition, error
    ):
        garbage = bytes([0xA5, 0x5A]) * 4096  # not erased, not a SPIFFS image
        image = tmp_path / "garbage.bin"
        image.write_bytes(garbage)
        write_partition(partition, image)
        try:
            dut.serial.hard_reset()
            dut.expect(error, timeout=LINK_TIMEOUT_S)
            dut.expect(rb"Returned from app_main", timeout=LINK_TIMEOUT_S)
            offset = dut.app.partition_table[partition]["offset"]
            assert dut.serial.dump_flash(address=offset, size=len(garbage)) == garbage
        finally:
            erase_partition(partition)

    def test_unmountable_web_slot_is_not_formatted(
        self, dut, write_partition, erase_partition, tmp_path
    ):
        self.keeps_unmountable_data(
            dut,
            write_partition,
            erase_partition,
            tmp_path,
            "web_0",
            rb"storage: .*mounting web_0 at /web failed",
        )
