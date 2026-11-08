"""Radio channel mux: /device/channels with the C6 alone (no dongle). Hardware-gated: written with Task 10 of the radio
channel mux plan as the contract, run in the batched on-board pass (Task 11).

Rig: the board alone as a border router. The tests that stop Thread bring the C6 back to `otbr` afterwards. A dongle left
plugged in does not matter; only the c6 channel is used.
"""

import socket
import time

import pytest
import requests
from conftest import ot, wait_ot_state

CHANNEL_KEYS = [
    "id",
    "kind",
    "label",
    "mode",
    "stored_mode",
    "state",
    "error",
    "line",
    "usb",
    "caps",
    "rcp_proxy",
    "firmware",
    "counters",
    "channel_state",
    "channel_reason",
    "mode_state",
    "mode_reason",
]
MODE_KEYS = ["mode", "channel", "state", "reason", "reboot_on_change"]
COUNTER_KEYS = [
    "frames_in",
    "frames_out",
    "bytes_in",
    "bytes_out",
    "drops",
    "resets",
    "rx_errors",
    "uart_errors",
]
PROXY_KEYS = [
    "direction",
    "adapter",
    "port",
    "host",
    "framing",
    "state",
    "peer",
    "last_error",
]
PROXY_DEFAULTS = {
    "direction": "listen",
    "adapter": "tcp",
    "port": 6638,
    "host": "",
    "framing": "hdlc",
}
OFFERED_MODES = {"off", "otbr", "rcp-proxy"}
NO_RADIO_DETAIL = "no radio in OTBR mode"
# spinel PROP_VALUE_GET PROTOCOL_VERSION (TID 1), HDLC framed with its FCS: a first frame a proxy client sends.
SPINEL_GET_PROTOCOL_VERSION_HDLC = bytes.fromhex("7e810201c5b27e")
STATUS_TIMEOUT_S = 90
SETTLE_TIMEOUT_S = 120  # a move, start or stop of Thread: drain, probe and start
THREAD_TIMEOUT_S = 120
PROXY_PORT = 6638
PROXY_PEER_WAIT_S = 10
MAX_BODY = 512
DATASET = "/node/dataset/active"
WS_HANDSHAKE = {
    "Connection": "Upgrade",
    "Upgrade": "websocket",
    "Sec-WebSocket-Version": "13",
    "Sec-WebSocket-Key": "dGhlIHNhbXBsZSBub25jZQ==",
}
# PUT bodies the C6 channel must refuse with 400 bad_value, and the detail that names the field.
BAD_VALUES = [
    ({"mode": "banana"}, "mode"),
    ({"mode": "unavailable"}, "mode"),
    ({"mode": 1}, "mode"),
    ({"baud": 9600}, "baud"),
    ({"baud": "460800"}, "baud"),
    (
        {"baud": 460800},
        None,
    ),  # the C6's line is fixed: refused by the channel, not by the parser
    ({"flow_control": "yes"}, "flow_control"),
    ({"rcp_proxy": "tcp"}, "rcp_proxy"),
    ({"rcp_proxy": {"port": 0}}, "rcp_proxy.port"),
    ({"rcp_proxy": {"port": 80}}, "rcp_proxy.port"),
    ({"rcp_proxy": {"port": 179}}, "rcp_proxy.port"),
    ({"rcp_proxy": {"port": 65536}}, "rcp_proxy.port"),
    ({"rcp_proxy": {"direction": "sideways"}}, "rcp_proxy.direction"),
    ({"rcp_proxy": {"adapter": "quic"}}, "rcp_proxy.adapter"),
    ({"rcp_proxy": {"framing": "base64"}}, "rcp_proxy.framing"),
    ({"rcp_proxy": {"direction": "connect"}}, "rcp_proxy.host"),
    ({"rcp_proxy": {"direction": "connect", "host": "bad host!"}}, "rcp_proxy.host"),
    ({"rcp_proxy": {"no_such_setting": 1}}, "rcp_proxy"),
    ({"no_such_key": 1}, "no_such_key"),
]


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


def get_channels(base_url):
    reply = requests.get(f"{base_url}/device/channels", timeout=10)
    assert reply.status_code == 200, reply.text
    return reply.json()


def get_channel(base_url, channel_id="builtin-c6"):
    for channel in get_channels(base_url)["channels"]:
        if channel["id"] == channel_id:
            return channel
    pytest.fail(f"no channel {channel_id!r} in the list")


def put_channel(base_url, body, channel_id="builtin-c6"):
    return requests.put(
        f"{base_url}/device/channels/{channel_id}", json=body, timeout=30
    )


def get_status(base_url):
    reply = requests.get(f"{base_url}/device/status", timeout=10)
    assert reply.status_code == 200, reply.text
    return reply.json()


