"""Radio channel mux with a USB dongle (marker usb_dongle). Hardware-gated: written with Task 10 of the radio channel mux
plan as the contract, run in the batched on-board pass (Task 11) with `pytest -s -m usb_dongle`.

Rig: the ZigStar OllyZig (CC2652P, Koenkk's OpenThread RCP firmware 2025.3.1, CH340) on the board's USB-A port, running
the spinel firmware, and a person at the board: the unplug and replug steps ask for Enter, as the manual_cable tests do.
The board starts with Thread on the C6. Every test puts the modes back (C6 in `otbr`, the dongle `off`, default proxy
settings) afterwards. Optional: PROXY_HOST_IF is not needed; the proxy peers run on this host and reach the board by
its address. Environment:
  PROXY_SOAK_S   seconds the refused-connect soak runs (default 600)
  PROXY_PORT     host port the connect tests listen on (default 16638)
"""

import contextlib
import itertools
import os
import select
import shutil
import socket
import subprocess
import threading
import time

import pytest
import requests
from conftest import ot, wait_ot_state
from test_channels import (
    NO_RADIO_DETAIL,
    PROXY_DEFAULTS,
    get_channel,
    get_channels,
    get_status,
    put_channel,
    wait_mode,
)

pytestmark = pytest.mark.usb_dongle

CHIP = "CC13XX_CC26XX"
SPINEL_BAUD = 460800
STATUS_TIMEOUT_S = 90
THREAD_TIMEOUT_S = 120
DETECT_TIMEOUT_S = 15
MOVE_TIMEOUT_S = 120
PROXY_PEER_WAIT_S = 15
LATENCY_ROUNDS = 100
LATENCY_BUDGET_MS = 15.0
LISTEN_PORT = int(os.environ.get("PROXY_PORT", "16638"))
SOAK_S = int(os.environ.get("PROXY_SOAK_S", "600"))
BACKOFF_STEPS_S = (1, 2, 4, 8, 16, 30)
GROWING_GAPS = 2
GAP_GROWTH = 1.5
GAP_TOLERANCE_S = 5
BACKOFF_CAP_S = 30
HTTP_BURST = 30
HDLC_FLAG = 0x7E
HDLC_ESCAPE = 0x7D
HDLC_XOR = 0x20
CRC_INIT = 0xFFFF
CRC_POLY_REFLECTED = 0x8408
SPINEL_HEADER_FLAG = 0x80
SPINEL_CMD_PROP_VALUE_GET = 2
SPINEL_PROP_PROTOCOL_VERSION = 1
SPINEL_PROP_NCP_VERSION = 2
SPINEL_CMD_PROP_VALUE_IS = 6
AGENT_IMAGE = "usbproxy-otbr:local"
AGENT_START_S = 90
AGENT_TTY = "/dev/ttyOTBR"  # a pty made by socat inside the container (a host pty passed in gives EIO)
BACKBONE_IF = os.environ.get("PROXY_BACKBONE_IF", "lo")
REST_LOAD_THREADS = 4


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


def find_dongle(base_url):
    """The attached USB channel, or None."""
    for channel in get_channels(base_url)["channels"]:
        if channel["kind"] == "usb" and channel["mode"] != "unavailable":
            return channel
    return None


@pytest.fixture
def dongle(base_url):
    """The id of the plugged-in dongle (the test is skipped without one); modes restored afterwards."""
    channel = find_dongle(base_url)
    if channel is None:
        pytest.skip("no USB dongle attached")
    channel_id = channel["id"]
    yield channel_id
    reset_modes(base_url, channel_id)


def reset_modes(base_url, channel_id):
    """C6 back to `otbr` with Thread running, the dongle `off`, default proxy settings."""
    with contextlib.suppress(requests.RequestException):
        wait_no_move(base_url)
        put_channel(base_url, {"mode": "off", "rcp_proxy": PROXY_DEFAULTS}, channel_id)
        put_channel(base_url, {"mode": "otbr", "rcp_proxy": PROXY_DEFAULTS}, "c6")
        wait_mode(base_url, "BR_RUNNING", MOVE_TIMEOUT_S)


