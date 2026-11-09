"""Thread Console page (BDD: ConsoleLogic, ConsolePage). Runs static/console.js under node."""

import json

import pytest
from device_js import FRONTEND, logic, needs_node, page, scenario

NET = "ha-thread-bbc1"
RISKY = {
    "factoryreset": f"Erases the Thread network settings on this board and restarts OpenThread. The board leaves {NET} until it is joined again.",
    "reset": "Restarts the OpenThread stack. The board leaves the mesh for a few seconds.",
    "thread stop": f"Stops Thread on this board. It stops being a border router for {NET} until Thread is started again.",
    "ifconfig down": "Takes the Thread interface down. The board leaves the mesh until it is brought up again.",
    "dataset commit active": "Changes the network settings. A wrong dataset moves the board to another network.",
    "dataset init new": "Changes the network settings. A wrong dataset moves the board to another network.",
}


def c(expression):
    return logic("console", expression)


@needs_node
class TestConsoleLogic:
    """normalise, riskyReason, noticeState, backoff, historyStep"""

    @pytest.mark.parametrize("line", ["ot state", "state\n", "  state \r\n"])
    def test_normalise_strips_the_ot_prefix_and_line_ends(self, line):
        assert c(f"L.normalise({json.dumps(line)})") == "state"

    @pytest.mark.parametrize("line", ["", "   ", "ot "])
    def test_normalise_of_nothing_is_null(self, line):
        assert c(f"L.normalise({json.dumps(line)})") is None

    @pytest.mark.parametrize("cmd", sorted(RISKY))
    def test_risky_commands_explain_themselves(self, cmd):
        assert c(f"L.riskyReason({json.dumps(cmd)}, {json.dumps(NET)})") == RISKY[cmd]

    def test_risky_without_a_network_name_says_the_network(self):
        assert "the Thread network" in c("L.riskyReason('thread stop', null)")

    @pytest.mark.parametrize(
        "cmd",
        [
            "dataset",
            "dataset active",
            "dataset active -x",
            "dataset pending",
            "dataset pending -x",
            "state",
            "thread start",
            "ifconfig",
        ],
    )
    def test_safe_commands_are_not_risky(self, cmd):
        assert c(f"L.riskyReason({json.dumps(cmd)}, {json.dumps(NET)})") is None

    @pytest.mark.parametrize(
        "text,state",
        [
            (
                "# Connected to esp-otbr-d2cafc (OpenThread 1.4, router 0x6800)",
                "connected",
            ),
            ("# Thread is not running (mode SNIFFER)", "not-running"),
            ("# Closed: a new console session opened from 192.168.1.20", "taken-over"),
            ("# Closed: the console was turned off", "off"),
            ('# Busy: "ping fd00::1" is still running', "busy"),
            ("# Line too long (max 383 bytes)", "too-long"),
            ("router", None),
        ],
    )
    def test_notice_state(self, text, state):
        assert c(f"L.noticeState({json.dumps(text)})") == state

    def test_backoff_is_1_2_then_5_seconds(self):
        assert c("[1, 2, 3, 4, 10].map(L.backoff)") == [1000, 2000, 5000, 5000, 5000]

    def test_history_up_stops_at_the_oldest(self):
        r = c(
            "(() => { const h = ['a', 'b']; let s = {pos: -1}; const out = [];"
            " for (let i = 0; i < 3; i++) { s = L.historyStep(h, s.pos, 'up'); out.push(s.value); } return out; })()"
        )
        assert r == ["b", "a", "a"]

    def test_history_down_past_the_newest_returns_to_an_empty_input(self):
        r = c(
            "(() => { const h = ['a', 'b']; const s = L.historyStep(h, 1, 'down');"
            " return [s.pos, s.value, L.historyStep(h, -1, 'down').value]; })()"
        )
        assert r == [-1, "", ""]


def routes(enabled=True, session="null", network=NET, status="null"):
    # `status`: a JS expression for GET /device/status's body (null: 404, as before the Thread notice).
    return (
        "(u, o) => u === '/device/console' ? (o.method === 'PUT' ? {status: 200, body: {enabled: JSON.parse(o.body).enabled, session: null}}"
        f" : {{status: 200, body: {{enabled: {json.dumps(enabled)}, session: {session}}}}})"
        f" : u === '/node' ? {{status: 200, body: {{state: 'router', networkName: {json.dumps(network)}}}}}"
        f" : u === '/device/status' && ({status}) ? {{status: 200, body: ({status})}}"
        " : {status: 404, body: {}}"
    )


