"""Logging: GET/PUT /device/logging, the live log WebSocket, syslog and the `log` console command (spec
docs/superpowers/specs/2026-11-10-logging-design.md). Hardware-gated: run in the batched on-board pass.

Rig: the board alone, running as a border router with a stored dataset (the live lines come from OpenThread at debug
level). The syslog test needs the test host reachable from the board over UDP (no firewall on the listener's port);
the 403 test needs the VLAN rig (-m vlan_switch). Every test puts the logging settings and the Thread console's
setting back as it found them. No test erases NVS: the board keeps the Thread dataset of the shared rig.
"""

import asyncio
import contextlib
import json
import re
import socket
import struct
import textwrap
import time

import aiohttp
import pexpect
import pytest
import requests
from conftest import BOOT_TIMEOUT_S, GOT_IP, ROM_BANNER, wait_ot_state, wait_reachable
from test_vlan import (  # noqa: F401  (fixtures of the VLAN rig)
    docker_run,
    link_local,
    rig,
    sh,
    split_board,
    to_split_trial,
)

RUNNING_ROLES = ("leader", "router", "child")
STATE_TIMEOUT_S = 120
LINE_WAIT_S = 20
SLOW_IDLE_S = 20
SLOW_READ_S = 8
NO_ANSWER_CLOSE_S = 50
SYSLOG_WAIT_S = 20
UNRESOLVED_WAIT_S = 60
UART_QUIET_S = 10
UART_STORM_LINES = 3
# A UART line of the logging component or its syslog lookup.
LOGGING_LINE = re.compile(
    r"^[EWIDV] \(\d+\) (logging|log_dns|log_out)[^:]*:|syslog", re.IGNORECASE
)
BODY_MAX = 4096
VIEWERS_MAX = 2
DEFAULTS = {
    "channels": {
        "uart": {"level": "info"},
        "jtag": {"level": "info"},
        "syslog": {"level": "off", "target": ""},
    },
    "components": {"*": "info"},
}
POKE_EVERY_S = 3
OT_LINES_WANTED = 3
OT_WAIT_S = 90
CONSOLE_END = re.compile(r"^(Done|Error \d+: .*)$", re.MULTILINE)
# What the Thread console logs at info for every command: tag ot_console, the first word and the length only.
TRIGGER = re.compile(r"^I \(\d+\) ot_console: console .+: state \(\d+ bytes\)$")
LINE = re.compile(r"^[EWIDV] \(\d+\) [^\s:]+: .+$")
OT_LINE = re.compile(r"^[EWIDV] \(\d+\) OPENTHREAD: .+$")
# RFC 5424: <PRI>1 TIMESTAMP HOSTNAME APP-NAME PROCID MSGID STRUCTURED-DATA MSG; the board sends "-" for the unused
# ones, "-" as TIMESTAMP until the clock is set, and starts MSG with the uptime in ms.
SYSLOG = re.compile(
    r"^<(\d{1,3})>1 (-|\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z) (\S+) (\S+) - - - (\d+) (.+)$"
)
UPGRADE_SNIPPET = textwrap.dedent("""
    import json, socket, sys
    host, path = sys.argv[1:3]
    head = f"GET {path} HTTP/1.1\\r\\nHost: board\\r\\nConnection: Upgrade\\r\\nUpgrade: websocket\\r\\n"
    head += "Sec-WebSocket-Version: 13\\r\\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\\r\\n\\r\\n"
    try:
        sock = socket.create_connection((host, 80), timeout=4)
        sock.sendall(head.encode())
        sock.settimeout(3)
        data = b""
        try:
            while chunk := sock.recv(4096):
                data += chunk
        except socket.timeout:
            pass
        print(json.dumps({"status": int(data.split()[1]) if data else None}))
    except OSError as error:
        print(json.dumps({"status": None, "error": str(error)}))
""")


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


@pytest.fixture
def ws_url(board_ip):
    return f"ws://{board_ip}/device/logging/ws"


