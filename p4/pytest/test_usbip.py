"""USB/IP server (spec docs/superpowers/specs/2026-11-10-usbip-design.md): /device/usbip, the TCP 3240 listener and the
USB channel rules. Written with Task 12 of the USB/IP plan as the contract; run in the batched on-board pass, never
inside a task.

The first five tests need the board alone (no USB device on the USB-A port). The two marked `usb_dongle` need the
ZigStar OllyZig on the USB-A port (run with `pytest -s -m usb_dongle`, see test_dongle.py). The enable flag is stored
and takes a reboot, so the tests reboot the board; every test leaves USB/IP off.
"""

import socket
import struct
import time

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
ST_DEV_BUSY = 2
ST_NODEV = 4
FIRST_BYTE_S = 10
FIRST_BYTE_SLACK_S = 4
IO_TIMEOUT_S = 5
COUNTER_WAIT_S = 5
USBIP_REASON = "USB is exported over USB/IP"


def get_usbip(base_url):
    reply = requests.get(f"{base_url}/device/usbip", timeout=10)
    assert reply.status_code == requests.codes.ok, reply.text
    return reply.json()


def put_usbip(base_url, body):
    return requests.put(f"{base_url}/device/usbip", json=body, timeout=10)


def set_usbip(dut, base_url, enabled):
    """PUT the flag, wait for the reboot it announces; return (the board's URL after it, the PUT reply body)."""
    reply = put_usbip(base_url, {"enabled": enabled})
    assert reply.status_code == requests.codes.ok, reply.text
    body = reply.json()
    if not body["reboot"]:
        return base_url, body
    dut.expect(ROM_BANNER, timeout=BOOT_TIMEOUT_S)
    address = dut.expect(GOT_IP, timeout=BOOT_TIMEOUT_S).group(1).decode()
    wait_reachable(address)
    return f"http://{address}", body


@pytest.fixture
def usbip(dut, board_ip):
    """usbip(): the board's URL, with USB/IP switched off first and again at the end (the board is shared)."""
    state = {"url": f"http://{board_ip}"}

    def switch_off():
        if get_usbip(state["url"])["enabled"]:
            state["url"], _ = set_usbip(dut, state["url"], False)

    switch_off()
    yield state
    switch_off()


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
    usbip["url"], _ = set_usbip(dut, usbip["url"], True)
    if get_usbip(usbip["url"])["device"] is not None:
        pytest.skip("a USB device is attached")
    return usbip["url"]


def test_off_by_default(usbip):
    # On a board with USB/IP stored off: state off and nothing listens on 3240.
    status = get_usbip(usbip["url"])
    assert status["enabled"] is False
    assert status["state"] == "off"
    with pytest.raises(OSError), usbip_connect(usbip["url"]):
        pass


def test_enable_without_device(dut, usbip):
    # PUT enable answers {"reboot": true}; after the reboot: no_device, one interface, empty DEVLIST, IMPORT is ST_NODEV.
    url, body = set_usbip(dut, usbip["url"], True)
    usbip["url"] = url
    assert body == {"reboot": True}
    status = get_usbip(url)
    if status["device"] is not None:
        pytest.skip("a USB device is attached")
    assert status["enabled"] is True
    assert status["state"] == "no_device"
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
    usbip["url"], _ = set_usbip(dut, usbip["url"], True)
    assert get_usbip(usbip["url"])["enabled"] is True
    url, body = set_usbip(dut, usbip["url"], False)
    usbip["url"] = url
    assert body == {"reboot": True}
    status = get_usbip(url)
    assert status["enabled"] is False
    assert status["state"] == "off"
    with pytest.raises(OSError), usbip_connect(url):
        pass
    assert put_usbip(url, {"enabled": False}).json() == {"reboot": False}


@pytest.fixture
def dongle(usbip):
    """The id of the plugged-in dongle (skipped without one); modes restored afterwards."""
    channel = find_dongle(usbip["url"])
    if channel is None:
        pytest.skip("no USB dongle attached")
    yield channel["id"]
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
    usbip["url"], _ = set_usbip(dut, usbip["url"], True)
    channel = get_channel(usbip["url"], dongle)
    assert channel["mode"] == "unavailable"
    assert channel["channel_reason"] == USBIP_REASON
    assert channel["stored_mode"] == "rcp-proxy"
    reply = put_channel(usbip["url"], {"mode": "off"}, dongle)
    assert reply.status_code == requests.codes.conflict, reply.text
    assert reply.json()["error"] == "usbip_enabled"
    usbip["url"], _ = set_usbip(dut, usbip["url"], False)
    deadline = time.monotonic() + MOVE_TIMEOUT_S
    while find_dongle(usbip["url"]) is None and time.monotonic() < deadline:
        time.sleep(1)
    assert get_channel(usbip["url"], dongle)["stored_mode"] == "rcp-proxy"
