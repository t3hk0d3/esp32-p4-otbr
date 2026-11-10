"""Device -> Logs page (BDD: LogsLogic, LogsPage). Runs static/logs.js against logs.html's fake DOM under node."""

import json

import pytest
from device_js import logic, needs_node, page, scenario


def lg(expression):
    return logic("logs", expression)


def settings(uart="info", jtag="off", syslog="warning", target="192.168.1.10"):
    return {
        "channels": {
            "uart": {"level": uart},
            "jtag": {"level": jtag, "host": False},
            "syslog": {"level": syslog, "target": target, "state": "sending"},
        },
        "components": {"*": "info", "ziggurat": "debug"},
    }


@needs_node
class TestLogsLogic:
    """summaryChannels, summaryComponents, classify, filter, targetValid and the helpers the page builds on"""

    def test_channels_summary_names_levels_and_the_syslog_target(self):
        channels = json.dumps(settings()["channels"])
        assert (
            lg(f"L.summaryChannels({channels})")
            == "UART Info · USB-JTAG Off · Syslog Warning → 192.168.1.10:514"
        )

    def test_channels_summary_keeps_a_given_port_and_brackets_ipv6(self):
        a = settings(target="logs.lan:1514")["channels"]
        b = settings(target="[fd00::1]")["channels"]
        assert lg(f"L.summaryChannels({json.dumps(a)})").endswith("→ logs.lan:1514")
        assert lg(f"L.summaryChannels({json.dumps(b)})").endswith("→ [fd00::1]:514")

    def test_channels_summary_has_no_arrow_when_syslog_is_off_or_has_no_target(self):
        off = settings(syslog="off")["channels"]
        none = settings(target="")["channels"]
        assert (
            lg(f"L.summaryChannels({json.dumps(off)})")
            == "UART Info · USB-JTAG Off · Syslog Off"
        )
        assert (
            lg(f"L.summaryChannels({json.dumps(none)})")
            == "UART Info · USB-JTAG Off · Syslog Warning"
        )

    def test_components_summary_lists_the_default_then_the_overrides(self):
        c = {"*": "info", "ziggurat": "debug"}
        assert (
            lg(f"L.summaryComponents({json.dumps(c)})")
            == "everything else Info · ziggurat Debug"
        )

    def test_components_summary_without_overrides_is_the_default_alone(self):
        assert lg("L.summaryComponents({'*': 'warning'})") == "everything else Warning"

    @pytest.mark.parametrize(
        "letter,level",
        [
            ("E", "error"),
            ("W", "warning"),
            ("I", "info"),
            ("D", "debug"),
            ("V", "verbose"),
        ],
    )
    def test_classify_gives_a_log_line_its_level(self, letter, level):
        frame = f"{letter} (813120) zigbee: request timeout"
        r = lg(f"L.classify({json.dumps(frame)})")
        assert r["level"] == level
        assert r["text"] == frame
        assert r["tag"] == "zigbee"
        assert r["notice"] is False

    def test_classify_gives_a_hash_frame_the_notice_kind(self):
        r = lg("L.classify('# dropped 12 lines')")
        assert r["notice"] is True
        assert r["text"] == "# dropped 12 lines"

    def test_classify_keeps_colons_of_the_body(self):
        r = lg("L.classify('I (5) OPENTHREAD: [I] Mle: Send Advertisement')")
        assert r["tag"] == "OPENTHREAD"

    def test_classify_of_an_unrecognised_frame_is_a_plain_line(self):
        r = lg("L.classify('something else')")
        assert r["notice"] is False
        assert r["level"] is None
        assert r["text"] == "something else"

    def test_filter_is_a_case_insensitive_substring_match(self):
        lines = [{"text": "I (1) Zigbee: up"}, {"text": "I (2) netcfg: up"}]
        assert lg(f"L.filter({json.dumps(lines)}, 'ZIG')") == [lines[0]]

    def test_filter_with_no_text_keeps_every_line(self):
        lines = [{"text": "a"}, {"text": "b"}]
        assert lg(f"L.filter({json.dumps(lines)}, '')") == lines

    def test_filter_with_no_match_is_empty(self):
        assert lg("L.filter([{text: 'a'}], 'zzz')") == []

    @pytest.mark.parametrize(
        "text",
        [
            "192.168.1.10",
            "192.168.1.10:1514",
            "[fd00::1]",
            "[fd00::1]:1514",
            "logs.lan:514",
        ],
    )
    def test_target_valid_accepts_what_the_board_accepts(self, text):
        assert lg(f"L.targetValid({json.dumps(text)})") is True

    @pytest.mark.parametrize(
        "text",
        ["", ":514", "host:0", "host:65536", "host:x", "[fd00::1", "fd00::1:514"],
    )
    def test_target_valid_refuses_what_the_board_refuses(self, text):
        assert lg(f"L.targetValid({json.dumps(text)})") is False

    def test_with_component_adds_or_replaces_and_keeps_the_default(self):
        r = lg("L.withComponent({'*': 'info', a: 'debug'}, 'a', 'error')")
        assert r == {"*": "info", "a": "error"}
        r = lg("L.withComponent({'*': 'info'}, 'b', 'verbose')")
        assert r == {"*": "info", "b": "verbose"}

    def test_without_component_removes_one_override(self):
        r = lg("L.withoutComponent({'*': 'info', a: 'debug', b: 'error'}, 'a')")
        assert r == {"*": "info", "b": "error"}

    @pytest.mark.parametrize(
        "tag,ok",
        [
            ("ziggurat", True),
            ("a" * 23, True),
            ("a" * 24, False),
            ("", False),
            ("*", False),
            ("has space", False),
        ],
    )
    def test_tag_valid(self, tag, ok):
        assert lg(f"L.tagValid({json.dumps(tag)})") is ok


