"""Status page, device.html (BDD: StatusLogic, StatusPage). Runs static/status.js under node."""

import json

import pytest
from device_js import logic, needs_node, page, scenario


def st(expression):
    return logic("status", expression)


def status(**patch):
    return f"Object.assign(H.fixture(H.STATUS), {json.dumps(patch)})"


ROUTES = "(u, o) => o.method === 'POST' ? ({status: 200, body: {status: 'rebooting'}}) : ({status: 200, body: u === '/device/diag' ? H.DIAG : u === '/device/network' ? H.NETWORK : (t.s || H.STATUS)})"


@needs_node
class TestStatusLogic:
    """StatusLogic pure functions"""

    def test_running_hero_is_green_without_a_link(self):
        # On BR_RUNNING: "Border router running", tone ok, no link.
        h = st("L.heroFor({mode: 'BR_RUNNING', fault_reason: null, sniffer: {}})")
        assert (h["title"], h["tone"], h["link"]) == (
            "Border router running",
            "ok",
            None,
        )

    def test_sniffer_hero_names_channel_and_frames_and_links_the_capture(self):
        # On SNIFFER: "Capturing packets", the text names channel 15 and 1284 frames, link capture.html.
        h = st(
            "L.heroFor({mode: 'SNIFFER', sniffer: {active: true, channel: 15, frames: 1284}})"
        )
        assert (h["title"], h["tone"]) == ("Capturing packets", "info")
        assert "15" in h["text"] and "1284" in h["text"]
        assert h["link"]["href"] == "/capture.html"

    @pytest.mark.parametrize(
        ("reason", "word"),
        [
            ("'no rcp firmware'", "no firmware"),
            ("'rcp incompatible'", "does not match"),
            ("'spinel timeout'", "spinel timeout"),
        ],
    )
    def test_fault_hero_explains_the_reason_and_links_firmware(self, reason, word):
        # On RADIO_FAULT: "Radio fault", the text explains the reason, link firmware.html.
        h = st(
            f"L.heroFor({{mode: 'RADIO_FAULT', fault_reason: {reason}, sniffer: {{}}}})"
        )
        assert (h["title"], h["tone"]) == ("Radio fault", "fault")
        assert word in h["text"]
        assert h["link"]["href"] == "/firmware.html"

    def test_starting_hero_is_grey(self):
        # On STARTING: "Starting", tone idle.
        h = st("L.heroFor({mode: 'STARTING', sniffer: {}})")
        assert (h["title"], h["tone"]) == ("Starting", "idle")

    def test_sniffer_text_for_active_idle_and_fault(self):
        # On active / idle with a last capture / idle without / ended by a fault: state and detail.
        assert st(
            "L.snifferText({active: true, channel: 15, frames: 9, drops: 1})"
        ) == {"state": "Capturing on channel 15", "detail": "9 frames, 1 dropped"}
        assert st("L.snifferText({active: false, frames: 3912, drops: 0})") == {
            "state": "Idle",
            "detail": "Last capture: 3912 frames, 0 dropped",
        }
        assert st("L.snifferText({active: false, frames: 0, drops: 0})") == {
            "state": "Idle",
            "detail": "No capture since boot",
        }
        assert (
            "radio fault"
            in st(
                "L.snifferText({active: false, ended_by_fault: true, frames: 3, drops: 0})"
            )["detail"]
        )

    def test_pd_text_names_the_prefix_or_none(self):
        # On a prefix / none / unreadable state: the prefix / "None received" / "unknown".
        assert st("L.pdText({state: 'running', prefix: '2001:db8:1::/64'})") == {
            "state": "running",
            "prefix": "2001:db8:1::/64",
        }
        assert (
            st("L.pdText({state: 'running', prefix: null})")["prefix"]
            == "None received"
        )
        assert "unknown" in st("L.pdText({state: null, prefix: null})")["state"]


