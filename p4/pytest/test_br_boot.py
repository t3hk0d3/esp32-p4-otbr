"""br_core boot sequence: OpenThread over the SPI RCP, border router start, DHCPv6-PD, mDNS (BDD: br_core boot)."""

import ipaddress
import subprocess
import time
from pathlib import Path

import pexpect
import pytest
import requests
from conftest import GOT_IP, ot, wait_ot_state

BOOT_TIMEOUT_S = 60
CONSOLE_TIMEOUT_S = 10
LEADER_TIMEOUT_S = 90
ATTACH_TIMEOUT_S = 120
PD_DISABLED = 0
BR_STATUS = (
    rb"br_status: mode=(\w+) installed=(\S*) known=(\d) compatible=(\d) dataset=(\d) channel=(\d+) pd=(-?\d+)"
    rb" prefix=(\S*) fault=([^\r\n]*)"
)
WEB_GUI_GET_PATHS = (
    "/get_properties",
    "/available_network",
    "/node_information",
    "/topology",
    "/ipaddr",
    "/diagnostics",
)
WEB_GUI_POST_PATHS = (
    "/join_network",
    "/form_network",
    "/add_prefix",
    "/delete_prefix",
    "/commission",
    "/ping",
    "/add_ipaddr",
    "/delete_ipaddr",
)
HOSTNAME = rb"br_core: mDNS hostname (esp-otbr-[0-9a-f]{6})\.local"
MDNS_NETWORK = "otbr-p0"
MDNS_IMAGE = "python:3.14-alpine"
MDNS_QUERY = Path(__file__).resolve().parent / "mdns_query.py"


def br_status(dut):
    """Parsed output of the console command `br_status`."""
    dut.write("br_status")
    match = dut.expect(BR_STATUS, timeout=CONSOLE_TIMEOUT_S)
    mode, installed, known, compatible, dataset, channel, pd, prefix, fault = (
        group.decode() for group in match.groups()
    )
    return {
        "mode": mode,
        "installed": installed,
        "known": known == "1",
        "compatible": compatible == "1",
        "dataset": dataset == "1",
        "channel": int(channel),
        "pd": int(pd),
        "prefix": prefix,
        "fault": fault,
    }


def board_url(dut):
    """Base URL of the board's HTTP server, from its DHCPv4 address."""
    return "http://" + dut.expect(GOT_IP, timeout=BOOT_TIMEOUT_S).group(1).decode()


class TestBoot:
    """br_core.boot, with an active dataset stored"""

    def test_stored_dataset_starts_thread_with_the_stored_dataset(
        self, dut, stored_dataset, peer
    ):
        # On stored active dataset and Ethernet address: starts Thread with the stored dataset.
        dut.expect(
            rb"br_core: Thread started with the stored dataset", timeout=BOOT_TIMEOUT_S
        )
        wait_ot_state(dut, {"leader"}, LEADER_TIMEOUT_S)
        assert ot(dut, "dataset active -x")[-1] == stored_dataset
        # The peer joins the network by the same dataset and reaches the board over the mesh.
        mesh_local = ot(dut, "ipaddr mleid")[-1]
        peer.command(f"dataset set active {stored_dataset}")
        peer.command("ifconfig up")
        peer.command("thread start")
        peer.wait_state({"child", "router", "leader"}, ATTACH_TIMEOUT_S)
        reply = peer.command(f"ping {mesh_local}", timeout=20)
        assert any("bytes from" in line for line in reply), reply

    def test_stored_dataset_enables_dhcpv6_pd(self, dut, stored_dataset):
        # On stored active dataset and Ethernet address: enables DHCPv6-PD.
        dut.expect(rb"br_core: DHCPv6-PD enabled", timeout=BOOT_TIMEOUT_S)
        dut.expect(rb"mode_fsm: mode BR_RUNNING", timeout=BOOT_TIMEOUT_S)
        assert br_status(dut)["pd"] != PD_DISABLED

    def test_stored_dataset_enters_br_running(self, dut, stored_dataset):
        # On stored active dataset and Ethernet address: enters BR_RUNNING.
        dut.expect(rb"mode_fsm: mode BR_RUNNING", timeout=BOOT_TIMEOUT_S)
        status = br_status(dut)
        assert status["mode"] == "BR_RUNNING"
        assert status["dataset"]

    def test_probe_version_and_accepted_rcp_are_reported(self, dut, stored_dataset):
        # On probe returning a version and OpenThread accepting the RCP:
        # rcp_installed equals the version and rcp_compatible is true.
        version = (
            dut.expect(
                rb"br_core: installed RCP firmware (\S+)", timeout=BOOT_TIMEOUT_S
            )
            .group(1)
            .decode()
        )
        dut.expect(rb"mode_fsm: mode BR_RUNNING", timeout=BOOT_TIMEOUT_S)
        status = br_status(dut)
        assert status["installed"] == version
        assert status["known"]
        assert status["compatible"]