REPLY = {
    "channels": {
        "uart": {"level": "info"},
        "jtag": {"level": "off", "host": False},
        "syslog": {"level": "warning", "target": "192.168.1.10", "state": "sending"},
    },
    "components": {"*": "info", "ziggurat": "debug"},
    "known_tags": ["radio_mux", "zigbee", "ziggurat", "OPENTHREAD"],
    "viewers": 0,
    "counters": {
        "uart": {"sent": 10, "dropped": 0},
        "jtag": {"sent": 0, "dropped": 0},
        "syslog": {"sent": 4, "dropped": 0},
        "live": {"sent": 0, "dropped": 0},
    },
}

# Fake board: GET returns globalThis.rep; PUT merges the subset into it and returns it (or answers globalThis.bad).
ROUTES = (
    "(u, o) => { if (u !== '/device/logging') return {status: 404, body: {}};"
    " if (o.method === 'PUT') { const b = JSON.parse(o.body); (globalThis.puts = globalThis.puts || []).push(b);"
    " if (globalThis.bad) return {status: 400, body: {error: 'invalid_request', detail: globalThis.bad}};"
    " for (const ch of Object.keys(b.channels || {})) Object.assign(globalThis.rep.channels[ch], b.channels[ch]);"
    " if (b.components) globalThis.rep.components = b.components;"
    " return {status: 200, body: globalThis.rep}; }"
    " return {status: globalThis.st || 200, body: globalThis.st ? {} : globalThis.rep}; }"
)
SETUP = f"globalThis.rep = {json.dumps(REPLY)};"
START = page("logs", ROUTES) + "t.page.init(); await t.advance(100);"
# The text of an element and its children, run together (allText joins them with spaces).
TXT = "const txt = (e) => e.textContent + e.children.map(txt).join('');"
LINE = "'I (1000) zigbee: up'"


def run(body, setup=""):
    return scenario(SETUP + setup + TXT + START + body)