@needs_node
class TestStatusPage:
    """status.js run against a fake DOM and fetch"""

    def test_status_fills_the_hero_and_the_tiles(self):
        # On a status answer: hero, raw mode and each tile; offline line hidden.
        r = scenario(
            page("device", ROUTES)
            + """
            await t.page.refreshStatus();
            return ['h-title','h-mode','t-app','t-slot','t-storage','t-rcp','t-compat','t-base','t-jumper','t-ipv4',
                    't-ipv6','t-pd-state','t-pd-prefix','t-sniffer-state','t-sniffer-detail']
                   .map((i) => t.el(i).textContent).concat([t.el('s-offline').hidden(), t.el('hero').className,
                   t.el('h-link').hidden()]);
        """
        )
        assert r == [
            "Border router running",
            "BR_RUNNING",
            "1.0",
            "Slot 0",
            "OK",
            "2.1",
            "Yes",
            "b1, as expected",
            "Fitted",
            "10.0.0.5",
            "fe80::1",
            "running",
            "None received",
            "Idle",
            "No capture since boot",
            True,
            "card hero hero-ok",
            True,
        ]

    def test_fault_hero_links_the_firmware_page(self):
        # On RADIO_FAULT with no RCP firmware: hero-fault, its text and the link.
        r = scenario(
            page("device", ROUTES)
            + f"t.s = {status(mode='RADIO_FAULT', fault_reason='no rcp firmware')};"
            + "await t.page.refreshStatus(); return [t.el('hero').className, t.el('h-link').hidden(), t.el('h-link').getAttribute('href')];"
        )
        assert r == ["card hero hero-fault", False, "/firmware.html"]

    def test_base_image_mismatch_names_the_expected_one(self):
        # On a base image other than the expected one: both are named; without the jumper: a dash.
        r = scenario(
            page("device", ROUTES)
            + """
            t.s = H.fixture(H.STATUS); t.s.jumper.base_installed = 'b0'; await t.page.refreshStatus();
            const mismatch = t.el('t-base').textContent;
            t.s.jumper.present = false; await t.page.refreshStatus();
            return [mismatch, t.el('t-base').textContent, t.el('t-jumper').textContent];
        """
        )
        assert r == ["b0 (expected b1)", "-", "Not fitted"]

    def test_server_text_is_rendered_as_text(self):
        # On a fault reason with markup: it lands in textContent unchanged.
        r = scenario(
            page("device", ROUTES)
            + f"t.s = {status(mode='RADIO_FAULT', fault_reason='<img src=x>')};"
            + "await t.page.refreshStatus(); return t.el('h-text').textContent;"
        )
        assert "<img src=x>" in r

    def test_diag_fills_uptime_reset_and_load(self):
        # On a diag answer: uptime in words, reset reason, 10 s CPU per core, internal and PSRAM free.
        r = scenario(
            page("device", ROUTES)
            + "await t.page.refreshDiag(); return ['h-uptime','h-reset','t-cpu','t-mem-int','t-mem-psram'].map((i) => t.el(i).textContent);"
        )
        assert r == [
            "7 h 35 min",
            "software",
            "Core 0 0.5%, core 1 0.6%",
            "408.9 KB (lowest 350.7 KB)",
            "30.2 MB",
        ]

    def test_diag_failure_leaves_dashes(self):
        # On a failing diag: the uptime stays a dash.
        r = scenario(
            page("device", "() => { throw new Error('down'); }")
            + "t.el('h-uptime').textContent = '-'; await t.page.refreshDiag(); return t.el('h-uptime').textContent;"
        )
        assert r == "-"

    def test_unanswered_status_shows_the_offline_line_with_backoff(self):
        # On failing status polls: the offline line with last-seen seconds; requests back off.
        r = scenario(
            page(
                "device",
                "(u) => { if (t.fail) throw new Error('down'); return {status: 200, body: u === '/device/diag' ? H.DIAG : u === '/device/network' ? H.NETWORK : H.STATUS}; }",
            )
            + """
            t.fail = false; t.page.init(); await t.advance(100);
            t.fail = true; await t.advance(60000);
            const statusCalls = t.calls.filter((c) => c.url === '/device/status').length;
            return [!t.el('s-offline').hidden(), t.el('s-offline').textContent, statusCalls];
        """
        )
        assert r[0] is True
        assert "No answer from the device (last seen" in r[1]
        assert r[2] <= 9  # 3 s fixed would be about 20 requests in 60 s

    def test_reboot_asks_posts_and_waits_for_the_board(self):
        # On Reboot: a confirmation, POST /device/reboot, a message, busy with a waiter.
        r = scenario(
            page("device", ROUTES)
            + """
            await t.page.reboot();
            const c = t.calls.find((x) => x.method === 'POST');
            return [t.confirms().length, c.url, t.el('rebootMsg').textContent, t.page.kit.isBusy(), t.page.kit.isWaiting()];
        """
        )
        assert r[0:2] == [1, "/device/reboot"]
        assert "reloads" in r[2]
        assert r[3:] == [True, True]

    def test_reboot_refused_shows_why(self):
        # On a 409 from reboot: the reason in rebootMsg, not busy.
        r = scenario(
            page(
                "device",
                "(u, o) => o.method === 'POST' ? ({status: 409, body: {error: 'busy', detail: 'update running'}}) : ({status: 200, body: H.STATUS})",
            )
            + "await t.page.reboot(); return [t.el('rebootMsg').textContent, t.page.kit.isBusy()];"
        )
        assert "update running" in r[0]
        assert r[1] is False

    def test_init_polls_status_diag_and_the_trial_banner(self):
        # On init: GET /device/status, /device/diag and /device/network.
        r = scenario(
            page("device", ROUTES)
            + "t.page.init(); await t.advance(100); return [...new Set(t.calls.map((c) => c.url))].sort();"
        )
        assert r == ["/device/diag", "/device/network", "/device/status"]