class TestBootHttpRegistration:
    """br_core.boot, HTTP handler registration"""

    def test_all_handlers_are_registered_before_the_catch_all(self, dut):
        # On boot: no handler registration fails for lack of slots, /device/* and the catch-all static UI both answer.
        dut.expect(rb"mode_fsm: mode BR_RUNNING", timeout=BOOT_TIMEOUT_S)
        log = dut.pexpect_proc.before.decode(errors="replace")
        url = board_url(dut)
        assert "no slots left" not in log
        assert "NOT available" not in log
        assert requests.get(f"{url}/device/status", timeout=10).status_code == 200
        assert requests.get(f"{url}/", timeout=10).status_code == 200


class TestBootWithoutDataset:
    """br_core.boot, with no active dataset stored"""

    def test_no_dataset_does_not_form_a_network(self, dut, fresh_nvs):
        # On no stored dataset: does not form a network.
        # "got ip" is matched in the same expect: it may come before or after the border router log line.
        _, ip = dut.expect(
            [rb"br_core: no stored dataset, Thread stays idle", GOT_IP],
            expect_all=True,
            timeout=BOOT_TIMEOUT_S,
        )
        assert ot(dut, "state") == ["disabled"]
        response = requests.get(f"http://{ip.group(1).decode()}/node/state", timeout=10)
        assert response.status_code == 200
        assert "disabled" in response.text

    def test_no_dataset_enters_br_running_idle(self, dut, fresh_nvs):
        # On no stored dataset: enters BR_RUNNING (idle).
        dut.expect(rb"mode_fsm: mode BR_RUNNING", timeout=BOOT_TIMEOUT_S)
        status = br_status(dut)
        assert status["mode"] == "BR_RUNNING"
        assert not status["dataset"]


@pytest.fixture
def c6_held_in_reset(c6_hold, reboot):
    """Reboot with the C6 held in reset."""
    reboot()


@pytest.fixture
def empty_rcp_slot(probe_override, reboot):
    """Reboot with the probe forced to report an empty C6 boot slot."""
    probe_override("empty")
    reboot()


@pytest.fixture
def failing_probe(probe_override, reboot):
    """Reboot with the probe forced to fail."""
    probe_override("fail")
    reboot()


class TestBootWithoutRcpFirmware:
    """br_core.boot, probe reporting an empty installed version"""

    def test_boot_without_rcp_firmware_enters_fault(self, dut, empty_rcp_slot):
        # On probe reporting an empty installed version: does not start OpenThread and enters RADIO_FAULT
        # with fault_reason "no rcp firmware".
        dut.expect(rb"mode_fsm: mode RADIO_FAULT", timeout=BOOT_TIMEOUT_S)
        with pytest.raises(pexpect.TIMEOUT):
            dut.expect(rb"br_core: OpenThread initialised", timeout=20)
        status = br_status(dut)
        assert status["mode"] == "RADIO_FAULT"
        assert status["fault"] == "no rcp firmware"
        assert status["installed"] == ""

    def test_boot_without_rcp_firmware_still_starts_http_server(
        self, dut, empty_rcp_slot
    ):
        # On probe reporting an empty installed version: the HTTP server is still started.
        _, _, ip = dut.expect(
            [rb"mode_fsm: mode RADIO_FAULT", rb"obtr_web: http://esp-otbr-", GOT_IP],
            expect_all=True,
            timeout=BOOT_TIMEOUT_S,
        )
        response = requests.get(f"http://{ip.group(1).decode()}/index.html", timeout=10)
        assert response.status_code == 200


