"""Packet capture page (BDD: CaptureLogic, CapturePage). Runs static/capture.js under node."""

import json

import pytest
from device_js import logic, needs_node, page, scenario


def cap(expression):
    return logic("capture", expression)


ACTIVE = "Object.assign(H.fixture(H.STATUS), {mode: 'SNIFFER', sniffer: {active: true, ended_by_fault: false, channel: 15, frames: 1284, drops: 2, elapsed_s: 42, duration_s: 60}})"


def status_routes(status_js="H.STATUS"):
    return f"(u, o) => u === '/device/status' ? ({{status: 200, body: {status_js}}}) : o.method === 'DELETE' ? ({{status: 200, body: {{}}}}) : ({{status: 200, body: H.NETWORK}})"


@needs_node
class TestCaptureLogic:
    """CaptureLogic pure functions"""

    def test_capture_url_carries_channel_and_duration(self):
        # On a valid channel and duration: /device/sniffer.pcap with both query parameters.
        assert (
            cap("L.captureUrl('15', 60)")
            == "/device/sniffer.pcap?channel=15&duration=60"
        )

    @pytest.mark.parametrize("channel", ["", "  "])
    def test_empty_channel_leaves_the_channel_to_the_board(self, channel):
        # On an empty channel: no channel parameter, so the board uses its Thread channel.
        assert (
            cap(f"L.captureUrl({json.dumps(channel)}, 600)")
            == "/device/sniffer.pcap?duration=600"
        )

    @pytest.mark.parametrize("channel", ["10", "27", "abc"])
    def test_capture_url_refuses_a_channel_outside_11_to_26(self, channel):
        # On a channel outside 11..26: no URL.
        assert cap(f"L.captureUrl({json.dumps(channel)}, 60)") is None

    @pytest.mark.parametrize("duration", ["0", "3601", "x"])
    def test_capture_url_refuses_a_duration_outside_1_to_3600(self, duration):
        # On a duration outside 1..3600: no URL.
        assert cap(f"L.captureUrl('15', {json.dumps(duration)})") is None

    def test_live_command_streams_the_capture_into_wireshark(self):
        # On a host and a capture URL: curl -N to that URL piped into wireshark reading stdin.
        assert (
            cap("L.liveCommand('board.lan', '/device/sniffer.pcap?duration=60')")
            == "curl -N 'http://board.lan/device/sniffer.pcap?duration=60' | wireshark -k -i -"
        )

    def test_active_view_has_time_left_and_progress(self):
        # On 42 of 60 s: 0:18 left, 70 % done.
        v = cap(
            "L.activeView({channel: 15, frames: 9, drops: 1, elapsed_s: 42, duration_s: 60})"
        )
        assert v == {
            "title": "Capturing on channel 15",
            "left": "0:18",
            "frames": "9",
            "drops": "1",
            "pct": 70,
        }