def wait_no_move(base_url, timeout=MOVE_TIMEOUT_S):
    """Wait until no channel is `starting` (a move that was accepted has settled), so a PUT is not refused as busy."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if all(c["state"] != "starting" for c in get_channels(base_url)["channels"]):
            return
        time.sleep(1)
    pytest.fail("a move never settled")


def wait_channel(base_url, channel_id, state, timeout=MOVE_TIMEOUT_S):
    """Poll the channel until its `state` is `state`; return it."""
    deadline = time.monotonic() + timeout
    channel = None
    while time.monotonic() < deadline:
        with contextlib.suppress(requests.RequestException):
            channel = get_channel(base_url, channel_id)
            if channel["state"] == state:
                return channel
        time.sleep(1)
    pytest.fail(
        f"channel {channel_id} is {channel and channel['state']!r}, expected {state!r}"
    )


def move_thread(base_url, channel_id):
    """PUT otbr on `channel_id` (a move when Thread runs elsewhere); wait until Thread runs on it."""
    reply = put_channel(base_url, {"mode": "otbr"}, channel_id)
    assert reply.status_code in (200, 202), reply.text
    if reply.status_code == 202:
        assert reply.json()["moving"] is True
    deadline = time.monotonic() + MOVE_TIMEOUT_S
    while time.monotonic() < deadline:
        status = get_status(base_url)
        if status["mode"] == "BR_RUNNING" and status["otbr_channel"] == channel_id:
            return reply
        time.sleep(1)
    pytest.fail(f"Thread did not start on {channel_id}")


def network_identity(dut):
    """The Thread network as the board's OpenThread CLI sees it."""
    return ot(dut, "dataset active -x")[-1]


def ask(prompt):
    input(f"\n{prompt} Press Enter... ")


def hdlc_crc(data):
    crc = CRC_INIT
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ CRC_POLY_REFLECTED if crc & 1 else crc >> 1
    return crc ^ CRC_INIT


def hdlc_frame(payload):
    """One HDLC-lite frame (flag, escaped payload and FCS, flag), as spinel uses over a serial line."""
    crc = hdlc_crc(payload)
    body = bytes(payload) + bytes([crc & 0xFF, crc >> 8])
    out = bytearray([HDLC_FLAG])
    for byte in body:
        if byte in (HDLC_FLAG, HDLC_ESCAPE, 0x11, 0x13, 0xF8):
            out += bytes([HDLC_ESCAPE, byte ^ HDLC_XOR])
        else:
            out.append(byte)
    out.append(HDLC_FLAG)
    return bytes(out)


def spinel_get(prop, tid=1):
    """A spinel PROP_VALUE_GET request for `prop` (single-byte property ids), as an unframed payload."""
    return bytes([SPINEL_HEADER_FLAG | tid, SPINEL_CMD_PROP_VALUE_GET, prop])


def read_frame(sock, timeout=2.0):
    """Read bytes from a stream until one complete HDLC frame; return its raw bytes (flags included)."""
    deadline = time.monotonic() + timeout
    data = bytearray()
    while time.monotonic() < deadline:
        ready, _, _ = select.select(
            [sock], [], [], max(0.0, deadline - time.monotonic())
        )
        if not ready:
            break
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
        start = data.find(bytes([HDLC_FLAG]))
        if start >= 0:
            end = data.find(bytes([HDLC_FLAG]), start + 1)
            if end > start + 1:
                return bytes(data[start : end + 1])
    pytest.fail(f"no HDLC frame within {timeout} s (got {bytes(data)!r})")


def connect(host, port, timeout=PROXY_PEER_WAIT_S):
    """A TCP connection to the proxy; retried briefly, since the listener opens a moment after the PUT."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            sock = socket.create_connection((host, port), timeout=5)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            return sock
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(0.5)


def host_address_towards(board):
    """The address of this host that the board reaches (for connect-direction tests)."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.connect((board, 80))
        return probe.getsockname()[0]


