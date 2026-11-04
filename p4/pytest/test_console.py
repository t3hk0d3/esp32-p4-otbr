"""Thread console: the board's OpenThread CLI over GET /device/console/ws, its setting, and the USB `ot` command that
shares the same arbiter (spec 2026-11-04-thread-console). Hardware-gated: run in the batched on-board pass.

Rig: the board alone, running as a border router with a stored dataset. No Thread peer is needed.
"""

import asyncio
import re
import time
from pathlib import Path

import aiohttp
import pytest
import requests
from conftest import ot, start_capture, stop_capture, wait_ot_state

RUNNING_ROLES = ("leader", "router", "child")
STATE_TIMEOUT_S = 120
USB_TIMEOUT_S = 10
ANSWER_S = 15
MODE_TIMEOUT_S = 30
NO_ANSWER_CLOSE_S = 40
KEEP_ALIVE_SOCKETS = 6
FRONTEND = (
    Path(__file__).resolve().parents[1] / "components" / "esp_ot_br_server" / "frontend"
)
END = re.compile(r"^(Done|Error \d+: .*)$", re.MULTILINE)


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


@pytest.fixture
def console(base_url):
    """The console turned on for the test, off again afterwards (also the default)."""
    r = requests.put(f"{base_url}/device/console", json={"enabled": True}, timeout=10)
    assert r.status_code == 200, r.text
    yield f"ws://{base_url.removeprefix('http://')}/device/console/ws"
    requests.put(f"{base_url}/device/console", json={"enabled": False}, timeout=10)


def wait_node_state(base_url, states, timeout):
    """Poll GET /node until its state is one of `states`: the USB `ot` command is refused while a session is open."""
    deadline = time.monotonic() + timeout
    state = None
    while time.monotonic() < deadline:
        try:
            state = requests.get(f"{base_url}/node", timeout=10).json().get("state")
        except (requests.RequestException, ValueError):
            state = None
        if state in states:
            return
        time.sleep(2)
    pytest.fail(f"node state is {state!r}, expected one of {states}")


def wait_for_mode(base_url, mode, timeout):
    deadline = time.monotonic() + timeout
    current = None
    while time.monotonic() < deadline:
        current = requests.get(f"{base_url}/device/status", timeout=10).json()["mode"]
        if current == mode:
            return
        time.sleep(0.5)
    pytest.fail(f"mode is {current}, expected {mode}")


async def frame(ws, timeout=ANSWER_S):
    """The next text frame; fails on a close. wait_for, since the board's pings restart receive()'s own timeout."""
    msg = await asyncio.wait_for(ws.receive(), timeout)
    assert msg.type == aiohttp.WSMsgType.TEXT, (msg.type, msg.data, ws.close_code)
    return msg.data


async def run(ws, line, timeout=ANSWER_S):
    """Send a line; return the CLI output up to its Done or Error line, and the notices seen meanwhile."""
    await ws.send_str(line)
    text, notices = "", []
    deadline = time.monotonic() + timeout
    while not END.search(text):
        data = await frame(ws, max(0.1, deadline - time.monotonic()))
        if data.startswith("# "):
            notices.append(data)
        else:
            text += data.replace("\r", "")
    return text, notices


async def connected(session, url):
    ws = await session.ws_connect(url)
    greeting = await frame(ws)
    assert greeting.startswith("# Connected to "), greeting
    return ws


async def closed(ws, timeout=ANSWER_S):
    """Frames until the close; return (notices, close code)."""
    notices = []
    while True:
        msg = await ws.receive(timeout=timeout)
        if msg.type == aiohttp.WSMsgType.TEXT:
            notices.append(msg.data)
        elif msg.type in (
            aiohttp.WSMsgType.CLOSE,
            aiohttp.WSMsgType.CLOSED,
            aiohttp.WSMsgType.CLOSING,
        ):
            return notices, ws.close_code