NO_RADIO = "globalThis.st || {mode: 'NO_RADIO', otbr_channel: 'usb-10c4-ea60-1', fault_reason: 'radio unplugged'}"


START = "t.page.init(); await t.advance(100);"
OPEN = (
    START
    + "const ws = t.sockets[0]; ws.open(); ws.recv('# Connected to esp-otbr-d2cafc (OpenThread 1.4, router 0x6800)');"
)


@needs_node
class TestConsolePage:
    """console.js against console.html's fake DOM, fake fetch and fake WebSocket"""

    def test_open_with_the_console_on_and_free_connects_by_itself(self):
        r = scenario(
            page("console", routes())
            + OPEN
            + "return [t.sockets.length, ws.url, t.el('con-pill').textContent, t.el('con-cmd').disabled,"
            " t.el('con-toggle').textContent];"
        )
        assert r == [
            1,
            "ws://board.lan/device/console/ws",
            "Connected",
            False,
            "Disconnect",
        ]

    def test_open_with_another_session_waits_for_connect(self):
        r = scenario(
            page("console", routes(session="{peer: '192.168.1.20', since_s: 40}"))
            + START
            + "await t.advance(2000);"
            " const before = [t.sockets.length, t.el('con-pill').textContent, t.el('con-where').textContent,"
            " t.el('con-cmd').disabled];"
            " t.el('con-toggle').click(); await t.advance(10);"
            " return before.concat([t.sockets.length]);"
        )
        assert r == [0, "In use", "In use by 192.168.1.20", True, 1]

    def test_open_while_a_reloaded_page_still_holds_the_session_connects_once_it_is_gone(
        self,
    ):
        r = scenario(
            page(
                "console",
                routes(
                    session="(globalThis.gets = (globalThis.gets || 0) + 1) === 1"
                    " ? {peer: '192.168.1.20', since_s: 0} : null"
                ),
            )
            + START
            + "const first = [t.sockets.length, t.el('con-pill').textContent];"
            " await t.advance(2000);"
            " return first.concat([t.sockets.length, t.el('con-where').textContent]);"
        )
        assert r == [0, "Connecting", 1, ""]

    def test_open_with_the_console_off_offers_to_turn_it_on(self):
        r = scenario(
            page("console", routes(enabled=False))
            + START
            + "const before = [t.sockets.length, t.el('con-off').hidden(), t.el('con-pill').textContent];"
            " t.el('con-enable').click(); await t.advance(100);"
            " const put = t.calls.find(x => x.method === 'PUT');"
            " return before.concat([put.url, JSON.parse(put.body), t.sockets.length]);"
        )
        assert r == [0, False, "Off", "/device/console", {"enabled": True}, 1]

    def test_a_command_is_sent_and_echoed(self):
        r = scenario(
            page("console", routes())
            + OPEN
            + "t.el('con-cmd').value = 'ot state'; t.el('con-run').click();"
            " ws.recv('router\\r\\n'); ws.recv('Done\\r\\n');"
            " return [ws.sent, t.el('con-out').allText(), t.el('con-cmd').value];"
        )
        assert r[0] == ["state"]
        assert "> state" in r[1] and "router" in r[1] and "Done" in r[1]
        assert r[2] == ""

    def test_a_risky_command_opens_the_dialog_and_cancel_sends_nothing(self):
        r = scenario(
            page("console", routes())
            + OPEN
            + "t.el('con-cmd').value = 'thread stop'; t.el('con-run').click(); await t.advance(10);"
            " const shown = [t.el('con-dialog').open, t.el('con-dlg-cmd').textContent, t.el('con-dlg-why').textContent];"
            " t.el('con-dlg-cancel').click();"
            " return shown.concat([t.el('con-dialog').open, ws.sent]);"
        )
        assert r == [True, "thread stop", RISKY["thread stop"], False, []]

    def test_confirming_a_risky_command_sends_it(self):
        r = scenario(
            page("console", routes())
            + OPEN
            + "t.el('con-cmd').value = 'thread stop'; t.el('con-run').click(); await t.advance(10);"
            " t.el('con-dlg-run').click(); return ws.sent;"
        )
        assert r == ["thread stop"]

    def test_disconnect_closes_the_socket_and_disables_the_input(self):
        r = scenario(
            page("console", routes())
            + OPEN
            + "t.el('con-toggle').click(); await t.advance(10000);"
            " return [ws.closedWith, t.el('con-cmd').disabled, t.el('con-toggle').textContent, t.sockets.length];"
        )
        assert r == [1000, True, "Connect", 1]

    def test_a_dropped_connection_reconnects_and_never_resends_the_waiting_line(self):
        r = scenario(
            page("console", routes())
            + OPEN
            + "t.el('con-cmd').value = 'ping fd00::1'; t.el('con-run').click();"
            " ws.drop(1006); const pill = t.el('con-pill').textContent; await t.advance(1000);"
            " const ws2 = t.sockets[1]; ws2.open(); ws2.recv('# Connected to x (OpenThread 1.4, router 0x6800)');"
            " return [pill, t.sockets.length, ws2.sent, t.el('con-out').allText().includes('was not sent again')];"
        )
        assert r == ["Reconnecting", 2, [], True]

    def test_taken_over_stays_closed_until_connect(self):
        r = scenario(
            page("console", routes())
            + OPEN
            + "ws.recv('# Closed: a new console session opened from 192.168.1.20'); ws.drop(4001);"
            " await t.advance(10000);"
            " return [t.el('con-pill').textContent, t.sockets.length, t.el('con-toggle').textContent];"
        )
        assert r == ["Taken over", 1, "Connect"]

    def test_a_reconnect_that_finds_another_session_stops_and_says_who(self):
        r = scenario(
            page("console", routes(session="globalThis.sess || null"))
            + OPEN
            + "await t.advance(20000); globalThis.sess = {peer: '192.168.1.20', since_s: 3}; ws.drop(1006);"
            " await t.advance(1100);"
            " return [t.sockets.length, t.el('con-pill').textContent, t.el('con-where').textContent,"
            " t.el('con-toggle').textContent];"
        )
        assert r == [1, "In use", "In use by 192.168.1.20", "Connect"]

    def test_a_reconnect_that_finds_its_own_stale_session_takes_it_back(self):
        r = scenario(
            page("console", routes(session="globalThis.sess || null"))
            + OPEN
            + "await t.advance(20000); globalThis.sess = {peer: '192.168.1.5', since_s: 21}; ws.drop(1006);"
            " await t.advance(1100); return t.sockets.length;"
        )
        assert r == 2

    def test_waiting_for_the_radio_reads_disconnect_and_its_click_ends_the_wait(self):
        # On NO_RADIO while the page waits for the radio: the toggle reads Disconnect (the wait is a retry); its click
        # ends the wait (no socket, no further status checks), the toggle then reads Connect.
        r = scenario(
            page("console", routes(status=NO_RADIO))
            + START
            + "const before = [t.el('con-pill').textContent, t.el('con-toggle').textContent];"
            " t.el('con-toggle').click(); await t.advance(10);"
            " const n = t.calls.length; await t.advance(20000);"
            " return before.concat([t.el('con-pill').textContent, t.el('con-toggle').textContent, t.sockets.length,"
            " t.calls.length - n]);"
        )
        assert r == [
            "Thread not running",
            "Disconnect",
            "Disconnected",
            "Connect",
            0,
            0,
        ]

    def test_waiting_for_the_radio_connects_once_thread_runs_again(self):
        # On NO_RADIO, then Thread back: the wait asks again, the toggle reads Disconnect throughout, and the page
        # connects by itself.
        r = scenario(
            page("console", routes(status=NO_RADIO))
            + START
            + "await t.advance(5500); const during = t.el('con-toggle').textContent;"
            " globalThis.st = {mode: 'BR_RUNNING', otbr_channel: 'builtin-c6'}; await t.advance(5500);"
            " return [during, t.sockets.length, t.el('con-toggle').textContent];"
        )
        assert r == ["Disconnect", 1, "Disconnect"]

    def test_disconnect_while_connecting_closes_that_socket(self):
        r = scenario(
            page("console", routes())
            + START
            + "t.el('con-toggle').click(); await t.advance(10000);"
            " return [t.sockets.length, t.sockets[0].closedWith, t.el('con-toggle').textContent];"
        )
        assert r == [1, 1000, "Connect"]

    def test_a_late_close_of_the_old_socket_leaves_the_new_session_alone(self):
        r = scenario(
            page("console", routes())
            + OPEN
            + "ws.close = function(code) { ws.closedWith = code; };"
            " t.el('con-toggle').click(); t.el('con-toggle').click(); await t.advance(100);"
            " const ws2 = t.sockets[1]; ws2.open(); ws2.recv('# Connected to x (OpenThread 1.4, router 0x6800)');"
            " ws.drop(1000); await t.advance(10000);"
            " return [t.sockets.length, t.el('con-pill').textContent, t.el('con-cmd').disabled,"
            " t.el('con-toggle').textContent];"
        )
        assert r == [2, "Connected", False, "Disconnect"]

    def test_a_refused_line_leaves_the_earlier_one_waiting(self):
        r = scenario(
            page("console", routes())
            + OPEN
            + "t.page.run('ping fd00::1'); t.page.run('state');"
            " ws.recv('# Busy: \"ping fd00::1\" is still running'); ws.drop(1006);"
            " return t.el('con-out').allText();"
        )
        assert '"ping fd00::1" got no answer' in r
        assert '"state" got no answer' not in r

    def test_a_busy_notice_after_the_first_line_finished_does_not_bring_it_back(self):
        r = scenario(
            page("console", routes())
            + OPEN
            + "t.page.run('state'); t.page.run('ping fd00::1');"
            " ws.recv('leader\\r\\nDone\\r\\n'); ws.recv('# Busy: \"state\" is still running'); ws.drop(1006);"
            " return t.el('con-out').allText();"
        )
        assert "got no answer" not in r

    @pytest.mark.parametrize("code,pill", [(4001, "Taken over"), (4003, "Off")])
    def test_a_close_from_the_board_without_its_notice_is_not_retried(self, code, pill):
        r = scenario(
            page("console", routes())
            + OPEN
            + f"ws.drop({code}); await t.advance(10000);"
            " return [t.el('con-pill').textContent, t.sockets.length, t.el('con-toggle').textContent];"
        )
        assert r == [pill, 1, "Connect"]

    def test_connect_during_the_reload_check_cancels_it(self):
        r = scenario(
            page("console", routes(session="{peer: '192.168.1.20', since_s: 0}"))
            + START
            + "t.el('con-toggle').click(); const ws = t.sockets[0]; ws.open();"
            " ws.recv('# Connected to x (OpenThread 1.4, router 0x6800)'); await t.advance(3000);"
            " return [t.sockets.length, t.el('con-pill').textContent, t.el('con-where').textContent];"
        )
        assert r == [1, "Connected", ""]

    def test_disconnect_during_the_reload_check_cancels_it(self):
        r = scenario(
            page(
                "console",
                routes(
                    session="(globalThis.gets = (globalThis.gets || 0) + 1) === 1"
                    " ? {peer: '192.168.1.20', since_s: 0} : null"
                ),
            )
            + START
            + "t.el('con-toggle').click(); t.el('con-toggle').click(); await t.advance(3000);"
            " return [t.sockets.length, t.el('con-pill').textContent];"
        )
        assert r == [1, "Disconnected"]

    def test_copy_all_puts_the_output_on_the_clipboard(self):
        r = scenario(
            page("console", routes())
            + OPEN
            + "ws.recv('leader\\r\\nDone\\r\\n'); t.el('con-copy').click(); await t.advance(10);"
            " return [t.clipboard.written.length, t.clipboard.written[0].includes('leader'),"
            " t.el('con-copy').textContent];"
        )
        assert r == [1, True, "Copied"]

    def test_copy_all_without_the_clipboard_api_falls_back_to_exec_command(self):
        r = scenario(
            page("console", routes())
            + OPEN
            + "t.env.navigator.clipboard = null; t.doc.body = t.el('con-chips');"
            " t.doc.execCommand = (cmd) => { globalThis.cmd = cmd; return true; };"
            " t.el('con-copy').click(); await t.advance(10);"
            " return [globalThis.cmd, t.el('con-copy').textContent];"
        )
        assert r == ["copy", "Copied"]

    def test_copy_all_says_so_when_no_way_to_copy_works(self):
        r = scenario(
            page("console", routes())
            + OPEN
            + "t.env.navigator.clipboard = null; t.el('con-copy').textContent = 'Copy all';"
            " t.el('con-copy').click(); await t.advance(10);"
            " const shown = t.el('con-copy').textContent; await t.advance(3000);"
            " return [shown, t.el('con-copy').textContent];"
        )
        assert r == ["Copy failed", "Copy all"]


def test_nav_lists_the_console_after_tools():
    api = (FRONTEND / "static" / "api.js").read_text()
    tools = api.index("{href : '/tools.html', label : 'Tools'}")
    console = api.index("{href : '/console.html', label : 'Console'}")
    assert console > tools