def proxy_state(base_url, channel_id):
    return get_channel(base_url, channel_id)["rcp_proxy"]


def wait_proxy_state(base_url, channel_id, wanted, timeout):
    deadline = time.monotonic() + timeout
    state = None
    while time.monotonic() < deadline:
        state = proxy_state(base_url, channel_id)["state"]
        if state == wanted:
            return
        time.sleep(0.5)
    pytest.fail(f"proxy state is {state!r}, expected {wanted!r}")


def test_detect_ollyzig_spinel(base_url, dongle):
    # Plug-in detection, On the OllyZig with the OpenThread RCP firmware: type spinel, chip CC13XX_CC26XX, 460800 baud.
    channel = wait_firmware(base_url, dongle)
    assert channel["firmware"]["type"] == "spinel"
    assert channel["firmware"]["chip"] == CHIP
    assert channel["firmware"]["baud"] == SPINEL_BAUD
    # "probe" after a plug-in, "passive" when the channel has run otbr since (the handshake records it again).
    assert channel["firmware"]["source"] in ("probe", "passive")
    assert channel["firmware"]["version"]
    assert channel["kind"] == "usb"
    assert channel["usb"]["driver"] in ("ch34x", "cp210x", "cdc_acm")
    assert channel["caps"]["hw_reset"] is False
    assert channel["line"]["baud"] == SPINEL_BAUD


def wait_firmware(base_url, channel_id, timeout=DETECT_TIMEOUT_S):
    deadline = time.monotonic() + timeout
    channel = get_channel(base_url, channel_id)
    while time.monotonic() < deadline and not channel["firmware"]:
        time.sleep(0.5)
        channel = get_channel(base_url, channel_id)
    assert channel["firmware"], "detection never finished"
    return channel


def test_move_thread_to_dongle_and_back(dut, base_url, dongle):
    # PUT otbr on the dongle, then on c6: 202 for the move, the same Thread network both ways, the old radio goes off.
    identity = network_identity(dut)
    first = move_thread(base_url, dongle)
    assert first.status_code == 202
    assert first.json() == {"moving": True, "moved_from": "c6"}
    wait_ot_state(dut, {"leader", "router", "child"}, THREAD_TIMEOUT_S)
    assert network_identity(dut) == identity
    assert get_channel(base_url, "c6")["mode"] == "off"
    assert get_channel(base_url, dongle)["mode"] == "otbr"
    assert get_status(base_url)["otbr_channel"] == dongle
    second = move_thread(base_url, "c6")
    assert second.status_code == 202
    assert second.json() == {"moving": True, "moved_from": dongle}
    wait_ot_state(dut, {"leader", "router", "child"}, THREAD_TIMEOUT_S)
    assert network_identity(dut) == identity
    assert get_channel(base_url, dongle)["mode"] == "off"
    assert get_status(base_url)["otbr_channel"] == "c6"
    # A third trip: the radio capability check passes every time (no RADIO_FAULT from it).
    move_thread(base_url, dongle)
    assert get_status(base_url)["mode"] == "BR_RUNNING"


def test_move_refusals(dut, base_url, dongle):
    # PUT on a move, On a move that also carries line settings: 400; during a move a mode PUT: 409 busy; the attached
    # dongle cannot be forgotten: 409 in_use; a ZNP/EZSP dongle would give 409 probe_failed or incompatible (not here).
    mixed = put_channel(base_url, {"mode": "otbr", "baud": SPINEL_BAUD}, dongle)
    assert mixed.status_code == 400, mixed.text
    assert mixed.json()["error"] == "bad_value"
    assert get_channel(base_url, "c6")["mode"] == "otbr"
    forget = requests.delete(f"{base_url}/device/channels/{dongle}", timeout=10)
    assert forget.status_code == 409, forget.text
    assert forget.json()["error"] == "in_use"
    started = put_channel(base_url, {"mode": "otbr"}, dongle)
    assert started.status_code == 202, started.text
    busy = put_channel(base_url, {"mode": "rcp-proxy"}, dongle)
    assert busy.status_code == 409, busy.text
    assert busy.json()["error"] == "busy"
    deadline = time.monotonic() + MOVE_TIMEOUT_S
    while (
        get_status(base_url)["otbr_channel"] != dongle
    ):  # not BR_RUNNING: the board is still on the C6 mid-move
        assert time.monotonic() < deadline, "the move never finished"
        time.sleep(1)
    wait_mode(base_url, "BR_RUNNING")


