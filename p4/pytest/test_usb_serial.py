"""USB serial proxy (experimental): settings, validation and the one-client rule, without a dongle. Hardware-gated: run
in the batched on-board pass.

Rig: the board alone, running as a border router. No USB dongle is needed; the TCP listener works without one.
"""

import socket
import time

import pytest
import requests

DEFAULTS = {"enabled": False, "port": 6638, "baud": 115200, "flow_control": False}
FIELDS = [
    "enabled",
    "port",
    "baud",
    "flow_control",
    "dongle",
    "client",
    "bytes_to_dongle",
    "bytes_from_dongle",
    "discarded_from_client",
    "overflow_disconnects",
    "refused_connections",
]
SETTINGS = list(DEFAULTS)
CONNECT_TIMEOUT_S = 10
CLIENT_SEEN_S = 5
REFUSED_S = 5


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


@pytest.fixture
def usb_serial(base_url):
    """The proxy turned on for the test; the default settings are restored afterwards (off)."""
    r = requests.put(
        f"{base_url}/device/usb_serial", json={"enabled": True}, timeout=10
    )
    assert r.status_code == 200, r.text
    yield r.json()
    requests.put(f"{base_url}/device/usb_serial", json=DEFAULTS, timeout=10)


def get_status(base_url):
    r = requests.get(f"{base_url}/device/usb_serial", timeout=10)
    assert r.status_code == 200, r.text
    return r.json()


def connect(host, port):
    """A TCP connection to the proxy; retried briefly, since the listener opens a moment after the PUT."""
    deadline = time.monotonic() + CONNECT_TIMEOUT_S
    while True:
        try:
            return socket.create_connection((host, port), timeout=5)
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(0.5)


def test_off_by_default(base_url):
    """Keep this first in the file: it relies on the stored settings being the defaults."""
    body = get_status(base_url)
    assert body["enabled"] is False
    assert list(body) == FIELDS


def test_put_validates(base_url):
    before = get_status(base_url)
    for bad in ({"port": 80}, {"port": 0}, {"baud": 9600}, {"port": "6638"}):
        r = requests.put(f"{base_url}/device/usb_serial", json=bad, timeout=10)
        assert r.status_code == 400, (bad, r.status_code, r.text)
        assert r.json()["error"] == "invalid_request", (bad, r.text)
    after = get_status(base_url)
    assert {k: after[k] for k in SETTINGS} == {k: before[k] for k in SETTINGS}


def test_second_client_refused(base_url, board_ip, usb_serial):
    port = usb_serial["port"]
    refused_before = usb_serial["refused_connections"]
    first = connect(board_ip, port)
    try:
        deadline = time.monotonic() + CLIENT_SEEN_S
        while get_status(base_url)["client"] is None and time.monotonic() < deadline:
            time.sleep(0.2)
        assert get_status(base_url)["client"] is not None

        second = connect(board_ip, port)
        try:
            second.settimeout(REFUSED_S)
            try:
                assert second.recv(1) == b""
            except ConnectionResetError:
                pass
        finally:
            second.close()

        body = get_status(base_url)
        assert body["refused_connections"] >= refused_before + 1
        assert body["client"] is not None
    finally:
        first.close()