class TestBootWithFailingProbe:
    """br_core.boot, probe failing but OpenThread accepting the RCP"""

    def test_failing_probe_still_enters_br_running(self, dut, failing_probe):
        # On probe failing but OpenThread accepting the RCP: enters BR_RUNNING, rcp_installed_known false.
        dut.expect(rb"mode_fsm: mode BR_RUNNING", timeout=BOOT_TIMEOUT_S)
        status = br_status(dut)
        assert status["mode"] == "BR_RUNNING"
        assert not status["known"]
        assert status["compatible"]


class TestBootNeverPushes:
    """br_core.boot, on any boot"""

    @pytest.mark.parametrize("forced", [None, "empty", "fail"])
    def test_boot_never_pushes_firmware(self, dut, probe_override, reboot, forced):
        # On any boot: sends no BEGIN to the C6 (the first fault or BR_RUNNING line ends the boot).
        if forced:
            probe_override(forced)
        reboot()
        end = dut.expect(
            rb"rcp_sync: BEGIN|mode_fsm: mode (?:BR_RUNNING|RADIO_FAULT)",
            timeout=BOOT_TIMEOUT_S,
        )
        assert b"BEGIN" not in end.group(0)


class TestBootRadioFault:
    """br_core.boot, OpenThread failing to initialise the RCP"""

    def test_failing_rcp_initialisation_enters_radio_fault(self, dut, c6_held_in_reset):
        # On OpenThread failing to initialise or accept the RCP: enters RADIO_FAULT with fault_reason
        # "rcp incompatible" and rcp_compatible false.
        dut.expect(rb"mode_fsm: mode RADIO_FAULT", timeout=BOOT_TIMEOUT_S)
        status = br_status(dut)
        assert status["mode"] == "RADIO_FAULT"
        assert status["fault"] == "rcp incompatible"
        assert not status["compatible"]

    def test_ot_console_command_without_openthread_answers_error_13(
        self, dut, c6_held_in_reset
    ):
        # Test console `ot` while OpenThread does not run: answers Error 13 instead of crashing on OpenThread's
        # missing task queue, and the console keeps working.
        dut.expect(rb"mode_fsm: mode RADIO_FAULT", timeout=BOOT_TIMEOUT_S)
        dut.write("ot state")
        dut.expect(
            rb"Error 13: InvalidState \(OpenThread not running\)",
            timeout=CONSOLE_TIMEOUT_S,
        )
        assert br_status(dut)["mode"] == "RADIO_FAULT"

    def test_radio_fault_still_starts_http_server(self, dut, c6_held_in_reset):
        # On OpenThread failing to initialise the RCP: the HTTP server is still started.
        # The /device/status endpoint comes with Task 15; until then the server's start log and a
        # static file stand in for it. The server starts before the radio sync, so the log order varies.
        _, _, ip = dut.expect(
            [rb"mode_fsm: mode RADIO_FAULT", rb"obtr_web: http://esp-otbr-", GOT_IP],
            expect_all=True,
            timeout=BOOT_TIMEOUT_S,
        )
        response = requests.get(f"http://{ip.group(1).decode()}/index.html", timeout=10)
        assert (
            response.status_code == 200
        )  # the status log line stands in for /device/status (Task 15)

    def test_radio_fault_thread_rest_answers_503(self, dut, c6_held_in_reset):
        # On OpenThread failing to initialise the RCP: the Thread REST API answers 503 instead of crashing.
        _, ip = dut.expect(
            [rb"mode_fsm: mode RADIO_FAULT", GOT_IP],
            expect_all=True,
            timeout=BOOT_TIMEOUT_S,
        )
        response = requests.get(f"http://{ip.group(1).decode()}/node/state", timeout=10)
        assert response.status_code == 503

    def test_radio_fault_web_gui_api_answers_503(self, dut, c6_held_in_reset):
        # Same for the Web GUI handlers and /diagnostics, which have no single choke point (Task 14 REST guard):
        # every one of them answers 503 instead of crashing on the missing OpenThread instance.
        _, ip = dut.expect(
            [rb"mode_fsm: mode RADIO_FAULT", GOT_IP],
            expect_all=True,
            timeout=BOOT_TIMEOUT_S,
        )
        base = f"http://{ip.group(1).decode()}"
        refused = {}
        for path in WEB_GUI_GET_PATHS:
            refused["GET " + path] = requests.get(base + path, timeout=10).status_code
        for path in WEB_GUI_POST_PATHS:
            refused["POST " + path] = requests.post(
                base + path, json={}, timeout=10
            ).status_code
        assert refused == {key: 503 for key in refused}
        # The board is still alive and serving.
        assert requests.get(base + "/index.html", timeout=10).status_code == 200