def test_unplug_and_replug_under_otbr(dut, base_url, dongle):
    # Unplug and replug of the OTBR dongle: NO_RADIO at once, no reboot (uptime keeps growing), the channel unavailable with its
    # stored mode kept, Thread REST 503.
    identity = network_identity(dut)
    move_thread(base_url, dongle)
    uptime = requests.get(f"{base_url}/device/diag", timeout=10).json()["uptime_s"]
    ask(f"Unplug the dongle ({dongle}) from the board's USB-A port.")
    status = wait_mode(base_url, "NO_RADIO", 15)
    assert status["otbr_channel"] is None or status["otbr_channel"] == dongle
    channel = get_channel(base_url, dongle)
    assert channel["mode"] == "unavailable"
    assert channel["stored_mode"] == "otbr"
    assert "unplugged" in status["fault_reason"] or status["fault_reason"] is None
    reply = requests.get(f"{base_url}/node/state", timeout=10)
    assert reply.status_code == 503
    assert reply.json()["detail"] == NO_RADIO_DETAIL
    time.sleep(5)
    later = requests.get(f"{base_url}/device/diag", timeout=10).json()["uptime_s"]
    assert later > uptime, "the board rebooted"
    # Replug, in the same test (the fixture's teardown must find the dongle attached): detected again, Thread starts on
    # it with the same network, because the stored mode stayed otbr.
    ask("Plug the dongle back into the board's USB-A port.")
    wait_mode(base_url, "BR_RUNNING", STATUS_TIMEOUT_S)
    channel = find_dongle(base_url)
    assert channel is not None
    assert channel["mode"] == "otbr"
    assert get_status(base_url)["otbr_channel"] == channel["id"]
    wait_ot_state(dut, {"leader", "router", "child"}, THREAD_TIMEOUT_S)
    assert network_identity(dut) == identity
    assert channel["firmware"]["type"] == "spinel"


def test_forget_unplugged_dongle(base_url, dongle):
    # DELETE of a dongle that is unplugged: 204 and the channel leaves the list; it comes back as a new `off` channel.
    ask(f"Unplug the dongle ({dongle}).")
    deadline = time.monotonic() + DETECT_TIMEOUT_S
    while find_dongle(base_url) is not None and time.monotonic() < deadline:
        time.sleep(0.5)
    forget = requests.delete(f"{base_url}/device/channels/{dongle}", timeout=10)
    assert forget.status_code == 204, forget.text
    assert forget.content == b""
    assert all(c["id"] != dongle for c in get_channels(base_url)["channels"])
    ask("Plug the dongle back in.")
    deadline = time.monotonic() + DETECT_TIMEOUT_S
    while find_dongle(base_url) is None and time.monotonic() < deadline:
        time.sleep(0.5)
    fresh = find_dongle(base_url)
    assert fresh is not None
    assert fresh["stored_mode"] == "off"


