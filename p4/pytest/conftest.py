"""Fixtures for target tests against the P4 test build (see STYLEGUIDE.md, Tests)."""

import contextlib
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

import esptool
import pexpect
import pytest
import requests
import serial
from pytest_embedded_serial_esp.serial import EspSerial

SPIFFSGEN = Path(os.environ.get("IDF_PATH", "")) / "components/spiffs/spiffsgen.py"
GOT_IP = re.compile(rb"eth: got ip (\d+\.\d+\.\d+\.\d+)")
BOOT_TIMEOUT_S = 60
REACHABLE_TIMEOUT_S = 20
BOOT_SLOT_ATTEMPTS = 10
CLEAR_ATTEMPTS = 3
CLEAR_ACK_TIMEOUT_S = 5
BOOT_SLOT_ACK_TIMEOUT_S = 5
CONSOLE_READY = rb"Returned from app_main"  # logged when app_main returns, after the radio and HTTP start
PEER_PORT = os.environ.get(
    "PEER_PORT", "/dev/ttyACM1"
)  # Thread test peer running ot_cli
OT_DONE = re.compile(r"^(Done|Error \d+: .*)$")


def pytest_configure(config):
    """Default the app to the p4 project and take the serial port from $P4_PORT unless --port is given."""
    if not getattr(config.option, "app_path", None):
        config.option.app_path = str(Path(__file__).resolve().parent.parent)
    port = os.environ.get("P4_PORT")
    if port and not getattr(config.option, "port", None):
        config.option.port = port


def wait_reachable(address, timeout=REACHABLE_TIMEOUT_S):
    """Wait until the board accepts a TCP connection on port 80 (the host's ARP entry lags the `got ip` log line).

    Gives up silently: a board without HTTP (radio-less tests) must not fail the fixture."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((address, 80), timeout=2):
                return
        except OSError:
            time.sleep(0.5)


@pytest.fixture
def board_ip(dut):
    """IPv4 address of the board, parsed from the boot log line `eth: got ip <addr>`."""
    address = dut.expect(GOT_IP, timeout=BOOT_TIMEOUT_S).group(1).decode()
    wait_reachable(address)
    return address


class BoardSession(requests.Session):
    """requests session that resolves relative paths against the board."""

    def __init__(self, base_url):
        super().__init__()
        self.base_url = base_url

    def request(self, method, url, **kwargs):
        if url.startswith("/"):
            url = self.base_url + url
        kwargs.setdefault("timeout", 10)
        return super().request(method, url, **kwargs)


@pytest.fixture
def http(board_ip):
    """requests session for `http://<board_ip>`; pass paths such as `/web/index.html`."""
    with BoardSession(f"http://{board_ip}") as session:
        yield session


ROM_BANNER = rb"ESP-ROM:esp32p4"  # first line of a boot
CAPTURE_START_TIMEOUT_S = 30
PCAP_GLOBAL_HEADER_LEN = 24


def start_capture(base_url, channel=15, duration=3600):
    """Open GET /device/sniffer.pcap as a stream and return the response once the headers arrived (the capture runs).

    The first chunk (pcap global header) is only sent after the sniffer mode was entered."""
    response = requests.get(
        f"{base_url}/device/sniffer.pcap",
        params={"channel": channel, "duration": duration},
        stream=True,
        timeout=CAPTURE_START_TIMEOUT_S,
    )
    assert response.status_code == requests.codes.ok, response.text
    return response


def stop_capture(base_url, response=None):
    """End a capture: DELETE /device/sniffer (404 if none) and close the stream. Safe to call twice."""
    with contextlib.suppress(requests.RequestException):
        requests.delete(f"{base_url}/device/sniffer", timeout=30)
    if response is not None:
        response.close()


def send_until_ack(dut, command, ack):
    """Run a test console command, repeating it until `ack` appears (the console may still be starting)."""
    for _ in range(BOOT_SLOT_ATTEMPTS):
        dut.write(command)
        try:
            return dut.expect(ack, timeout=BOOT_SLOT_ACK_TIMEOUT_S)
        except pexpect.TIMEOUT:
            continue
    pytest.fail(f"`{command}` was never acknowledged")


def _on_esptool(dut, *args):
    """Run an esptool command on the DUT's serial connection (pauses the log reader)."""

    def run(serial):
        esptool.main(list(args), esp=serial.esp)

    EspSerial.use_esptool()(run)(dut.serial)


