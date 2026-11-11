"""USB/IP server (spec docs/superpowers/specs/2026-11-10-usbip-design.md): /device/usbip, the TCP 3240 listener and the
USB channel rules. Written with Task 12 of the USB/IP plan as the contract; run in the batched on-board pass, never
inside a task.

The rig runs the board in single-interface mode (`interface` is asserted `single`).

The first five tests need the board alone (no USB device on the USB-A port). The two marked `usb_dongle` need the
ZigStar OllyZig on the USB-A port (run with `pytest -s -m usb_dongle`, see test_dongle.py). The enable flag is stored
and takes a reboot, so the tests reboot the board; every test leaves USB/IP off.
"""

import contextlib
import socket
import struct
import time

import pexpect
import pytest
import requests
from conftest import BOOT_TIMEOUT_S, GOT_IP, ROM_BANNER, wait_reachable
from test_channels import get_channel, put_channel, set_mode
from test_dongle import MOVE_TIMEOUT_S, find_dongle, move_thread, reset_modes

USBIP_PORT = 3240
USBIP_VERSION = 0x0111
OP_REQ_DEVLIST = 0x8005
OP_REQ_IMPORT = 0x8003
OP_HDR = struct.Struct(">HHI")  # version, code, status
BUSID_LEN = 32
BUSID = b"1-1"
ST_NODEV = 4
FIRST_BYTE_S = 10
FIRST_BYTE_SLACK_S = 4
IO_TIMEOUT_S = 5
COUNTER_WAIT_S = 5
STATE_WAIT_S = 15
USBIP_REASON = "USB is exported over USB/IP"


def get_usbip(base_url):
    reply = requests.get(f"{base_url}/device/usbip", timeout=10)
    assert reply.status_code == requests.codes.ok, reply.text
    return reply.json()


def put_usbip(base_url, body):
    return requests.put(f"{base_url}/device/usbip", json=body, timeout=10)


def get_usbip_when_up(base_url, timeout=BOOT_TIMEOUT_S):
    """GET /device/usbip, retried while the board is unreachable (it may be mid-reboot)."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            return get_usbip(base_url)
        except requests.RequestException:
            if time.monotonic() > deadline:
                raise
            time.sleep(1)


def wait_boot(dut, session):
    """Wait for a reboot in flight to finish; keep session["url"] at the board's address after it."""
    dut.expect(ROM_BANNER, timeout=BOOT_TIMEOUT_S)
    address = dut.expect(GOT_IP, timeout=BOOT_TIMEOUT_S).group(1).decode()
    session["url"] = f"http://{address}"
    wait_reachable(address)


def set_usbip(dut, session, enabled):
    """PUT the flag, wait for the reboot it announces; return the PUT reply body. session["url"] follows the board."""
    reply = put_usbip(session["url"], {"enabled": enabled})
    assert reply.status_code == requests.codes.ok, reply.text
    body = reply.json()
    if body["reboot"]:
        wait_boot(dut, session)
    return body


def wait_state(base_url, wanted, timeout=STATE_WAIT_S):
    """Poll GET /device/usbip until `state` is `wanted` (USB enumeration lags the boot); return the status."""
    deadline = time.monotonic() + timeout
    status = get_usbip_when_up(base_url)
    while status["state"] != wanted and time.monotonic() < deadline:
        time.sleep(0.5)
        status = get_usbip_when_up(base_url)
    assert status["state"] == wanted, status
    return status