def test_off_by_default_closes_with_4003(base_url):
    assert requests.get(f"{base_url}/device/console", timeout=10).json() == {
        "enabled": False,
        "session": None,
    }

    async def go():
        async with aiohttp.ClientSession() as session:
            ws = await session.ws_connect(
                f"ws://{base_url.removeprefix('http://')}/device/console/ws"
            )
            return await closed(ws)

    notices, code = asyncio.run(go())
    assert notices == ["# Closed: the console was turned off"]
    assert code == 4003


def test_put_rejects_a_non_boolean(base_url):
    r = requests.put(f"{base_url}/device/console", json={"enabled": "yes"}, timeout=10)
    assert r.status_code == 400
    assert r.json()["error"] == "invalid_request"


def test_state_dns_browse_and_the_session_report(base_url, console, dut):
    wait_ot_state(dut, RUNNING_ROLES, STATE_TIMEOUT_S)

    async def go():
        async with aiohttp.ClientSession() as session:
            ws = await connected(session, console)
            state, _ = await run(ws, "ot state")
            report = (
                await asyncio.to_thread(
                    requests.get, f"{base_url}/device/console", timeout=10
                )
            ).json()
            browse, _ = await run(
                ws, "dns browse _meshcop._udp.default.service.arpa", timeout=20
            )
            await ws.close()
            return state, report, browse

    state, report, browse = asyncio.run(go())
    assert state.splitlines()[0] in RUNNING_ROLES and state.splitlines()[-1] == "Done"
    assert (
        report["enabled"] is True
        and report["session"]["since_s"] >= 0
        and report["session"]["peer"]
    )
    assert "DNS browse response for _meshcop._udp.default.service.arpa." in browse
    assert browse.rstrip().endswith("Done")


def test_busy_while_a_ping_runs(console, dut):
    wait_ot_state(dut, RUNNING_ROLES, STATE_TIMEOUT_S)

    async def go():
        async with aiohttp.ClientSession() as session:
            ws = await connected(session, console)
            mleid = (await run(ws, "ipaddr mleid"))[0].splitlines()[0]
            await ws.send_str(f"ping {mleid} 8 3 2")
            await ws.send_str("state")
            text, notices = "", []
            while "packets transmitted" not in text or not END.search(text):
                data = await frame(ws)
                if data.startswith("# "):
                    notices.append(data)
                else:
                    text += data.replace("\r", "")
            after, _ = await run(ws, "state")
            await ws.close()
            return mleid, notices, after

    mleid, notices, after = asyncio.run(go())
    assert f'# Busy: "ping {mleid} 8 3 2" is still running' in notices
    assert after.splitlines()[0] in RUNNING_ROLES


def test_a_new_session_takes_over(console):
    async def go():
        async with aiohttp.ClientSession() as session:
            first = await connected(session, console)
            second = await connected(session, console)
            result = await closed(first)
            await second.close()
            return result

    notices, code = asyncio.run(go())
    assert len(notices) == 1 and notices[0].startswith(
        "# Closed: a new console session opened from "
    )
    assert code == 4001


def test_sniffer_refuses_lines_and_the_console_recovers(base_url, console, dut):
    wait_ot_state(dut, RUNNING_ROLES, STATE_TIMEOUT_S)

    async def go():
        async with aiohttp.ClientSession() as session:
            ws = await connected(session, console)
            capture = await asyncio.to_thread(start_capture, base_url)
            try:
                await asyncio.to_thread(
                    wait_for_mode, base_url, "SNIFFER", MODE_TIMEOUT_S
                )
                await ws.send_str("state")
                refused = await frame(ws)
            finally:
                await asyncio.to_thread(stop_capture, base_url, capture)
            await asyncio.to_thread(
                wait_for_mode, base_url, "BR_RUNNING", MODE_TIMEOUT_S
            )
            await asyncio.to_thread(
                wait_node_state, base_url, RUNNING_ROLES, STATE_TIMEOUT_S
            )
            after, _ = await run(ws, "state")
            await ws.close()
            return refused, after

    refused, after = asyncio.run(go())
    assert refused == "# Thread is not running (mode SNIFFER)"
    assert after.splitlines()[0] in RUNNING_ROLES