def settings_body(state):
    """The PUT body that sets what a GET reply shows (its stored part: levels, target, components)."""
    channels = state["channels"]
    return {
        "channels": {
            "uart": {"level": channels["uart"]["level"]},
            "jtag": {"level": channels["jtag"]["level"]},
            "syslog": {
                "level": channels["syslog"]["level"],
                "target": channels["syslog"]["target"],
            },
        },
        "components": state["components"],
    }


@pytest.fixture
def logging_settings(base_url):
    """put(body): PUT /device/logging, expect 200, return the reply. The settings the test found (read before it)
    are put back afterwards, whatever they were: the board is shared."""
    r = requests.get(f"{base_url}/device/logging", timeout=10)
    assert r.status_code == requests.codes.ok, r.text
    prior = settings_body(r.json())

    def put(body):
        r = requests.put(f"{base_url}/device/logging", json=body, timeout=10)
        assert r.status_code == requests.codes.ok, r.text
        return r.json()

    yield put
    with contextlib.suppress(requests.RequestException):
        requests.put(f"{base_url}/device/logging", json=prior, timeout=10)


@pytest.fixture
def console_ws(base_url, board_ip):
    """The Thread console turned on for the test (afterwards back to on or off, as the test found it), as the URL of
    its WebSocket.

    It is the deterministic line source of these tests: every command sent over it is logged by the board at info
    under the tag `ot_console` ("console <peer>: <first word> (<n> bytes)", ot_console.c), the moment it arrives.
    """
    r = requests.get(f"{base_url}/device/console", timeout=10)
    assert r.status_code == requests.codes.ok, r.text
    prior = r.json()["enabled"]
    r = requests.put(f"{base_url}/device/console", json={"enabled": True}, timeout=10)
    assert r.status_code == requests.codes.ok, r.text
    yield f"ws://{board_ip}/device/console/ws"
    with contextlib.suppress(requests.RequestException):
        requests.put(f"{base_url}/device/console", json={"enabled": prior}, timeout=10)


async def poke(session, console_url, command="state"):
    """Send one OpenThread CLI command over the console WebSocket and wait for its Done: the board logs
    `I (..) ot_console: console <peer>: <command> (<n> bytes)` (TRIGGER for `state`)."""
    ws = await session.ws_connect(console_url)
    await ws.send_str(command)
    text = ""
    deadline = time.monotonic() + 15
    while not CONSOLE_END.search(text) and time.monotonic() < deadline:
        msg = await asyncio.wait_for(ws.receive(), 15)
        if msg.type == aiohttp.WSMsgType.TEXT and not msg.data.startswith("# "):
            text += msg.data.replace("\r", "")
    await ws.close()


async def gather_until(viewers, done, poker, timeout=LINE_WAIT_S):
    """Frames per viewer, polled in 1 s slices: `poker()` is awaited again every POKE_EVERY_S, until
    `done(frames_per_viewer)` holds or `timeout` ran out. One more slice follows `done`, so that a line that should
    not arrive has had its time. Returns the lists (the caller asserts that `done` really held)."""
    frames = [[] for _ in viewers]
    deadline = time.monotonic() + timeout
    next_poke, finished = 0.0, False
    while time.monotonic() < deadline:
        if time.monotonic() >= next_poke:
            await poker()
            next_poke = time.monotonic() + POKE_EVERY_S
        slices = await asyncio.gather(*(collect(ws, 1) for ws in viewers))
        for mine, new in zip(frames, slices, strict=True):
            mine.extend(new)
        if finished:
            break
        finished = done(frames)
    return frames


@pytest.fixture
def ot_running(dut):
    """Thread is up: it is the one reliable source of lines (OpenThread at debug)."""
    wait_ot_state(dut, RUNNING_ROLES, STATE_TIMEOUT_S)


def get_logging(base_url):
    r = requests.get(f"{base_url}/device/logging", timeout=10)
    assert r.status_code == requests.codes.ok, r.text
    return r.json()