def wait_dongle(base_url, timeout=MOVE_TIMEOUT_S):
    """Wait until the dongle is listed as an available channel again; fail if it never is."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        with contextlib.suppress(requests.RequestException):
            channel = find_dongle(base_url)
            if channel is not None:
                return channel
        time.sleep(1)
    pytest.fail("the dongle never came back as a channel")


def switch_off(dut, session):
    """USB/IP stored off and off in this boot, whatever state the board is in (mid-reboot, flag pending)."""
    status = get_usbip_when_up(session["url"])
    if status[
        "pending_reboot"
    ]:  # stored and running differ: a reboot is due; wait for it, else force one
        try:
            wait_boot(dut, session)
        except pexpect.TIMEOUT:
            dut.serial.hard_reset()
            wait_boot(dut, session)
        status = get_usbip_when_up(session["url"])
    if status["enabled"]:
        set_usbip(dut, session, False)


@pytest.fixture
def usbip(dut, board_ip):
    """Session dict {"url": ...}, with USB/IP switched off first and again at the end (the board is shared)."""
    session = {"url": f"http://{board_ip}"}
    switch_off(dut, session)
    yield session
    switch_off(dut, session)


def op_request(code, busid=None):
    """An OP request: version, code, status 0, and for IMPORT the 32 byte bus id."""
    data = OP_HDR.pack(USBIP_VERSION, code, 0)
    if busid is not None:
        data += busid.ljust(BUSID_LEN, b"\0")
    return data


def recv_exact(sock, count):
    data = b""
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            break
        data += chunk
    return data


def usbip_connect(base_url):
    host = base_url.removeprefix("http://")
    return socket.create_connection((host, USBIP_PORT), timeout=IO_TIMEOUT_S)


def closed_without_reply(sock):
    """True when the peer closes (or resets) the connection without sending a byte."""
    try:
        return sock.recv(1) == b""
    except ConnectionResetError:
        return True


def wait_refused(base_url, wanted):
    deadline = time.monotonic() + COUNTER_WAIT_S
    refused = None
    while time.monotonic() < deadline:
        refused = get_usbip(base_url)["counters"]["refused"]
        if refused == wanted:
            return
        time.sleep(0.5)
    pytest.fail(f"counters.refused is {refused}, expected {wanted}")


@pytest.fixture
def exported(dut, usbip):
    """USB/IP switched on and the board rebooted; skipped when a USB device is attached (the tests expect none)."""
    set_usbip(dut, usbip, True)
    if get_usbip_when_up(usbip["url"])["device"] is not None:
        pytest.skip("a USB device is attached")
    return usbip["url"]


def test_off_by_default(usbip):
    # On a board with USB/IP stored off: state off and nothing listens on 3240.
    status = get_usbip(usbip["url"])
    assert status["enabled"] is False
    assert status["state"] == "off"
    with pytest.raises(ConnectionRefusedError), usbip_connect(usbip["url"]):
        pass


def test_enable_without_device(dut, usbip):
    # PUT enable answers {"reboot": true}; after the reboot: no_device, one interface, empty DEVLIST, IMPORT is ST_NODEV.
    body = set_usbip(dut, usbip, True)
    url = usbip["url"]
    assert body == {"reboot": True}
    status = get_usbip_when_up(url)
    if status["device"] is not None:
        pytest.skip("a USB device is attached")
    status = wait_state(url, "no_device")
    assert status["enabled"] is True
    assert status["interface"] == "single"
    with usbip_connect(url) as sock:
        sock.sendall(op_request(OP_REQ_DEVLIST))
        version, code, st = OP_HDR.unpack(recv_exact(sock, OP_HDR.size))
        assert (version, code, st) == (USBIP_VERSION, 0x0005, 0)
        assert struct.unpack(">I", recv_exact(sock, 4))[0] == 0
    with usbip_connect(url) as sock:
        sock.sendall(op_request(OP_REQ_IMPORT, BUSID))
        version, code, st = OP_HDR.unpack(recv_exact(sock, OP_HDR.size))
        assert (version, code, st) == (USBIP_VERSION, 0x0003, ST_NODEV)
        assert sock.recv(1) == b""


def test_http_first_bytes_refused(exported):
    # A first byte that is not 0x01 (an HTTP request): closed without a reply, counters.refused grows by 1.
    before = get_usbip(exported)["counters"]["refused"]
    with usbip_connect(exported) as sock:
        sock.sendall(b"GET / HTTP/1.1\r\nHost: board\r\n\r\n")
        assert closed_without_reply(sock)
    wait_refused(exported, before + 1)


def test_silent_client_closed(exported):
    # A connection that sends nothing is closed after 10 s.
    with usbip_connect(exported) as sock:
        sock.settimeout(FIRST_BYTE_S + FIRST_BYTE_SLACK_S)
        start = time.monotonic()
        assert closed_without_reply(sock)
        elapsed = time.monotonic() - start
    assert FIRST_BYTE_S - 1 <= elapsed <= FIRST_BYTE_S + FIRST_BYTE_SLACK_S


def test_disable_restores(dut, usbip):
    # PUT disable reboots into state off, nothing listens, and the same value again is {"reboot": false}.
    set_usbip(dut, usbip, True)
    assert get_usbip(usbip["url"])["enabled"] is True
    body = set_usbip(dut, usbip, False)
    url = usbip["url"]
    assert body == {"reboot": True}
    status = get_usbip(url)
    assert status["enabled"] is False
    assert status["state"] == "off"
    with pytest.raises(ConnectionRefusedError), usbip_connect(url):
        pass
    assert put_usbip(url, {"enabled": False}).json() == {"reboot": False}


@pytest.fixture
def dongle(dut, usbip):
    """The id of the plugged-in dongle (skipped without one); USB/IP off and modes restored afterwards."""
    channel = find_dongle(usbip["url"])
    if channel is None:
        pytest.skip("no USB dongle attached")
    yield channel["id"]
    switch_off(
        dut, usbip
    )  # before the modes: a mode PUT is refused (409) while USB/IP is on
    reset_modes(usbip["url"], channel["id"])


@pytest.mark.usb_dongle
def test_enable_refused_with_otbr_dongle(usbip, dongle):
    # With Thread on the dongle (otbr): enabling answers 409 usb_channel_in_use naming it, and nothing changes.
    move_thread(usbip["url"], dongle)
    reply = put_usbip(usbip["url"], {"enabled": True})
    assert reply.status_code == requests.codes.conflict, reply.text
    body = reply.json()
    assert body["error"] == "usb_channel_in_use"
    assert body["detail"].startswith(f"{dongle} runs otbr")
    assert get_usbip(usbip["url"])["enabled"] is False


@pytest.mark.usb_dongle
def test_usb_channel_while_exported(dut, usbip, dongle):
    # While exported: the channel is unavailable with the reason, a mode PUT is 409 usbip_enabled, and after disable
    # plus the reboot its stored mode (rcp-proxy, which does not block enabling) is back.
    set_mode(usbip["url"], "rcp-proxy", dongle)
    set_usbip(dut, usbip, True)
    channel = get_channel(usbip["url"], dongle)
    assert channel["mode"] == "unavailable"
    assert channel["channel_reason"] == USBIP_REASON
    assert channel["stored_mode"] == "rcp-proxy"
    reply = put_channel(usbip["url"], {"mode": "off"}, dongle)
    assert reply.status_code == requests.codes.conflict, reply.text
    assert reply.json()["error"] == "usbip_enabled"
    set_usbip(dut, usbip, False)
    wait_dongle(usbip["url"])
    channel = get_channel(usbip["url"], dongle)
    assert channel["mode"] == "rcp-proxy"
    assert channel["stored_mode"] == "rcp-proxy"