def test_unplug_during_rest_load(dut, base_url, dongle):
    # Task 11 review focus 1: unplug of the OTBR dongle while Thread REST calls are in flight: no abort, no reboot,
    # every call answers (200, 503 or an error), NO_RADIO within seconds, and a replug brings the same network back.
    identity = network_identity(dut)
    move_thread(base_url, dongle)
    uptime = requests.get(f"{base_url}/device/diag", timeout=10).json()["uptime_s"]
    stop = threading.Event()
    answers = []

    def hammer():
        while not stop.is_set():
            try:
                answers.append(
                    requests.get(
                        f"{base_url}/node/dataset/active", timeout=20
                    ).status_code
                )
            except requests.RequestException as error:
                answers.append(type(error).__name__)

    workers = [
        threading.Thread(target=hammer, daemon=True) for _ in range(REST_LOAD_THREADS)
    ]
    for worker in workers:
        worker.start()
    try:
        ask(f"Unplug the dongle ({dongle}) while the REST load runs.")
        wait_mode(base_url, "NO_RADIO", 15)
        time.sleep(10)
    finally:
        stop.set()
        for worker in workers:
            worker.join(timeout=30)
    assert answers, "no REST call was made"
    assert any(a == 503 for a in answers), answers[-10:]
    assert (
        requests.get(f"{base_url}/device/diag", timeout=10).json()["uptime_s"] > uptime
    )
    ask("Plug the dongle back in.")
    wait_mode(base_url, "BR_RUNNING", STATUS_TIMEOUT_S)
    wait_ot_state(dut, {"leader", "router", "child"}, THREAD_TIMEOUT_S)
    assert network_identity(dut) == identity


def test_proxy_listen_tcp_latency(base_url, board_ip, dongle):
    # rcp-proxy listening on TCP, On the dongle: a spinel PROP_VALUE_GET round trip through the proxy takes under 15 ms
    # at the median, and the second client is refused while the first stays.
    assert put_channel(base_url, {"mode": "rcp-proxy"}, dongle).status_code == 200
    wait_proxy_state(base_url, dongle, "listening", PROXY_PEER_WAIT_S)
    port = proxy_state(base_url, dongle)["port"]
    sock = connect(board_ip, port)
    try:
        wait_proxy_state(base_url, dongle, "connected", PROXY_PEER_WAIT_S)
        second = connect(board_ip, port)
        second.settimeout(5)
        try:
            assert second.recv(1) == b""
        except ConnectionResetError:
            pass
        finally:
            second.close()
        assert proxy_state(base_url, dongle)["state"] == "connected"
        request = hdlc_frame(spinel_get(SPINEL_PROP_PROTOCOL_VERSION))
        times = []
        for _ in range(LATENCY_ROUNDS):
            start = time.perf_counter()
            sock.sendall(request)
            reply = read_frame(sock)
            times.append((time.perf_counter() - start) * 1000)
            assert reply[0] == HDLC_FLAG
            time.sleep(0.01)
        times.sort()
        median = times[len(times) // 2]
        assert median < LATENCY_BUDGET_MS, (
            f"median {median:.1f} ms, worst {times[-1]:.1f} ms"
        )
    finally:
        sock.close()
    wait_proxy_state(base_url, dongle, "listening", PROXY_PEER_WAIT_S)
    counters = get_channel(base_url, dongle)["counters"]
    assert counters["frames_out"] >= LATENCY_ROUNDS or counters["bytes_out"] > 0


def accept_and_close(listener, stop, times):
    """Accept connections on the host listener and close each at once; record when each arrived."""
    listener.settimeout(1)
    while not stop.is_set():
        try:
            peer, _ = listener.accept()
        except OSError:
            continue
        times.append(time.monotonic())
        peer.close()


def test_proxy_connect_backoff(base_url, board_ip, dongle):
    # rcp-proxy connecting over TCP, On a host listener that drops every connection: the gap between the board's
    # attempts grows (1, 2, 4, 8 s ... toward 30 s), and it connects and stays when the listener keeps a connection.
    host = host_address_towards(board_ip)
    settings = {
        "direction": "connect",
        "adapter": "tcp",
        "host": host,
        "port": LISTEN_PORT,
    }
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("", LISTEN_PORT))
    listener.listen(1)
    times = []
    stop = threading.Event()
    watcher = threading.Thread(
        target=accept_and_close, args=(listener, stop, times), daemon=True
    )
    try:
        watcher.start()
        assert (
            put_channel(
                base_url, {"mode": "rcp-proxy", "rcp_proxy": settings}, dongle
            ).status_code
            == 200
        )
        time.sleep(sum(BACKOFF_STEPS_S[:5]) + BACKOFF_STEPS_S[5] + PROXY_PEER_WAIT_S)
    finally:
        stop.set()
        watcher.join(timeout=5)
    gaps = [b - a for a, b in itertools.pairwise(times)]
    assert len(gaps) >= GROWING_GAPS + 1, f"only {len(times)} attempts: {times}"
    rises = sum(1 for a, b in itertools.pairwise(gaps) if b > a * GAP_GROWTH)
    assert rises >= GROWING_GAPS, (
        f"the retry gap does not grow: {[round(g, 1) for g in gaps]}"
    )
    assert max(gaps) < BACKOFF_CAP_S + GAP_TOLERANCE_S, gaps
    listener.listen(1)
    listener.settimeout(BACKOFF_CAP_S + PROXY_PEER_WAIT_S)
    try:
        peer, _ = listener.accept()  # the board returns; this one is kept
        wait_proxy_state(base_url, dongle, "connected", PROXY_PEER_WAIT_S)
        peer.close()
    finally:
        listener.close()