@pytest.fixture
def write_partition(dut):
    """write_partition(name, image_path): flash a file at the offset of a partition of the app."""

    def write(name, image_path):
        offset = dut.app.partition_table[name]["offset"]
        _on_esptool(dut, "write-flash", hex(offset), str(image_path))

    return write


@pytest.fixture
def erase_partition(dut):
    """erase_partition(name): erase a partition of the app."""
    return dut.serial.erase_partition


@pytest.fixture
def spiffs_image(spiffs_image_for):
    """spiffs_image(files): SPIFFS image for the 2 MB `web_0` partition; `files` maps names to text."""

    def build(files):
        return spiffs_image_for("web_0", {n: t.encode() for n, t in files.items()})

    return build


@pytest.fixture
def spiffs_image_for(dut, tmp_path):
    """spiffs_image_for(partition, files): build a SPIFFS image sized for a partition of the app.

    `files` maps file names to bytes. Uses ESP-IDF's spiffsgen with the SPIFFS defaults.
    """

    def build(partition, files):
        source = tmp_path / f"spiffs_root_{partition}"
        source.mkdir(exist_ok=True)
        for name, content in files.items():
            (source / name).write_bytes(content)
        image = tmp_path / f"{partition}.bin"
        size = dut.app.partition_table[partition]["size"]
        subprocess.run(
            [sys.executable, str(SPIFFSGEN), str(size), str(source), str(image)],
            check=True,
        )
        return image

    return build


@pytest.fixture
def boot_slot(dut):
    """boot_slot(n): commit boot partition ota_n through the test console and wait for the reboot.

    The console may still be starting (or the board rebooting after a flash write), so the command is
    repeated until the firmware acknowledges it.
    """

    def switch(slot):
        send_until_ack(dut, f"boot_slot {slot}", rb"boot_slot: ota_%d" % slot)
        dut.expect(CONSOLE_READY, timeout=BOOT_TIMEOUT_S)

    return switch


def ot(dut, command, timeout=10):
    """Run an OpenThread CLI command on the board (console command `ot`); return its output lines."""
    dut.write(f"ot {command}")
    dut.expect(rb"\r*\n(Done|Error \d+: [^\r\n]*)\r*\n", timeout=timeout)
    text = dut.pexpect_proc.before.decode(errors="replace")
    lines = [line.strip() for line in text.replace("\r", "").splitlines()]
    return [
        line
        for line in lines
        if line and not line.startswith(("ot ", "I (", "W (", "E (", "I(", "W(", "p4>"))
    ]


def wait_ot_state(dut, states, timeout):
    """Poll `ot state` on the board until it is one of `states`; return it."""
    deadline = time.monotonic() + timeout
    state = None
    while time.monotonic() < deadline:
        lines = ot(dut, "state")
        state = lines[-1] if lines else None
        if state in states:
            return state
        time.sleep(2)
    pytest.fail(f"board OpenThread state is {state!r}, expected one of {states}")


class Peer:
    """The Thread test peer (ESP32 devkit running ot_cli), driven over its UART."""

    def __init__(self, port):
        self.serial = serial.Serial(port, 115200, timeout=0.2)

    def command(self, line, timeout=10):
        self.serial.reset_input_buffer()
        self.serial.write(f"ot {line}\n".encode())
        deadline = time.monotonic() + timeout
        lines = []
        pending = b""
        while time.monotonic() < deadline:
            pending += self.serial.read(4096)
            *complete, pending = pending.split(b"\n")
            for raw in complete:
                text = raw.decode(errors="replace").strip()
                if OT_DONE.match(text):
                    return lines
                if text and not text.startswith(("ot ", "I (", "W (", "E (", ">")):
                    lines.append(text)
        pytest.fail(f"peer: no answer to `ot {line}`: {lines}")

    def wait_state(self, states, timeout):
        deadline = time.monotonic() + timeout
        state = None
        while time.monotonic() < deadline:
            lines = self.command("state")
            state = lines[-1] if lines else None
            if state in states:
                return state
            time.sleep(2)
        pytest.fail(f"peer OpenThread state is {state!r}, expected one of {states}")

    def restart(self, timeout=30):
        """Reboot the peer's OpenThread (`ot reset`) and wait until its CLI answers again. A peer that was stopped and
        started again keeps a SLAAC address that it does not answer echo requests on (pd-investigation-report.md)."""
        self.serial.reset_input_buffer()
        self.serial.write(b"ot reset\n")
        time.sleep(6)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                self.command("state", timeout=3)
                return
            except pytest.fail.Exception:
                time.sleep(1)
        pytest.fail("peer: no CLI after `ot reset`")

    def leave(self):
        self.command("thread stop")
        self.command("ifconfig down")