@needs_node
class TestLogsPage:
    """logs.js against logs.html's fake DOM, fake fetch and fake WebSocket"""

    # --- On load
    def test_load_reads_the_settings_once(self):
        r = run(
            "return t.calls.filter(c => c.url === '/device/logging').map(c => c.method);"
        )
        assert r == ["GET"]

    def test_load_leaves_both_settings_cards_folded_with_summaries(self):
        r = run(
            "return [t.el('ch-card').classList.contains('lg-open'), t.el('cp-card').classList.contains('lg-open'),"
            " t.el('ch-sum').textContent, t.el('cp-sum').textContent,"
            " t.el('ch-fold').getAttribute('aria-expanded')];"
        )
        assert r == [
            False,
            False,
            "UART Info · USB-JTAG Off · Syslog Warning → 192.168.1.10:514",
            "everything else Info · ziggurat Debug",
            "false",
        ]

    def test_load_shows_each_channel_level_target_and_status(self):
        r = run(
            "return [t.el('ch-uart').value, t.el('ch-jtag').value, t.el('ch-syslog').value, t.el('sys-target').value,"
            " t.el('uart-pill').textContent, t.el('jtag-pill').textContent, t.el('sys-pill').textContent];"
        )
        assert r == [
            "info",
            "off",
            "warning",
            "192.168.1.10",
            "active",
            "no host",
            "sending",
        ]

    def test_load_starts_the_live_card_disconnected_without_a_socket(self):
        r = run(
            "return [t.el('live-pill').textContent, t.el('live-pill').classList.contains('con-idle'),"
            " t.el('live-toggle').textContent, t.sockets.length];"
        )
        assert r == ["Disconnected", True, "Connect", 0]

    def test_load_failure_with_403_says_the_interface_does_not_serve_the_device_api(
        self,
    ):
        r = run("return t.el('lg-note').textContent;", setup="globalThis.st = 403;")
        assert "device API" in r

    def test_folding_a_card_opens_it_and_a_second_click_folds_it(self):
        r = run(
            "t.el('ch-fold').click(); const a = [t.el('ch-card').classList.contains('lg-open'), t.el('ch-fold').getAttribute('aria-expanded')];"
            " t.el('ch-fold').click(); return a.concat([t.el('ch-card').classList.contains('lg-open')]);"
        )
        assert r == [True, "true", False]

    # --- Channels
    def test_a_channel_level_change_puts_only_that_field(self):
        r = run(
            "t.el('ch-uart').value = 'debug'; t.el('ch-uart').fire('change'); await t.advance(100);"
            " return globalThis.puts;"
        )
        assert r == [{"channels": {"uart": {"level": "debug"}}}]

    def test_a_channel_level_change_updates_the_summary_from_the_reply(self):
        r = run(
            "t.el('ch-jtag').value = 'error'; t.el('ch-jtag').fire('change'); await t.advance(100);"
            " return t.el('ch-sum').textContent;"
        )
        assert r.startswith("UART Info · USB-JTAG Error")

    def test_a_valid_syslog_target_is_put_on_change_with_nothing_else(self):
        r = run(
            "t.el('sys-target').type('logs.lan:1514'); const ok = t.el('sys-target').getAttribute('aria-invalid');"
            " t.el('sys-target').fire('change'); await t.advance(100);"
            " return [ok, globalThis.puts, t.el('ch-sum').textContent];"
        )
        assert r[0] == "false"
        assert r[1] == [{"channels": {"syslog": {"target": "logs.lan:1514"}}}]
        assert r[2].endswith("→ logs.lan:1514")

    def test_an_invalid_syslog_target_is_flagged_while_typing_and_never_sent(self):
        r = run(
            "t.el('sys-target').type('fd00::1:514'); const a = [t.el('sys-target').getAttribute('aria-invalid'),"
            " t.el('sys-hint').textContent];"
            " t.el('sys-target').fire('change'); await t.advance(100);"
            " return a.concat([globalThis.puts || null]);"
        )
        assert r[0] == "true"
        assert "host[:port]" in r[1]
        assert r[2] is None

    def test_an_emptied_syslog_target_is_put_as_empty(self):
        r = run(
            "t.el('sys-target').type(''); t.el('sys-target').fire('change'); await t.advance(100);"
            " return [t.el('sys-target').getAttribute('aria-invalid'), globalThis.puts];",
        )
        assert r == ["false", [{"channels": {"syslog": {"target": ""}}}]]

    @pytest.mark.parametrize(
        "state,text,cls",
        [
            ("off", "off", "con-idle"),
            ("resolving", "resolving", "con-warn"),
            ("sending", "sending", "con-ok"),
            ("unresolved", "unresolved", "con-warn"),
        ],
    )
    def test_syslog_pill_follows_the_state(self, state, text, cls):
        r = run(
            "return [t.el('sys-pill').textContent, t.el('sys-pill').classList.contains("
            + json.dumps(cls)
            + ")];",
            setup=f"globalThis.rep.channels.syslog.state = {json.dumps(state)};"
            f" globalThis.rep.channels.syslog.level = {json.dumps('off' if state == 'off' else 'warning')};",
        )
        assert r == [text, True]

    def test_jtag_pill_says_host_when_something_reads_it(self):
        r = run(
            "return t.el('jtag-pill').textContent;",
            setup="globalThis.rep.channels.jtag.host = true;",
        )
        assert r == "host"

    def test_a_channel_with_dropped_lines_says_how_many(self):
        r = run(
            "return t.el('jtag-pill').textContent;",
            setup="globalThis.rep.counters.jtag.dropped = 12;",
        )
        assert "12 dropped" in r

    def test_a_rejected_put_shows_the_field_and_puts_the_controls_back(self):
        r = run(
            "t.el('ch-syslog').value = 'debug'; t.el('ch-syslog').fire('change'); await t.advance(100);"
            " return [t.el('lg-note').textContent, t.el('ch-syslog').value];",
            setup="globalThis.bad = 'channels.syslog.target';",
        )
        assert r[0] == "Syslog needs a valid host[:port] before it can be turned on."
        assert r[1] == "warning"

    def test_a_rejected_put_names_any_other_refused_field(self):
        r = run(
            "t.el('ch-uart').value = 'debug'; t.el('ch-uart').fire('change'); await t.advance(100);"
            " return [t.el('lg-note').textContent, t.el('ch-uart').value];",
            setup="globalThis.bad = 'channels.uart.level';",
        )
        assert "channels.uart.level" in r[0]
        assert r[1] == "info"

    # --- Components
    def test_components_list_the_default_and_each_override_with_levels(self):
        r = run(
            "return [t.el('cp-lv-*').value, t.el('cp-lv-ziggurat').value, t.el('cp-rows').children.length];"
        )
        assert r == ["info", "debug", 2]

    def test_known_tags_without_an_override_are_suggested(self):
        r = run("return t.el('known-tags').children.map(o => o.value);")
        assert r == ["radio_mux", "zigbee", "OPENTHREAD"]

    def test_changing_the_default_puts_the_whole_component_set(self):
        r = run(
            "t.el('cp-lv-*').value = 'warning'; t.el('cp-lv-*').fire('change'); await t.advance(100);"
            " return [globalThis.puts, t.el('cp-sum').textContent];"
        )
        assert r[0] == [{"components": {"*": "warning", "ziggurat": "debug"}}]
        assert r[1] == "everything else Warning · ziggurat Debug"

    def test_changing_an_override_puts_the_set_with_the_new_level(self):
        r = run(
            "t.el('cp-lv-ziggurat').value = 'verbose'; t.el('cp-lv-ziggurat').fire('change'); await t.advance(100);"
            " return globalThis.puts;"
        )
        assert r == [{"components": {"*": "info", "ziggurat": "verbose"}}]

    def test_removing_an_override_puts_the_set_without_it(self):
        r = run(
            "t.el('cp-del-ziggurat').click(); await t.advance(100);"
            " return [globalThis.puts, t.el('cp-rows').children.length, t.el('cp-sum').textContent];"
        )
        assert r == [[{"components": {"*": "info"}}], 1, "everything else Info"]

    def test_adding_a_component_puts_it_with_the_chosen_level_and_clears_the_field(
        self,
    ):
        r = run(
            "t.el('cp-new').type('zigbee'); t.el('cp-new-lv').value = 'error'; t.el('cp-add').click(); await t.advance(100);"
            " return [globalThis.puts, t.el('cp-new').value, t.el('cp-rows').children.length,"
            " t.el('known-tags').children.map(o => o.value)];"
        )
        assert r[0] == [
            {"components": {"*": "info", "ziggurat": "debug", "zigbee": "error"}}
        ]
        assert r[1] == ""
        assert r[2] == 3
        assert "zigbee" not in r[3]

    def test_adding_a_bad_tag_sends_nothing_and_says_why(self):
        r = run(
            "t.el('cp-new').type('has space'); t.el('cp-add').click(); await t.advance(100);"
            " return [globalThis.puts || null, t.el('lg-note').textContent];"
        )
        assert r[0] is None
        assert r[1] != ""

    def test_adding_a_33rd_override_sends_nothing_and_says_why(self):
        many = {f"t{i}": "info" for i in range(32)}
        r = run(
            "t.el('cp-new').type('extra'); t.el('cp-add').click(); await t.advance(100);"
            " return [globalThis.puts || null, t.el('lg-note').textContent];",
            setup=f"Object.assign(globalThis.rep.components, {json.dumps(many)});",
        )
        assert r[0] is None
        assert "32" in r[1]

    def test_a_syslog_name_still_resolving_is_looked_at_again_until_it_settles(self):
        r = run(
            "const before = t.el('sys-pill').textContent; globalThis.rep.channels.syslog.state = 'sending';"
            " await t.advance(2100);"
            " return [before, t.el('sys-pill').textContent,"
            " t.calls.filter(c => c.url === '/device/logging').length];",
            setup="globalThis.rep.channels.syslog.state = 'resolving';",
        )
        assert r == ["resolving", "sending", 2]

    # --- Live: connect, disconnect
    def test_connect_opens_the_socket_at_the_viewer_level(self):
        r = run(
            "t.el('live-toggle').click(); return [t.sockets.length, t.sockets[0].url];"
        )
        assert r == [1, "ws://board.lan/device/logging/ws?level=info"]

    def test_connect_uses_the_level_picked_before(self):
        r = run(
            "t.el('live-lv').value = 'debug'; t.el('live-lv').fire('change'); t.el('live-toggle').click();"
            " return t.sockets[0].url;"
        )
        assert r.endswith("?level=debug")

    def test_connected_shows_the_pill_the_button_and_a_notice_line(self):
        r = run(
            "t.el('live-toggle').click(); t.sockets[0].open();"
            " return [t.el('live-pill').textContent, t.el('live-pill').classList.contains('con-ok'),"
            " t.el('live-toggle').textContent, txt(t.el('live-out'))];"
        )
        assert r[:3] == ["Connected", True, "Disconnect"]
        assert "# Connected" in r[3]

    def test_disconnect_closes_the_socket_and_keeps_the_lines(self):
        r = run(
            "t.el('live-toggle').click(); const ws = t.sockets[0]; ws.open(); ws.recv("
            + LINE
            + ");"
            " t.el('live-toggle').click(); await t.advance(10000);"
            " return [ws.closedWith, t.el('live-pill').textContent, t.el('live-pill').classList.contains('con-idle'),"
            " t.el('live-toggle').textContent, txt(t.el('live-out')).includes('zigbee: up'), t.sockets.length];"
        )
        assert r == [1000, "Disconnected", True, "Connect", True, 1]

    def test_connect_again_after_disconnect_opens_a_new_socket(self):
        r = run(
            "t.el('live-toggle').click(); t.sockets[0].open(); t.el('live-toggle').click(); t.el('live-toggle').click();"
            " return t.sockets.length;"
        )
        assert r == 2

    def test_a_dropped_connection_says_so_and_does_not_reconnect(self):
        r = run(
            "t.el('live-toggle').click(); t.sockets[0].open(); t.sockets[0].drop(1006); await t.advance(30000);"
            " return [t.el('live-pill').textContent, t.el('live-toggle').textContent, t.sockets.length,"
            " txt(t.el('live-out')).includes('lost')];"
        )
        assert r == ["Disconnected", "Connect", 1, True]

    # --- Live: viewer limit
    def test_a_close_with_1013_says_there_are_too_many_viewers(self):
        r = run(
            "t.el('live-toggle').click(); t.sockets[0].open(); t.sockets[0].drop(1013);"
            " return [t.el('live-pill').textContent, txt(t.el('live-out'))];"
        )
        assert r[0] == "Disconnected"
        assert "too many viewers" in r[1].lower()

    def test_a_refused_handshake_with_all_viewer_slots_taken_says_so(self):
        r = run(
            "globalThis.rep.viewers = 2; t.el('live-toggle').click(); t.sockets[0].drop(1006); await t.advance(100);"
            " return [t.el('live-pill').textContent, txt(t.el('live-out'))];"
        )
        assert r[0] == "Disconnected"
        assert "too many viewers" in r[1].lower()

    def test_a_refused_handshake_with_a_403_says_the_interface_does_not_serve_the_device_api(
        self,
    ):
        r = run(
            "globalThis.st = 403; t.el('live-toggle').click(); t.sockets[0].drop(1006); await t.advance(100);"
            " return txt(t.el('live-out'));"
        )
        assert "device API" in r

    # --- Live: lines
    @pytest.mark.parametrize(
        "frame,cls",
        [
            ("E (1) a: x", "lg-E"),
            ("W (1) a: x", "lg-W"),
            ("I (1) a: x", "lg-I"),
            ("D (1) a: x", "lg-D"),
            ("V (1) a: x", "lg-V"),
            ("# dropped 3 lines", "lg-note"),
        ],
    )
    def test_lines_are_coloured_by_level_and_notices_are_notices(self, frame, cls):
        r = run(
            "t.el('live-toggle').click(); t.sockets[0].open(); t.sockets[0].recv("
            + json.dumps(frame)
            + "); const rows = t.el('live-out').children; return [rows[rows.length - 1].className, txt(rows[rows.length - 1])];"
        )
        assert r[0] == cls
        assert frame in r[1]

    def test_the_level_change_is_sent_as_a_text_frame_while_connected(self):
        r = run(
            "t.el('live-toggle').click(); const ws = t.sockets[0]; ws.open();"
            " t.el('live-lv').value = 'warning'; t.el('live-lv').fire('change'); return ws.sent;"
        )
        assert r == ['{"level":"warning"}']

    def test_the_level_change_while_disconnected_sends_nothing_and_opens_nothing(self):
        r = run(
            "t.el('live-lv').value = 'warning'; t.el('live-lv').fire('change'); return t.sockets.length;"
        )
        assert r == 0

    def test_clear_empties_the_view(self):
        r = run(
            "t.el('live-toggle').click(); t.sockets[0].open(); t.sockets[0].recv("
            + LINE
            + "); t.el('live-clear').click();"
            " return [t.el('live-out').children.length, t.el('live-count').textContent];"
        )
        assert r[0] == 0
        assert r[1].startswith("0 lines")

    def test_filter_shows_only_matching_lines_without_case_and_restores_them(self):
        r = run(
            "t.el('live-toggle').click(); const ws = t.sockets[0]; ws.open(); ws.recv('I (1) zigbee: up'); ws.recv('I (2) netcfg: up');"
            " t.el('live-filter').type('ZIG'); const a = t.el('live-out').children.map(txt);"
            " ws.recv('W (3) netcfg: down'); ws.recv('W (4) zigbee: down'); const b = t.el('live-out').children.length;"
            " t.el('live-filter').type(''); return [a, b, t.el('live-out').children.length];"
        )
        assert len(r[0]) == 1 and "zigbee: up" in r[0][0]
        assert r[1] == 2
        assert r[2] == 5  # connect notice + 4 lines

    def test_copy_all_copies_every_line_in_order_even_those_a_filter_hides(self):
        r = run(
            "t.el('live-toggle').click(); const ws = t.sockets[0]; ws.open(); ws.recv('I (1) a: one'); ws.recv('I (2) b: two');"
            " t.el('live-filter').type('one'); t.el('live-copy').click(); await t.advance(10);"
            " return [t.clipboard.written[0].split('\\n').slice(1), t.el('live-copy').textContent];"
        )
        assert r == [["I (1) a: one", "I (2) b: two"], "Copied"]

    # --- Live: pause and the cap
    def test_pause_stops_scrolling_while_lines_keep_arriving(self):
        r = run(
            "const out = t.el('live-out'); out.scrollHeight = 500; t.el('live-toggle').click(); const ws = t.sockets[0]; ws.open();"
            " ws.recv('I (1) a: x'); const before = out.scrollTop; t.el('live-pause').click(); out.scrollHeight = 900;"
            " ws.recv('I (2) a: y'); ws.recv('I (3) a: z');"
            " const paused = [out.scrollTop, out.children.length, t.el('live-pause').textContent];"
            " t.el('live-pause').click();"
            " return [before, paused, out.scrollTop, t.el('live-pause').textContent];"
        )
        assert r == [500, [500, 4, "Resume"], 900, "Pause"]

    def test_the_view_keeps_at_most_5000_lines_and_drops_the_oldest(self):
        r = run(
            "t.el('live-toggle').click(); const ws = t.sockets[0]; ws.open(); t.el('live-pause').click();"
            " for (let i = 0; i < 5010; i++) ws.recv('I (' + i + ') a: n' + i);"
            " const rows = t.el('live-out').children;"
            " return [rows.length, txt(rows[0]), txt(rows[rows.length - 1])];"
        )
        assert r[0] == 5000
        assert "n10" in r[1]
        assert "n5009" in r[2]

    # --- Live: fullscreen
    def test_fullscreen_fills_the_viewport_and_the_button_leaves_it(self):
        r = run(
            "t.el('live-fs').click(); const a = [t.el('live').classList.contains('lg-fs'), t.el('live-fs').textContent];"
            " t.el('live-fs').click(); return a.concat([t.el('live').classList.contains('lg-fs'), t.el('live-fs').textContent]);"
        )
        assert r == [True, "Exit fullscreen", False, "⛶ Fullscreen"]

    def test_escape_leaves_fullscreen(self):
        r = run(
            "t.el('live-fs').click(); t.doc.fire('keydown', {key: 'Escape'}); return t.el('live').classList.contains('lg-fs');"
        )
        assert r is False

    def test_other_keys_leave_fullscreen_alone(self):
        r = run(
            "t.el('live-fs').click(); t.doc.fire('keydown', {key: 'a'}); return t.el('live').classList.contains('lg-fs');"
        )
        assert r is True