def wait_mode(base_url, mode, timeout=STATUS_TIMEOUT_S):
    """Poll /device/status until its `mode` is `mode`; return the status."""
    deadline = time.monotonic() + timeout
    status = {}
    while time.monotonic() < deadline:
        try:
            status = get_status(base_url)
        except requests.RequestException:
            status = {}
        if status.get("mode") == mode:
            return status
        time.sleep(1)
    pytest.fail(f"board mode is {status.get('mode')!r}, expected {mode!r}")


def set_mode(base_url, mode, channel_id="builtin-c6", timeout=SETTLE_TIMEOUT_S):
    """PUT `mode` on the channel and wait until it settled; return the reply. A 200 applies at once. A 202 (a move,
    start or stop of Thread, a long operation) is waited for: until the channel stores the mode, and for otbr until
    Thread runs on it. Before otbr, a radio that runs rcp-proxy is set off first (the API refuses otbr while the proxy
    runs on it, "set it off first")."""
    if mode == "otbr" and get_channel(base_url, channel_id)["mode"] == "rcp-proxy":
        set_mode(base_url, "off", channel_id, timeout)
    reply = put_channel(base_url, {"mode": mode}, channel_id)
    assert reply.status_code in (200, 202), reply.text
    deadline = time.monotonic() + timeout
    while reply.status_code == 202 and time.monotonic() < deadline:
        with_thread = mode != "otbr" or (
            get_status(base_url)["otbr_channel"] == channel_id
            and get_status(base_url)["mode"] == "BR_RUNNING"
        )
        if get_channel(base_url, channel_id)["stored_mode"] == mode and with_thread:
            return reply
        time.sleep(1)
    if reply.status_code == 202:
        pytest.fail(f"{channel_id} did not settle in {mode}")
    return reply


@pytest.fixture
def c6_in_otbr(base_url):
    """After the test the C6 is in `otbr` again with its default proxy settings, and Thread runs on it."""
    yield
    put_channel(base_url, {"rcp_proxy": PROXY_DEFAULTS})
    if get_channel(base_url)["stored_mode"] != "otbr":
        set_mode(base_url, "otbr")
    wait_mode(base_url, "BR_RUNNING")


def test_c6_listed_in_otbr(base_url, board_ip):
    # GET /device/channels on a board without a dongle: the C6 is listed in OTBR mode, running, with its caps.
    body = get_channels(base_url)
    channel = next(c for c in body["channels"] if c["id"] == "builtin-c6")
    assert list(channel) == CHANNEL_KEYS
    assert channel["kind"] == "builtin"
    assert channel["mode"] == "otbr"
    assert channel["stored_mode"] == "otbr"
    assert channel["state"] == "running"
    assert channel["error"] is None
    assert channel["usb"] is None
    assert channel["caps"]["hw_reset"] is True
    assert channel["caps"]["firmware_update"] is True
    assert channel["firmware"]["type"] == "spinel"
    assert list(channel["counters"]) == COUNTER_KEYS
    assert channel["counters"]["frames_in"] > 0
    assert list(channel["rcp_proxy"]) == PROXY_KEYS
    assert {k: channel["rcp_proxy"][k] for k in PROXY_DEFAULTS} == PROXY_DEFAULTS
    assert channel["rcp_proxy"]["state"] is None
    assert channel["rcp_proxy"]["peer"] is None
    assert channel["rcp_proxy"]["last_error"] is None
    # The orchestrator's view: the channel ready, Thread's own state on it, and Thread in the modes list.
    assert channel["channel_state"] == "ready"
    assert channel["channel_reason"] is None
    assert channel["mode_state"] == "running"
    assert channel["mode_reason"] is None
    thread = [m for m in body["modes"] if m["mode"] == "otbr"]
    assert len(thread) == 1, body["modes"]
    assert list(thread[0]) == MODE_KEYS
    assert thread[0]["channel"] == "builtin-c6"
    assert thread[0]["state"] == "running"
    assert thread[0]["reboot_on_change"] is False
    status = get_status(base_url)
    assert status["mode"] == "BR_RUNNING"
    assert status["otbr_channel"] == "builtin-c6"


def test_old_c6_id_is_unknown(base_url):
    # PUT /device/channels/c6, On the id the built-in channel had before it was builtin-c6: 404 unknown_channel (no
    # alias); nothing changes.
    reply = put_channel(base_url, {"mode": "otbr"}, "c6")
    assert reply.status_code == 404, reply.text
    assert reply.json()["error"] == "unknown_channel"
    assert get_channels(base_url)["channels"][0]["id"] == "builtin-c6"