class TestBootMdns:
    """br_core.boot, mDNS"""

    def test_board_answers_mdns_for_its_hostname(self, dut):
        # On boot with Ethernet address: board answers mDNS for esp-otbr-<mac3>.local.
        hostname = dut.expect(HOSTNAME, timeout=BOOT_TIMEOUT_S).group(1).decode()
        ip = dut.expect(GOT_IP, timeout=BOOT_TIMEOUT_S).group(1).decode()
        # The host has no address on the board's layer-2 segment, so the query runs in a Docker macvlan
        # container there (docker network otbr-p0, see docs/phase0/RESULTS.md).
        answer = subprocess.run(
            [
                "docker", "run", "--rm", "--network", MDNS_NETWORK,
                "-v", f"{MDNS_QUERY}:/q.py:ro", MDNS_IMAGE, "python", "/q.py", f"{hostname}.local", "10",
            ],
            capture_output=True, text=True, timeout=120, check=False,
        )  # fmt: skip
        addresses = [ipaddress.ip_address(line) for line in answer.stdout.split()]
        assert addresses, answer.stderr
        assert ipaddress.ip_address(ip) in addresses
        # SLAAC and link-local addresses are EUI-64: the last three MAC bytes are the hostname suffix.
        suffix = bytes.fromhex(hostname[-6:])
        ipv6 = [a for a in addresses if a.version == 6]
        assert ipv6 and all(a.packed[-3:] == suffix for a in ipv6), addresses


@pytest.mark.manual_cable
class TestBootWithoutEthernetLink:
    """br_core.boot, Ethernet link absent at boot"""

    def test_border_router_init_runs_when_an_address_appears(self, dut):
        # On Ethernet link absent at boot: BR init runs when an address appears.
        input("Unplug the Ethernet cable from the board, then press Enter...")
        dut.serial.hard_reset()
        dut.expect(rb"br_core: OpenThread initialised", timeout=BOOT_TIMEOUT_S)
        with pytest.raises(pexpect.TIMEOUT):
            dut.expect(rb"br_core: starting the border router", timeout=20)
        input("Plug the Ethernet cable back in, then press Enter...")
        dut.expect(rb"br_core: starting the border router", timeout=BOOT_TIMEOUT_S)
        dut.expect(rb"mode_fsm: mode BR_RUNNING", timeout=BOOT_TIMEOUT_S)

    def test_boot_without_link_marks_valid_and_starts_br_on_link_up(
        self, dut, stored_dataset, boot_slot, write_partition, send
    ):
        # Review focus 5: pending-verify image booted without a link is marked valid after 60 s,
        # the border router starts once the cable is plugged in, and a link flap keeps the leader role.
        write_partition("ota_1", Path(dut.app.binary_path) / "p4.bin")
        send("keep_pending", rb"keep_pending: set")
        input("Unplug the Ethernet cable from the board, then press Enter...")
        boot_slot(1)
        dut.write("ota_state")
        dut.expect(
            rb"ota_state: running=1 state=PENDING_VERIFY", timeout=CONSOLE_TIMEOUT_S
        )
        dut.expect(rb"validity: app marked valid", timeout=90)
        assert "starting the border router" not in dut.pexpect_proc.before.decode(
            errors="replace"
        )
        input("Plug the Ethernet cable back in, then press Enter...")
        dut.expect(rb"br_core: starting the border router", timeout=BOOT_TIMEOUT_S)
        dut.expect(
            rb"br_core: Thread started with the stored dataset", timeout=BOOT_TIMEOUT_S
        )
        wait_ot_state(dut, {"leader"}, LEADER_TIMEOUT_S)
        input("Unplug and replug the Ethernet cable (link flap), then press Enter...")
        time.sleep(10)
        assert wait_ot_state(dut, {"leader"}, 30) == "leader"
        boot_slot(0)