@pytest.fixture
def peer():
    """The Thread test peer, stopped before and after the test."""
    try:
        device = Peer(PEER_PORT)
    except serial.SerialException as error:
        pytest.skip(f"no Thread test peer on {PEER_PORT}: {error}")
    device.leave()
    yield device
    device.leave()
    device.serial.close()


@pytest.fixture
def reboot(dut):
    """reboot(): hard-reset the board; the log then restarts at the ROM banner."""

    def run():
        dut.serial.hard_reset()
        dut.expect(rb"ESP-ROM:esp32p4", timeout=BOOT_TIMEOUT_S)

    return run


@pytest.fixture
def fresh_nvs(dut, erase_partition, reboot):
    """Erase the whole nvs partition (no stored dataset) and reboot."""
    erase_partition("nvs")
    reboot()


@pytest.fixture
def stored_dataset(dut, fresh_nvs, reboot):
    """Store a freshly generated active dataset on the board, reboot, and return its hex TLVs.

    The board has just started with the stored dataset when this returns (its log lines are still pending).
    """
    dut.expect(
        rb"br_core: no stored dataset, Thread stays idle", timeout=BOOT_TIMEOUT_S
    )
    ot(dut, "dataset init new")
    ot(dut, "dataset commit active")
    tlvs = ot(dut, "dataset active -x")[-1]
    reboot()
    return tlvs


@pytest.fixture
def send(dut):
    """send(command, ack): run a test console command, repeating it until `ack` appears.

    The console may still be starting (the board boots, syncs the radio and starts HTTP before app_main returns).
    """

    def run(command, ack):
        return send_until_ack(dut, command, ack)

    return run


@pytest.fixture
def c6_hold(send):
    """From the next boot on the C6 stays in reset (radio fault); released again afterwards."""
    send("c6_hold 1", rb"c6_hold: ok")
    yield
    send("c6_hold 0", rb"c6_hold: ok")


@pytest.fixture
def probe_override(send):
    """From the next boot on the boot-time probe is forced to `empty` or `fail`; cleared afterwards."""

    def force(mode):
        send(f"rcp_probe_override {mode}", rb"rcp_probe_override: ok")

    yield force
    send("rcp_probe_override real", rb"rcp_probe_override: ok")


def _clear_test_flags(dut):
    """Best effort: `c6_hold 0`, `rcp_probe_override real` and `c6_jumper_ignore 0`, each repeated until acknowledged."""
    for command, ack in (
        ("c6_hold 0", rb"c6_hold: ok"),
        ("rcp_probe_override real", rb"rcp_probe_override: ok"),
        ("c6_jumper_ignore 0", rb"c6_jumper_ignore: ok"),
    ):
        for _ in range(CLEAR_ATTEMPTS):
            dut.write(command)
            try:
                dut.expect(ack, timeout=CLEAR_ACK_TIMEOUT_S)
                break
            except pexpect.TIMEOUT:
                continue


@pytest.hookimpl(tryfirst=True)
def pytest_runtest_teardown(item):
    """After every test that used the board, clear the persistent test flags (c6_hold, rcp_probe_override), so an
    aborted test (rcp_kill, c6_hold 1) does not leave the C6 held in reset for the next boot.

    A hook, not a fixture: it runs before any fixture is torn down, while the `dut` console still exists (an
    autouse fixture is finalised last, after `dut` is gone). The flags take effect from the next boot."""
    dut = getattr(item, "funcargs", {}).get("dut")
    if dut is None:
        return
    # The console may be gone (crash, reset): nothing to clean up then.
    with contextlib.suppress(Exception):
        _clear_test_flags(dut)