def test_available_modes(base_url):
    # GET /device/channels: available_modes comes first, offers off, otbr and rcp-proxy, and never ziggurat or unavailable.
    body = get_channels(base_url)
    assert list(body) == ["available_modes", "channels"]
    assert set(body["available_modes"]) == OFFERED_MODES
    assert len(body["available_modes"]) == len(OFFERED_MODES)
    for channel in body["channels"]:
        assert channel["stored_mode"] in body["available_modes"] + ["ziggurat"]


@pytest.mark.parametrize(("body", "detail"), BAD_VALUES)
def test_put_validation_400(base_url, body, detail):
    # PUT /device/channels/c6 with a wrong value, On any of the bad values: 400 bad_value, nothing stored.
    before = get_channel(base_url)
    reply = put_channel(base_url, body)
    assert reply.status_code == 400, (body, reply.text)
    assert reply.json()["error"] == "bad_value", reply.text
    if detail is not None:
        assert detail in reply.json()["detail"], reply.text
    assert without_counters(get_channel(base_url)) == without_counters(before)


def without_counters(channel):
    """The channel without its traffic counters, which Thread traffic moves at any time."""
    return {k: v for k, v in channel.items() if k != "counters"}


def test_put_validation_not_an_object(base_url):
    # PUT with a body that is no JSON object: 400 bad_value.
    for raw in (b"[1]", b'"otbr"', b"42", b"not json"):
        reply = requests.put(f"{base_url}/device/channels/c6", data=raw, timeout=10)
        assert reply.status_code == 400, (raw, reply.text)


def test_put_validation_body_size(base_url):
    # PUT with a body over 512 bytes: 413 payload_too_large.
    body = {"rcp_proxy": {"host": "h" * (MAX_BODY + 1)}}
    reply = put_channel(base_url, body)
    assert reply.status_code == 413, reply.text
    assert reply.json()["error"] == "payload_too_large"


def test_put_validation_unknown_channel(base_url):
    # PUT and DELETE on an id that does not exist: 404 unknown_channel.
    for channel_id in ("usb-0000-0000", "nothing", "x" * 80):
        reply = put_channel(base_url, {"mode": "off"}, channel_id)
        assert reply.status_code == 404, (channel_id, reply.text)
        assert reply.json()["error"] == "unknown_channel"
        gone = requests.delete(f"{base_url}/device/channels/{channel_id}", timeout=10)
        assert gone.status_code == 404, (channel_id, gone.text)
        assert gone.json()["error"] == "unknown_channel"


def test_put_validation_409(base_url):
    # PUT/DELETE refusals reachable with the C6 alone: not_available (ziggurat), in_use (forget the C6).
    before = get_channel(base_url)
    reply = put_channel(base_url, {"mode": "ziggurat"})
    assert reply.status_code == 409, reply.text
    assert reply.json()["error"] == "not_available"
    forget = requests.delete(f"{base_url}/device/channels/c6", timeout=10)
    assert forget.status_code == 409, forget.text
    assert forget.json()["error"] == "in_use"
    assert get_channel(base_url) == before
    # The refusals that need a second radio (unavailable, incompatible, probe_failed, busy, and a move carrying line
    # settings) are in test_dongle.py::test_move_refusals. mode_taken and hybrid_not_supported need a registered
    # ziggurat consumer and belong to the Zigbee work.


def test_put_validation_c6_busy(base_url, c6_in_otbr):
    # POST /device/radio/recover, On rcp-proxy with a connected peer on the C6: 409 c6_busy, the peer stays.
    set_mode(
        base_url, "rcp-proxy"
    )  # a stop of Thread on the C6 (202), then the proxy listens
    host = base_url.removeprefix("http://")
    with socket.create_connection(
        (host, PROXY_PORT), timeout=PROXY_PEER_WAIT_S
    ) as client:
        # A TCP client becomes the peer only once its first bytes pass the proxy's HTTP check (a silent one never
        # does): send a spinel PROP_VALUE_GET PROTOCOL_VERSION, HDLC framed, as a host would.
        client.sendall(SPINEL_GET_PROTOCOL_VERSION_HDLC)
        deadline = time.monotonic() + PROXY_PEER_WAIT_S
        while get_channel(base_url)["rcp_proxy"]["peer"] is None:
            assert time.monotonic() < deadline, "the proxy never saw the peer"
            time.sleep(0.5)
        recover = requests.post(f"{base_url}/device/radio/recover", timeout=30)
        # c6_in_otbr restores the C6 after this skip as well.
        if recover.status_code == 412 and recover.json().get("error") == "no_jumper":
            pytest.skip(
                "the C6 maintenance jumper is not fitted: c6_busy can't be reached"
            )
        assert recover.status_code == 409, recover.text
        assert recover.json()["error"] == "c6_busy"
        assert get_channel(base_url)["rcp_proxy"]["peer"] is not None