def test_loading_every_page_leaves_the_console_open(base_url, console):
    """A browser's six keep-alive sockets plus the console fill the server's seven; every extra connection makes it
    close the least recently used one, which must not be the console that answers its pings."""
    pages = sorted(p.name for p in FRONTEND.glob("*.html"))

    async def go():
        async with aiohttp.ClientSession() as session:
            ws = await connected(session, console)
            connector = aiohttp.TCPConnector(limit=KEEP_ALIVE_SOCKETS)
            async with aiohttp.ClientSession(connector=connector) as browser:
                for name in pages:
                    replies = await asyncio.gather(
                        *(
                            browser.get(f"{base_url}/{name}")
                            for _ in range(KEEP_ALIVE_SOCKETS)
                        )
                    )
                    for r in replies:
                        assert r.status == 200, name
                        await r.read()
                    await asyncio.sleep(1.5)  # past the console's stamp refresh
                    # A new socket: the server purges one.
                    async with (
                        aiohttp.ClientSession() as other,
                        other.get(f"{base_url}/device/status") as r,
                    ):
                        assert r.status == 200
            after, _ = await run(ws, "state")
            await ws.close()
            return after

    assert asyncio.run(go()).rstrip().endswith("Done")


def test_a_client_that_ignores_pings_is_closed(console):
    async def go():
        async with aiohttp.ClientSession() as session:
            ws = await session.ws_connect(console, autoping=False)
            start = time.monotonic()
            while True:
                msg = await ws.receive(timeout=NO_ANSWER_CLOSE_S + 5)
                if msg.type in (
                    aiohttp.WSMsgType.CLOSE,
                    aiohttp.WSMsgType.CLOSED,
                    aiohttp.WSMsgType.CLOSING,
                ):
                    return time.monotonic() - start, ws.close_code, msg.extra

    elapsed, code, reason = asyncio.run(go())
    assert code == 1001 and reason == "no answer"
    assert elapsed < NO_ANSWER_CLOSE_S


def test_turning_off_closes_the_session(base_url, console):
    async def go():
        async with aiohttp.ClientSession() as session:
            ws = await connected(session, console)
            r = await asyncio.to_thread(
                requests.put,
                f"{base_url}/device/console",
                json={"enabled": False},
                timeout=10,
            )
            assert r.status_code == 200 and r.json() == {
                "enabled": False,
                "session": None,
            }
            return await closed(ws)

    notices, code = asyncio.run(go())
    assert notices == ["# Closed: the console was turned off"]
    assert code == 4003


def test_usb_ot_command_still_works(dut, board_ip):
    wait_ot_state(dut, RUNNING_ROLES, STATE_TIMEOUT_S)
    lines = ot(dut, "state")
    assert lines[-1] in RUNNING_ROLES


def test_usb_ot_command_is_refused_while_a_session_is_attached(dut, console):
    async def go():
        async with aiohttp.ClientSession() as session:
            ws = await connected(session, console)
            dut.write("ot state")
            await asyncio.to_thread(
                dut.expect, re.compile(rb"console in use from \S+"), timeout=10
            )
            await ws.close()

    asyncio.run(go())


def test_usb_ot_command_times_out(dut, board_ip):
    wait_ot_state(dut, RUNNING_ROLES, STATE_TIMEOUT_S)
    mleid = ot(dut, "ipaddr mleid")[0]
    # Five pings 3 s apart: the prompt comes after about 12 s, past the command's 10 s wait. OpenThread does not
    # answer pings to its own address, so only the count sent is certain.
    dut.write(f"ot ping {mleid} 8 5 3")
    dut.expect(
        re.compile(rb"No prompt after 10 s: \"ping [^\"]+\" is still running"),
        timeout=USB_TIMEOUT_S + 5,
    )
    dut.expect(re.compile(rb"5 packets transmitted, "), timeout=10)
    dut.expect(re.compile(rb"\r*\nDone\r*\n"), timeout=5)
    assert ot(dut, "state")[-1] in RUNNING_ROLES