def stored(state):
    """The part of a GET reply that PUT sets (the rest is status)."""
    channels = state["channels"]
    return {
        "uart": channels["uart"]["level"],
        "jtag": channels["jtag"]["level"],
        "syslog": (channels["syslog"]["level"], channels["syslog"]["target"]),
        "components": state["components"],
    }


def host_address_towards(board):
    """The address of this host that the board reaches (for connect-direction tests)."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.connect((board, 80))
        return probe.getsockname()[0]


def upgrade_headers():
    return {
        "Connection": "Upgrade",
        "Upgrade": "websocket",
        "Sec-WebSocket-Version": "13",
        "Sec-WebSocket-Key": "dGhlIHNhbXBsZSBub25jZQ==",
    }


async def collect(ws, seconds):
    """Text frames until `seconds` passed or the socket closed (ws.receive's own timeout restarts on pings)."""
    frames = []
    deadline = time.monotonic() + seconds
    while (left := deadline - time.monotonic()) > 0:
        try:
            msg = await asyncio.wait_for(ws.receive(), left)
        except TimeoutError:
            break
        if msg.type == aiohttp.WSMsgType.TEXT:
            frames.append(msg.data)
        elif msg.type != aiohttp.WSMsgType.PING:
            break
    return frames


async def wait_viewers(base_url, count, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        state = await asyncio.to_thread(get_logging, base_url)
        if state["viewers"] == count:
            return
        await asyncio.sleep(0.5)
    pytest.fail(f"viewers never became {count}")


class RawViewer:
    """A live log client on a plain socket with a tiny receive buffer: it can really stop reading."""

    def __init__(self, board_ip, level):
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
        self.sock.settimeout(5)
        self.sock.connect((board_ip, 80))
        request = (
            f"GET /device/logging/ws?level={level} HTTP/1.1\r\nHost: board\r\n"
            + "".join(f"{k}: {v}\r\n" for k, v in upgrade_headers().items())
            + "\r\n"
        )
        self.sock.sendall(request.encode())
        data = b""
        while b"\r\n\r\n" not in data:
            data += self.sock.recv(1024)
        head, _, self.buffer = data.partition(b"\r\n\r\n")
        assert head.split()[1] == b"101", head

    def frames(self, seconds):
        """(opcode, payload) of the frames that arrive within `seconds`; pings are answered."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            frame = self._parse()
            if frame is None:
                try:
                    chunk = self.sock.recv(4096)
                except TimeoutError:
                    continue
                if not chunk:
                    return
                self.buffer += chunk
                continue
            opcode, payload = frame
            if opcode == 9:  # ping: pong with a zero mask key (payload unchanged)
                self.sock.sendall(
                    bytes([0x8A, 0x80 | len(payload)]) + bytes(4) + payload
                )
            yield opcode, payload

    def _parse(self):
        buf = self.buffer
        if len(buf) < 2:
            return None
        length, offset = buf[1] & 0x7F, 2
        if length == 126:
            if len(buf) < 4:
                return None
            length, offset = struct.unpack(">H", buf[2:4])[0], 4
        elif length == 127:
            if len(buf) < 10:
                return None
            length, offset = struct.unpack(">Q", buf[2:10])[0], 10
        if len(buf) < offset + length:
            return None
        self.buffer = buf[offset + length :]
        return buf[0] & 0x0F, buf[offset : offset + length]

    def close(self):
        self.sock.close()


class TestSettings:
    """device_api GET/PUT /device/logging"""

    def test_get_defaults(self, dut, base_url, logging_settings):
        # The NVS is not erased (the board keeps its Thread dataset): the documented defaults are put, GET gives them
        # back in the documented shape and they survive a reboot. A fresh board's defaults: host test (logging_init).
        logging_settings(DEFAULTS)
        state = get_logging(base_url)
        assert stored(state) == {
            "uart": "info",
            "jtag": "info",
            "syslog": ("off", ""),
            "components": {"*": "info"},
        }
        # make test-target runs a build-test image: every level compiled in.
        assert state["max_level"] == "verbose"
        assert state["channels"]["syslog"]["state"] == "off"
        assert isinstance(state["channels"]["jtag"]["host"], bool)
        assert isinstance(state["known_tags"], list)
        assert state["viewers"] == 0
        assert set(state["counters"]) == {"uart", "jtag", "syslog", "live"}
        for pair in state["counters"].values():
            assert set(pair) == {"sent", "dropped"}
        assert (
            requests.post(f"{base_url}/device/reboot", timeout=10).status_code
            == requests.codes.ok
        )
        dut.expect(ROM_BANNER, timeout=BOOT_TIMEOUT_S)
        url = "http://" + dut.expect(GOT_IP, timeout=BOOT_TIMEOUT_S).group(1).decode()
        assert stored(get_logging(url)) == stored(state)

    def test_put_subset_and_persist(self, dut, board_ip, base_url, logging_settings):
        # On a valid subset: 200 with the GET body; the settings survive a reboot.
        reply = logging_settings(
            {
                "channels": {"uart": {"level": "warning"}},
                "components": {"*": "error", "ziggurat": "debug"},
            }
        )
        assert stored(get_logging(base_url)) == stored(reply)
        assert stored(reply) == {
            "uart": "warning",
            "jtag": "info",
            "syslog": ("off", ""),
            "components": {"*": "error", "ziggurat": "debug"},
        }
        assert (
            requests.post(f"{base_url}/device/reboot", timeout=10).status_code
            == requests.codes.ok
        )
        dut.expect(ROM_BANNER, timeout=BOOT_TIMEOUT_S)
        # The UART is at warning now, so the boot's `eth: got ip` (info) is not printed: wait for the same address.
        wait_reachable(board_ip, timeout=BOOT_TIMEOUT_S)
        assert stored(get_logging(base_url)) == stored(reply)

    def test_put_replaces_the_override_set(self, base_url, logging_settings):
        # On components: the object replaces the overrides; "*" is the default.
        logging_settings(
            {"components": {"*": "info", "ziggurat": "debug", "wifi": "error"}}
        )
        reply = logging_settings({"components": {"*": "warning", "ziggurat": "info"}})
        assert reply["components"] == {"*": "warning", "ziggurat": "info"}
        reply = logging_settings({"channels": {"jtag": {"level": "error"}}})
        assert reply["components"] == {"*": "warning", "ziggurat": "info"}

    def test_put_ignores_unknown_keys(self, base_url, logging_settings):
        # On what GET gave: PUT takes the reply back and ignores the read-only parts.
        state = get_logging(base_url)
        reply = logging_settings(state)
        assert stored(reply) == stored(state)
        logging_settings(
            {"colour": "blue", "channels": {"uart": {"level": "info", "x": 1}}}
        )

    @pytest.mark.parametrize(
        ("body", "detail"),
        [
            ({"channels": {"uart": {"level": "loud"}}}, "channels.uart.level"),
            ({"channels": {"jtag": {"level": 3}}}, "channels.jtag.level"),
            ({"channels": {"syslog": {"level": "warn"}}}, "channels.syslog.level"),
            ({"channels": {"syslog": {"target": "host:0"}}}, "channels.syslog.target"),
            (
                {"channels": {"syslog": {"target": "host:65536"}}},
                "channels.syslog.target",
            ),
            ({"channels": {"syslog": {"target": 5}}}, "channels.syslog.target"),
            ({"channels": {"syslog": {"level": "info"}}}, "channels.syslog.target"),
            ({"channels": 5}, "channels"),
            ({"channels": {"uart": 5}}, "channels.uart"),
            ({"components": {"ziggurat": "loud"}}, "components.ziggurat"),
            ({"components": {"*": "loud"}}, "components.*"),
            ({"components": {"x" * 24: "info"}}, f"components.{'x' * 24}"),
            ({"components": {'a"b': "info"}}, 'components.a"b'),
            ({"components": {"a b": "info"}}, "components.a b"),
            ({"components": {f"t{i}": "info" for i in range(33)}}, "components"),
            ({"components": ["ziggurat"]}, "components"),
            ([], "body"),
        ],
    )
    def test_put_bad_fields(self, base_url, logging_settings, body, detail):
        # On a bad level, target, tag or shape: 400 invalid_request, detail names the field, nothing changes.
        before = stored(get_logging(base_url))
        r = requests.put(f"{base_url}/device/logging", json=body, timeout=10)
        assert r.status_code == requests.codes.bad_request, r.text
        assert r.json()["error"] == "invalid_request"
        assert r.json()["detail"] == detail
        assert stored(get_logging(base_url)) == before

    def test_put_duplicate_tag_is_refused(self, base_url):
        # On a tag listed twice in components: 400 components.<tag>.
        r = requests.put(
            f"{base_url}/device/logging",
            data='{"components":{"ziggurat":"info","ziggurat":"debug"}}',
            headers={"Content-Type": "application/json"},
            timeout=10,
        )
        assert r.status_code == requests.codes.bad_request, r.text
        assert r.json()["detail"] == "components.ziggurat"

    def test_put_not_json_is_refused(self, base_url):
        # On a body that is not JSON: 400 invalid_request.
        r = requests.put(f"{base_url}/device/logging", data=b"not json", timeout=10)
        assert r.status_code == requests.codes.bad_request, r.text
        assert r.json()["error"] == "invalid_request"

    def test_put_over_4kb_is_413(self, base_url):
        # On a body over 4 KB: 413 payload_too_large.
        r = requests.put(
            f"{base_url}/device/logging", data=b" " * (BODY_MAX + 1), timeout=10
        )
        assert r.status_code == requests.codes.request_entity_too_large, r.text
        assert r.json()["error"] == "payload_too_large"

    def test_put_without_content_length_is_400(self, base_url):
        # On a chunked body (no Content-Length): 400 length_required.
        r = requests.put(
            f"{base_url}/device/logging",
            data=(chunk for chunk in (b"{}",)),
            timeout=10,
        )
        assert r.status_code == requests.codes.bad_request, r.text
        assert r.json()["error"] == "length_required"


class TestLiveLog:
    """device_api /device/logging/ws"""

    def test_ws_connect_with_level_is_101(self, ws_url, base_url):
        # On connect with ?level=debug: ws_connect succeeds (it needs a 101) and the viewer is counted; a bad level is refused with 400.
        async def go():
            async with aiohttp.ClientSession() as session:
                ws = await session.ws_connect(f"{ws_url}?level=debug")
                await wait_viewers(base_url, 1)
                await ws.close()
                with pytest.raises(aiohttp.WSServerHandshakeError) as refused:
                    await session.ws_connect(f"{ws_url}?level=loud")
                assert refused.value.status == requests.codes.bad_request
                await wait_viewers(base_url, 0)

        asyncio.run(go())

    def test_ws_lines_after_connect(self, ws_url, console_ws, ot_running):
        # On connect: text frames "<L> (<uptime>) <tag>: <body>", one line each, uptime never going backwards, and
        # only lines logged after the connection.
        async def go():
            async with aiohttp.ClientSession() as session:
                await poke(
                    session, console_ws, "version"
                )  # logged before the viewer exists
                ws = await session.ws_connect(f"{ws_url}?level=info")
                (frames,) = await gather_until(
                    [ws],
                    lambda f: any(TRIGGER.match(x) for x in f[0]),
                    lambda: poke(session, console_ws),
                )
                await ws.close()
                return frames

        frames = [f for f in asyncio.run(go()) if not f.startswith("# ")]
        assert any(TRIGGER.match(f) for f in frames), frames
        assert all(LINE.match(f) for f in frames), [
            f for f in frames if not LINE.match(f)
        ]
        assert not [f for f in frames if "console" in f and ": version (" in f]
        uptimes = [int(re.match(r"\w \((\d+)\)", f).group(1)) for f in frames]
        assert uptimes == sorted(uptimes)

    def test_ws_per_viewer_level(self, ws_url, console_ws, ot_running):
        # On viewers at info and warning, with the info line TRIGGER provoked: the info viewer gets it, the warning
        # viewer gets no info, debug or verbose line (the board has no on-demand debug line, so info is the line
        # that tells the levels apart); a level frame moves one viewer only.
        async def go():
            async with aiohttp.ClientSession() as session:
                deep = await session.ws_connect(f"{ws_url}?level=info")
                shallow = await session.ws_connect(f"{ws_url}?level=warning")

                def poker():
                    return poke(session, console_ws)

                deep_lines, shallow_lines = await gather_until(
                    [deep, shallow],
                    lambda f: any(TRIGGER.match(x) for x in f[0]),
                    poker,
                )
                await shallow.send_str('{"level":"info"}')
                (raised,) = await gather_until(
                    [shallow],
                    lambda f: any(TRIGGER.match(x) for x in f[0]),
                    poker,
                )
                await shallow.send_str('{"level":"loud"}')
                notices = await collect(shallow, 3)
                await asyncio.gather(deep.close(), shallow.close())
                return deep_lines, shallow_lines, raised, notices

        deep, shallow, raised, notices = asyncio.run(go())
        assert any(TRIGGER.match(f) for f in deep), deep
        assert [f for f in shallow if f[:2] in ("I ", "D ", "V ")] == []
        assert any(TRIGGER.match(f) for f in raised), raised
        assert any(n.startswith("# ignored") for n in notices)

    def test_ws_third_viewer_refused(self, ws_url, board_ip, base_url):
        # On a third viewer (two are allowed): 503 "too many viewers" before the upgrade; a freed slot is usable.
        async def go():
            async with aiohttp.ClientSession() as session:
                first = await session.ws_connect(ws_url)
                second = await session.ws_connect(ws_url)
                await wait_viewers(base_url, VIEWERS_MAX)
                with pytest.raises(aiohttp.WSServerHandshakeError) as refused:
                    await session.ws_connect(ws_url)
                assert refused.value.status == requests.codes.service_unavailable
                plain = await asyncio.to_thread(
                    requests.get,
                    f"http://{board_ip}/device/logging/ws",
                    headers=upgrade_headers(),
                    timeout=10,
                )
                assert plain.status_code == requests.codes.service_unavailable
                assert plain.json()["detail"] == "too many viewers"
                await first.close()
                await wait_viewers(base_url, 1)
                third = await session.ws_connect(ws_url)
                await asyncio.gather(second.close(), third.close())
                await wait_viewers(base_url, 0)

        asyncio.run(go())

    def test_ws_viewer_that_ignores_pings_is_closed_1001(self, ws_url, base_url):
        # On a viewer that does not answer pings for 30 s: closed 1001, the slot is freed.
        async def go():
            async with aiohttp.ClientSession() as session:
                ws = await session.ws_connect(ws_url, autoping=False)
                start = time.monotonic()
                while True:
                    msg = await ws.receive(timeout=NO_ANSWER_CLOSE_S + 5)
                    if msg.type in (
                        aiohttp.WSMsgType.CLOSE,
                        aiohttp.WSMsgType.CLOSED,
                        aiohttp.WSMsgType.CLOSING,
                    ):
                        break
                elapsed = time.monotonic() - start
                await wait_viewers(base_url, 0)
                return elapsed, ws.close_code, msg.extra

        elapsed, code, reason = asyncio.run(go())
        assert code == 1001 and reason == "no answer"
        assert elapsed < NO_ANSWER_CLOSE_S

    def test_slow_viewer_drops(self, board_ip, ws_url, ot_running, logging_settings):
        # On a viewer that reads nothing for 20 s under `*` at debug: it gets a "# dropped" frame once it reads
        # again; the viewer that kept reading gets none.
        logging_settings({"components": {"*": "debug"}})
        slow = RawViewer(board_ip, "debug")

        async def go():
            async with aiohttp.ClientSession() as session:
                fast = await session.ws_connect(f"{ws_url}?level=debug")
                lines = await collect(fast, SLOW_IDLE_S)
                await fast.close()
                return lines

        try:
            fast_frames = asyncio.run(go())
            notices = [
                payload.decode()
                for opcode, payload in slow.frames(SLOW_READ_S)
                if opcode == 1 and payload.startswith(b"# dropped ")
            ]
        finally:
            slow.close()
        assert notices, "the slow viewer was never told about drops"
        assert re.fullmatch(r"# dropped \d+ lines", notices[0])
        assert not [f for f in fast_frames if f.startswith("# dropped")]

    def test_ot_line_is_whole(self, ws_url, console_ws, ot_running, logging_settings):
        # On an OPENTHREAD debug viewer with nothing else enabled: every frame is one whole OpenThread line. The
        # console `ping ff02::1` makes OpenThread send (and log) packets at once; its own timers (MLE advertisements)
        # keep it logging, so the wait polls up to OT_WAIT_S for OT_LINES_WANTED lines.
        logging_settings({"components": {"*": "off", "OPENTHREAD": "debug"}})

        async def go():
            async with aiohttp.ClientSession() as session:
                ws = await session.ws_connect(f"{ws_url}?level=debug")
                (frames,) = await gather_until(
                    [ws],
                    lambda f: (
                        len([x for x in f[0] if not x.startswith("# ")])
                        >= OT_LINES_WANTED
                    ),
                    lambda: poke(session, console_ws, "ping ff02::1"),
                    OT_WAIT_S,
                )
                await ws.close()
                return frames

        frames = [f for f in asyncio.run(go()) if not f.startswith("# ")]
        assert len(frames) >= OT_LINES_WANTED, f"OpenThread logged {len(frames)} lines"
        assert all(OT_LINE.match(f) for f in frames), [
            f for f in frames if not OT_LINE.match(f)
        ]

    def test_no_viewer_no_cost(self, base_url, console_ws, ot_running):
        # On lines produced with no viewer: the UART counts them (so lines were made), the live channel counts nothing.
        deadline = time.monotonic() + 15
        while get_logging(base_url)["viewers"] and time.monotonic() < deadline:
            time.sleep(0.5)
        before = get_logging(base_url)
        assert before["viewers"] == 0

        async def go():
            async with aiohttp.ClientSession() as session:
                deadline = time.monotonic() + LINE_WAIT_S
                while time.monotonic() < deadline:
                    await poke(session, console_ws)
                    now = await asyncio.to_thread(get_logging, base_url)
                    if (
                        now["counters"]["uart"]["sent"]
                        > before["counters"]["uart"]["sent"]
                    ):
                        return now
                    await asyncio.sleep(1)
                return now

        after = asyncio.run(go())
        assert after["counters"]["uart"]["sent"] > before["counters"]["uart"]["sent"], (
            "no lines were produced"
        )
        assert after["counters"]["live"]["sent"] == before["counters"]["live"]["sent"]
        assert (
            after["counters"]["live"]["dropped"]
            == before["counters"]["live"]["dropped"]
        )
        assert after["viewers"] == 0

    @pytest.mark.vlan_switch
    def test_ws_is_refused_with_403_where_the_device_api_is_not_served(
        self,
        split_board,  # noqa: F811
    ):
        # On an upgrade arriving on thread_bb (split mode): 403, as /device/console/ws.
        host = link_local(split_board, "thread_bb")
        result = docker_run(
            "thread", sh(UPGRADE_SNIPPET, host, "/device/logging/ws?level=debug")
        )
        assert result.returncode == 0, result.stderr
        reply = json.loads(result.stdout.strip().splitlines()[-1])
        assert reply["status"] == requests.codes.forbidden, reply


class TestSyslog:
    """logging syslog channel"""

    def test_syslog_receives(
        self, dut, board_ip, base_url, ot_running, logging_settings
    ):
        # On a target and a level: RFC 5424 datagrams arrive, facility local0, APP-NAME the tag, severity from level.
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
            listener.bind((host_address_towards(board_ip), 0))
            listener.settimeout(1)
            host, port = listener.getsockname()
            target = f"{host}:{port}"
            reply = logging_settings(
                {
                    "channels": {"syslog": {"level": "debug", "target": target}},
                    "components": {"*": "info", "OPENTHREAD": "debug"},
                }
            )
            assert reply["channels"]["syslog"]["target"] == target
            datagrams = []
            deadline = time.monotonic() + SYSLOG_WAIT_S
            while time.monotonic() < deadline and len(datagrams) < 20:
                with contextlib.suppress(TimeoutError):
                    datagrams.append(listener.recv(2048).decode())
        assert datagrams, "no datagram arrived (firewall on the test host?)"
        parsed = [SYSLOG.match(d) for d in datagrams]
        assert all(parsed), [d for d, m in zip(datagrams, parsed, strict=True) if not m]
        for m in parsed:
            pri = int(m.group(1))
            assert pri >> 3 == 16, pri  # local0
            assert pri & 7 in (3, 4, 6, 7), pri
        assert "OPENTHREAD" in {m.group(4) for m in parsed}
        assert {int(m.group(1)) & 7 for m in parsed if m.group(4) == "OPENTHREAD"} <= {
            3,
            4,
            6,
            7,
        }
        assert get_logging(base_url)["channels"]["syslog"]["state"] == "sending"

    def test_syslog_unresolvable_target(self, dut, base_url, logging_settings):
        # On a name that does not resolve: state unresolved, the board answers, and the UART shows no storm of logging's
        # own lines (OpenThread's usual info chatter, ~10 lines a second after a boot, is not counted).
        logging_settings(
            {"channels": {"syslog": {"level": "info", "target": "nonexistent.invalid"}}}
        )
        deadline = time.monotonic() + UNRESOLVED_WAIT_S
        state = None
        while time.monotonic() < deadline:
            state = get_logging(base_url)["channels"]["syslog"]["state"]
            if state == "unresolved":
                break
            time.sleep(1)
        assert state == "unresolved"
        assert (
            requests.get(f"{base_url}/device/status", timeout=10).status_code
            == requests.codes.ok
        )
        lines, deadline = [], time.monotonic() + UART_QUIET_S
        while time.monotonic() < deadline:
            try:
                line = (
                    dut.expect(rb"[^\n]*\n", timeout=1)
                    .group(0)
                    .decode(errors="replace")
                )
            except pexpect.TIMEOUT:
                continue
            if LOGGING_LINE.search(line):
                lines.append(line.strip())
        assert len(lines) < UART_STORM_LINES, lines
        assert get_logging(base_url)["channels"]["syslog"]["state"] == "unresolved"


class TestConsoleCommand:
    """logging_console log"""

    def test_console_log_command(self, send, base_url, logging_settings):
        # On `log`: the table is printed. On each form: applied like PUT. On bad input: usage, nothing changed.
        send("log", rb"log: channel +level +sent +dropped")
        send("log", rb"log: default \(everything else\): info")

        send("log level uart warning", rb"log: done")
        assert get_logging(base_url)["channels"]["uart"]["level"] == "warning"

        send("log tag ziggurat debug", rb"log: done")
        assert get_logging(base_url)["components"] == {"*": "info", "ziggurat": "debug"}
        send("log", rb"log: override ziggurat: debug")

        send("log tag * warning", rb"log: done")
        assert get_logging(base_url)["components"]["*"] == "warning"

        send("log tag ziggurat default", rb"log: done")
        assert get_logging(base_url)["components"] == {"*": "warning"}

        send("log syslog 10.0.0.2", rb"log: syslog level is still off")
        syslog = get_logging(base_url)["channels"]["syslog"]
        assert (syslog["target"], syslog["level"]) == ("10.0.0.2", "off")
        send("log syslog off", rb"log: done")
        assert get_logging(base_url)["channels"]["syslog"]["target"] == ""

        before = stored(get_logging(base_url))
        for bad in (
            "log level uart loud",
            "log level radio info",
            "log nothing",
            "log tag",
        ):
            send(bad, rb"usage: log")
        assert stored(get_logging(base_url)) == before