def test_proxy_connect_refused_soak(base_url, board_ip, dongle):
    # Task 11 (rcp_proxy): connect to a refusing address for PROXY_SOAK_S seconds (10 minutes by default), then check
    # the board is not out of sockets: a burst of concurrent HTTP requests is all served, and the proxy, switched to
    # listen/tcp, accepts a client. /device/diag has no socket count, so a slow leak that stays below the budget is not
    # seen by this check (see docs/radios.md).
    host = host_address_towards(board_ip)
    settings = {
        "direction": "connect",
        "adapter": "tcp",
        "host": host,
        "port": LISTEN_PORT,
    }
    assert (
        put_channel(
            base_url, {"mode": "rcp-proxy", "rcp_proxy": settings}, dongle
        ).status_code
        == 200
    )
    deadline = time.monotonic() + SOAK_S
    while time.monotonic() < deadline:
        time.sleep(5)
        assert proxy_state(base_url, dongle)["state"] in ("backoff", "connecting")
    # LWIP_MAX_SOCKETS is 24 and RCP_PROXY_SOCKETS 6, so 30 simultaneous connections cannot all be open unless sockets
    # were freed: a leaked socket or two would show as refused or timed-out requests.
    results = []

    def fetch():
        try:
            results.append(
                requests.get(f"{base_url}/device/status", timeout=30).status_code
            )
        except requests.RequestException as error:
            results.append(type(error).__name__)

    burst = [threading.Thread(target=fetch) for _ in range(HTTP_BURST)]
    for thread in burst:
        thread.start()
    for thread in burst:
        thread.join(timeout=60)
    assert results.count(requests.codes.ok) == HTTP_BURST, results
    assert (
        put_channel(
            base_url,
            {"rcp_proxy": {"direction": "listen", "adapter": "tcp", "host": ""}},
            dongle,
        ).status_code
        == 200
    )
    wait_proxy_state(base_url, dongle, "listening", PROXY_PEER_WAIT_S)
    port = proxy_state(base_url, dongle)["port"]
    with connect(board_ip, port):
        wait_proxy_state(base_url, dongle, "connected", PROXY_PEER_WAIT_S)