@needs_node
class TestCapturePage:
    """capture.js run against a fake DOM and fetch"""

    def test_idle_status_shows_the_form_with_one_minute_chosen(self):
        # On an idle board: the form is shown, the running card hidden, 1 min pressed and in the link and command.
        r = scenario(
            page("capture", status_routes())
            + """
            await t.page.refreshStatus();
            return [t.el('cap-idle').hidden(), t.el('cap-active').hidden(), t.el('len-60').getAttribute('aria-pressed'),
                    t.el('capStart').getAttribute('href'), t.el('cap-cmd').textContent];
        """
        )
        assert r[:3] == [False, True, "true"]
        assert r[3] == "/device/sniffer.pcap?duration=60"
        assert (
            r[4]
            == "curl -N 'http://board.lan/device/sniffer.pcap?duration=60' | wireshark -k -i -"
        )

    def test_active_capture_shows_the_running_card(self):
        # On an active capture: the running card with channel, time left, frames, drops and the bar; no form.
        r = scenario(
            page("capture", status_routes(ACTIVE))
            + """
            await t.page.refreshStatus();
            return [t.el('cap-idle').hidden(), t.el('cap-active').hidden(), t.el('cap-a-title').textContent,
                    t.el('cap-a-left').textContent, t.el('cap-a-frames').textContent, t.el('cap-a-drops').textContent,
                    t.el('cap-a-fill').style.width];
        """
        )
        assert r == [True, False, "Capturing on channel 15", "0:18", "1284", "2", "70%"]

    def test_capture_ended_by_a_fault_is_reported(self):
        # On ended_by_fault: the fault line is shown; otherwise hidden.
        r = scenario(
            page(
                "capture",
                status_routes(
                    "(() => { const s = H.fixture(H.STATUS); s.sniffer.ended_by_fault = true; return s; })()"
                ),
            )
            + "await t.page.refreshStatus(); return t.el('cap-fault').hidden();"
        )
        assert r is False

    def test_length_choice_updates_the_link_and_the_command(self):
        # On pressing 10 min: aria-pressed moves and both the link and the command carry duration=600.
        r = scenario(
            page("capture", status_routes())
            + """
            t.page.init(); await t.advance(100);
            t.el('len-600').click();
            return [t.el('len-600').getAttribute('aria-pressed'), t.el('len-60').getAttribute('aria-pressed'),
                    t.el('capStart').getAttribute('href'), t.el('cap-cmd').textContent.includes('duration=600')];
        """
        )
        assert r == ["true", "false", "/device/sniffer.pcap?duration=600", True]

    def test_typed_channel_goes_into_the_link(self):
        # On typing 20 as the channel: the link carries channel=20.
        r = scenario(
            page("capture", status_routes())
            + "t.page.init(); await t.advance(100); t.el('capChannel').type('20'); return t.el('capStart').getAttribute('href');"
        )
        assert r == "/device/sniffer.pcap?channel=20&duration=60"

    def test_start_is_let_through_on_a_running_border_router(self):
        # On Start with BR_RUNNING: the click is not prevented and the message says the browser saves the file.
        r = scenario(
            page("capture", status_routes())
            + """
            t.page.init(); await t.advance(100);
            const ev = t.el('capStart').click();
            return [ev.defaultPrevented, t.el('capMsg').textContent];
        """
        )
        assert r[0] is False
        assert "sniffer.pcap" in r[1]

    def test_bad_channel_blocks_the_start(self):
        # On channel 30: the click is prevented and a message names the range.
        r = scenario(
            page("capture", status_routes())
            + """
            t.page.init(); await t.advance(100); t.el('capChannel').type('30');
            const ev = t.el('capStart').click();
            return [ev.defaultPrevented, t.el('capMsg').textContent];
        """
        )
        assert r[0] is True
        assert "11 to 26" in r[1]

    def test_start_is_blocked_while_a_capture_runs(self):
        # On Start in SNIFFER: prevented, "already running".
        r = scenario(
            page("capture", status_routes(ACTIVE))
            + "t.page.init(); await t.advance(100); const ev = t.el('capStart').click(); return [ev.defaultPrevented, t.el('capMsg').textContent];"
        )
        assert r[0] is True
        assert "already running" in r[1]

    def test_stop_sends_delete_to_the_sniffer_endpoint(self):
        # On Stop: DELETE /device/sniffer and "Capture stopped.".
        r = scenario(
            page("capture", status_routes(ACTIVE))
            + """
            await t.page.stopCapture();
            const del = t.calls.find((c) => c.method === 'DELETE');
            return [del.url, t.el('capMsg').textContent];
        """
        )
        assert r == ["/device/sniffer", "Capture stopped."]

    def test_status_failure_shows_the_stale_line(self):
        # On a failing status poll: the stale line shows the problem.
        r = scenario(
            page("capture", "() => { throw new Error('down'); }")
            + "await t.page.refreshStatus(); return [t.el('cap-stale').hidden(), t.el('cap-stale').textContent];"
        )
        assert r[0] is False
        assert "No answer" in r[1]

    def test_init_polls_status_and_the_trial_banner(self):
        # On init: GET /device/status and GET /device/network.
        r = scenario(
            page("capture", status_routes())
            + "t.page.init(); await t.advance(100); return [...new Set(t.calls.map((c) => c.url))].sort();"
        )
        assert r == ["/device/network", "/device/status"]