def test_put_reply_is_the_channel(base_url, c6_in_otbr):
    # PUT /device/channels/c6 with a proxy setting, On success: 200 with the channel, stored; no reboot.
    reply = put_channel(base_url, {"rcp_proxy": {"port": 6639}})
    assert reply.status_code == 200, reply.text
    body = reply.json()
    assert list(body) == CHANNEL_KEYS
    assert body["rcp_proxy"]["port"] == 6639
    assert "reboot" not in body
    assert get_channel(base_url)["rcp_proxy"]["port"] == 6639
    assert get_channel(base_url)["mode"] == "otbr"


def test_c6_off_and_back(dut, base_url, c6_in_otbr):
    # PUT /device/channels/c6 off and back, On the C6 only: Thread stops, NO_RADIO, and the same dataset after it returns.
    wait_mode(base_url, "BR_RUNNING")
    # The board's own network, whatever it is: never form a throwaway one.
    before = ot(dut, "dataset active -x")
    if not before or not before[-1].isalnum():
        pytest.skip("the board has no active dataset")
    dataset = before[-1]
    off = set_mode(
        base_url, "off"
    )  # a stop of Thread: 202, the mode stored once OpenThread stopped
    assert off.status_code == 202, off.text
    assert off.json() == {"moving": True, "moved_from": "builtin-c6"}
    status = wait_mode(base_url, "NO_RADIO")
    assert status["otbr_channel"] is None
    channel = get_channel(base_url)
    assert channel["mode"] == "off"
    assert channel["stored_mode"] == "off"
    assert channel["state"] == "idle"
    assert channel["mode_state"] is None  # nothing is bound to it
    thread = next(m for m in get_channels(base_url)["modes"] if m["mode"] == "otbr")
    assert thread["channel"] is None
    assert thread["state"] == "no_radio"
    assert thread["reason"] == "no channel in otbr"
    on = set_mode(
        base_url, "otbr"
    )  # a start of Thread: 202, probe, start, stored once it runs
    assert on.status_code == 202, on.text
    assert on.json() == {"moving": True, "moved_from": None}
    status = wait_mode(base_url, "BR_RUNNING")
    assert status["otbr_channel"] == "builtin-c6"
    assert get_channel(base_url)["state"] == "running"
    assert get_channel(base_url)["mode_state"] == "running"
    wait_ot_state(dut, {"leader", "router", "child"}, THREAD_TIMEOUT_S)
    assert ot(dut, "dataset active -x")[-1] == dataset


def test_ziggurat_not_available(base_url):
    # PUT mode ziggurat, On a build without a Zigbee consumer: 409 not_available, mode unchanged, not offered.
    before = get_channel(base_url)
    reply = put_channel(base_url, {"mode": "ziggurat"})
    assert reply.status_code == 409, reply.text
    assert reply.json()["error"] == "not_available"
    assert "ziggurat" not in get_channels(base_url)["available_modes"]
    assert get_channel(base_url)["mode"] == before["mode"]
    assert get_channel(base_url)["stored_mode"] == before["stored_mode"]


def test_rest_503_in_no_radio(base_url, c6_in_otbr):
    # Thread REST, Web GUI and console WebSocket, On NO_RADIO: 503 with the detail "no radio in OTBR mode".
    console_was = requests.get(f"{base_url}/device/console", timeout=10).json()[
        "enabled"
    ]
    requests.put(f"{base_url}/device/console", json={"enabled": True}, timeout=10)
    try:
        set_mode(base_url, "off")
        wait_mode(base_url, "NO_RADIO")
        for path in (DATASET, "/node/state", "/node"):
            reply = requests.get(base_url + path, timeout=10)
            assert reply.status_code == 503, (path, reply.status_code)
            assert reply.json()["detail"] == NO_RADIO_DETAIL, path
        write = requests.put(
            base_url + DATASET, json={"networkName": "nobody"}, timeout=10
        )
        assert write.status_code == 503, write.text
        assert write.json()["detail"] == NO_RADIO_DETAIL
        socket_reply = requests.get(
            f"{base_url}/device/console/ws", headers=WS_HANDSHAKE, timeout=10
        )
        assert socket_reply.status_code == 503, socket_reply.status_code
        assert socket_reply.json() == {"error": "no_radio", "detail": NO_RADIO_DETAIL}
        # What does not need OpenThread keeps answering.
        assert requests.get(f"{base_url}/index.html", timeout=10).status_code == 200
        assert get_status(base_url)["mode"] == "NO_RADIO"
    finally:
        requests.put(
            f"{base_url}/device/console", json={"enabled": console_was}, timeout=10
        )