def test_proxy_udp_hdlc_roundtrip(base_url, board_ip, dongle):
    # rcp-proxy over UDP with hdlc framing, listening: a datagram holding one spinel frame gets one datagram back; a
    # second source is dropped while the first holds the lock; a kick releases the lock.
    assert (
        put_channel(
            base_url,
            {
                "mode": "rcp-proxy",
                "rcp_proxy": {
                    "adapter": "udp",
                    "direction": "listen",
                    "framing": "hdlc",
                },
            },
            dongle,
        ).status_code
        == 200
    )
    port = proxy_state(base_url, dongle)["port"]
    time.sleep(1)
    request = hdlc_frame(spinel_get(SPINEL_PROP_PROTOCOL_VERSION))
    first = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    second = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        first.settimeout(3)
        first.sendto(request, (board_ip, port))
        reply, _ = first.recvfrom(4096)
        assert reply[0] == HDLC_FLAG
        assert reply[-1] == HDLC_FLAG
        assert reply.count(bytes([HDLC_FLAG])) == 2, "more than one frame in a datagram"
        second.settimeout(2)
        second.sendto(request, (board_ip, port))
        with pytest.raises(socket.timeout):
            second.recvfrom(4096)
        kick = put_channel(base_url, {"rcp_proxy": {"kick": True}}, dongle)
        assert kick.status_code == 200, kick.text
        time.sleep(1)
        second.sendto(request, (board_ip, port))
        reply, _ = second.recvfrom(4096)
        assert reply[0] == HDLC_FLAG
    finally:
        first.close()
        second.close()


@pytest.fixture
def agent_container():
    """The usbproxy-otbr:local container from the spike, if Docker and the image exist; removed afterwards."""
    if shutil.which("docker") is None:
        pytest.skip("docker is not installed")
    found = subprocess.run(
        ["docker", "image", "inspect", AGENT_IMAGE], capture_output=True, check=False
    )
    if found.returncode != 0:
        pytest.skip(f"no Docker image {AGENT_IMAGE}")
    names = []

    def start(board, port):
        name = f"otbr-agent-c6-{os.getpid()}"
        names.append(name)
        script = (
            f"socat pty,link={AGENT_TTY},raw,echo=0 tcp:{board}:{port} & sleep 2; "
            f"exec otbr-agent --vendor-name=Test --model-name=Proxy -I wpan0 -B {BACKBONE_IF} "
            f"'spinel+hdlc+uart://{AGENT_TTY}?uart-baudrate=460800' 'trel://{BACKBONE_IF}'"
        )
        subprocess.run(
            ["docker", "run", "-d", "--rm", "--name", name, "--network", "host", "--privileged",
             "--entrypoint", "sh", AGENT_IMAGE, "-c", script],
            check=True, capture_output=True, text=True,
        )  # fmt: skip
        return name

    yield start
    for name in names:
        subprocess.run(["docker", "rm", "-f", name], capture_output=True, check=False)


def test_c6_proxy_otbr_agent(base_url, board_ip, agent_container, dongle):
    # rcp-proxy on the C6 with otbr-agent on this host (container usbproxy-otbr:local): the agent reaches the C6's RCP
    # through the proxy and reports a thread state; the board's own Thread stops while the C6 serves the proxy.
    del dongle  # only the dongle rig runs this test; the C6 is what is proxied
    assert put_channel(base_url, {"mode": "rcp-proxy"}, "c6").status_code == 200
    try:
        wait_mode(base_url, "NO_RADIO")
        wait_proxy_state(base_url, "c6", "listening", PROXY_PEER_WAIT_S)
        name = agent_container(board_ip, proxy_state(base_url, "c6")["port"])
        deadline = time.monotonic() + AGENT_START_S
        while time.monotonic() < deadline:
            if proxy_state(base_url, "c6")["state"] == "connected":
                break
            time.sleep(1)
        assert proxy_state(base_url, "c6")["state"] == "connected", (
            "otbr-agent never connected"
        )
        time.sleep(10)
        state = subprocess.run(
            ["docker", "exec", name, "ot-ctl", "state"],
            capture_output=True,
            text=True,
            check=False,
        )
        assert state.returncode == 0, state.stderr
        assert state.stdout.split()[0] in (
            "disabled",
            "detached",
            "child",
            "router",
            "leader",
        )
        counters = get_channel(base_url, "c6")["counters"]
        assert counters["frames_in"] > 0
        assert counters["frames_out"] > 0
    finally:
        put_channel(base_url, {"mode": "otbr"}, "c6")
        wait_mode(base_url, "BR_RUNNING", MOVE_TIMEOUT_S)
