"""Firmware page (BDD: FirmwareLogic, FirmwarePage). Runs static/firmware.js under node."""

import pytest

from device_js import logic, needs_node, page, scenario


def fw(expression):
    return logic("firmware", expression)


def routes(status_js="H.STATUS", other="{status: 200, body: {status: 'ok'}}"):
    return f"(u, o) => u === '/device/status' ? ({{status: 200, body: {status_js}}}) : u === '/device/network' ? ({{status: 200, body: H.NETWORK}}) : ({other})"


def status_with(js):
    return f"(() => {{ const s = H.fixture(H.STATUS); {js}; return s; }})()"


@needs_node
class TestFirmwareLogic:
    """FirmwareLogic pure functions"""

    def test_radio_url_percent_encodes_the_source_url(self):
        # On a from_url: the query holds the encoded URL and the path is the single firmware endpoint.
        assert (
            fw("L.radioFirmwareUrl('http://h/a b.bin?x=1&y=2')")
            == "/device/radio/firmware?from_url=http%3A%2F%2Fh%2Fa%20b.bin%3Fx%3D1%26y%3D2"
        )

    def test_radio_url_without_source_is_the_plain_endpoint(self):
        # On an upload: no query.
        assert fw("L.radioFirmwareUrl('')") == "/device/radio/firmware"

    def test_https_update_url_gets_a_clear_message(self):
        # On an https URL: the check says http only.
        assert "http://" in fw("L.checkUpdateUrl('https://example.org/x.bin')")

    def test_http_update_url_is_accepted(self):
        # On an http URL: no message.
        assert fw("L.checkUpdateUrl('http://example.org/x.bin')") is None

    def test_empty_update_url_gets_a_message(self):
        # On an empty URL: a message.
        assert fw("L.checkUpdateUrl('  ')")

    @pytest.mark.parametrize("reason", ["'no rcp firmware'", "'rcp incompatible'"])
    def test_radio_firmware_faults_highlight_the_radio_card(self, reason):
        # On "no rcp firmware" / "rcp incompatible": highlight with a text.
        hint = fw(f"L.faultHint({reason})")
        assert hint["highlight"] is True
        assert hint["text"]

    @pytest.mark.parametrize("reason", ["null", "'other'"])
    def test_other_fault_reasons_do_not_highlight(self, reason):
        # On no or another fault reason: no highlight.
        assert fw(f"L.faultHint({reason})")["highlight"] is False

    def test_recovery_follows_the_jumper(self):
        # On jumper present true/false/missing: true only for true.
        assert fw("[L.jumperPresent({jumper: {present: true}}), L.jumperPresent({jumper: {present: false}}), L.jumperPresent({})]") == [True, False, False]

    def test_rcp_installed_text_tells_none_unknown_and_version(self):
        # On rcp.installed "" / null / "1.4": no firmware / unknown / the version.
        assert "no" in fw("L.rcpInstalledText('')").lower()
        assert "unknown" in fw("L.rcpInstalledText(null)").lower()
        assert fw("L.rcpInstalledText('1.4')") == "1.4"


@needs_node
class TestFirmwarePage:
    """firmware.js run against a fake DOM and fetch"""

    def test_status_fills_both_cards(self):
        # On a status answer: P4 version and slot, RCP version and match.
        r = scenario(
            page("firmware", routes())
            + """
            await t.page.refreshStatus();
            return ['p4-version','p4-slot','rcp-version','rcp-compat'].map((i) => t.el(i).textContent);
        """
        )
        assert r == ["1.0", "Slot 0", "2.1", "Yes"]

    @pytest.mark.parametrize(
        ("installed", "expected"),
        [("null", "unknown (probe failed)"), ("''", "none (no valid RCP firmware)"), ("'2.1'", "2.1")],
    )
    def test_rcp_installed_renders_through_the_radio_card(self, installed, expected):
        # On rcp.installed null / "" / string: the radio card shows unknown / none / the version.
        r = scenario(
            page("firmware", routes(status_with(f"s.rcp.installed = {installed}")))
            + "await t.page.refreshStatus(); return t.el('rcp-version').textContent;"
        )
        assert r == expected

    def test_fault_no_rcp_firmware_outlines_the_radio_card(self):
        # On "no rcp firmware": the hint is shown and the card outlined; cleared on a healthy status.
        r = scenario(
            page("firmware", "(u) => ({status: 200, body: t.s || H.STATUS})")
            + """
            t.s = H.fixture(H.STATUS, {fault_reason: 'no rcp firmware'});
            await t.page.refreshStatus(); const on = [t.el('rcpCard').style.outline, !t.el('radioHint').hidden()];
            t.s = null; await t.page.refreshStatus();
            return [on, t.el('rcpCard').style.outline, t.el('radioHint').hidden()];
        """
        )
        assert r[0][0] and r[0][1] is True
        assert r[1:] == ["", True]

    def test_recovery_controls_follow_the_jumper(self):
        # On jumper absent: explanation shown, controls hidden; present: the reverse.
        r = scenario(
            page("firmware", "(u) => ({status: 200, body: t.s})")
            + """
            t.s = H.fixture(H.STATUS); t.s.jumper.present = false; await t.page.refreshStatus();
            const off = [t.el('noJumper').hidden(), t.el('jumperControls').hidden(), t.el('jumperChip').textContent];
            t.s = H.STATUS; await t.page.refreshStatus();
            return [off, [t.el('noJumper').hidden(), t.el('jumperControls').hidden(), t.el('jumperChip').textContent]];
        """
        )
        assert r == [[False, True, "Jumper not fitted"], [True, False, "Jumper fitted"]]

    def test_server_text_is_rendered_as_text(self):
        # On a version with markup: it lands in textContent unchanged.
        r = scenario(
            page("firmware", routes(status_with("s.app.version = '<img src=x>'")))
            + "await t.page.refreshStatus(); return t.el('p4-version').textContent;"
        )
        assert r == "<img src=x>"

    def test_bundle_file_is_sent_as_octet_stream_and_waits_for_the_reboot(self):
        # On Install with a chosen file: POST /device/ota with that same object; then busy and waiting.
        r = scenario(
            page("firmware", routes())
            + """
            t.page.init(); await t.advance(100);
            const file = {name: 'otbr.bundle', size: 1234};
            t.el('otaFile').files = [file];
            await t.page.install('ota');
            const c = t.calls.find((x) => x.url === '/device/ota');
            return [c.method, c.body === file, c.headers['Content-Type'], t.page.kit.isBusy(), t.page.kit.isWaiting()];
        """
        )
        assert r == ["POST", True, "application/octet-stream", True, True]

    def test_url_source_sends_the_url_as_json(self):
        # On From a URL for the bundle: POST /device/ota/url with {"url"} and a JSON content type.
        r = scenario(
            page("firmware", routes())
            + """
            t.page.init(); await t.advance(100);
            t.el('ota-src-url').click(); t.el('otaUrl').value = ' http://h/b.bin ';
            await t.page.install('ota');
            const c = t.calls.find((x) => x.url === '/device/ota/url');
            return [t.el('ota-url-pane').hidden(), t.el('ota-file-pane').hidden(), JSON.parse(c.body), c.headers['Content-Type']];
        """
        )
        assert r == [False, True, {"url": "http://h/b.bin"}, "application/json"]

    def test_radio_from_url_sends_an_empty_body_and_the_encoded_query(self):
        # On From a URL for the radio: POST with no body and from_url percent-encoded; the result is shown.
        r = scenario(
            page("firmware", routes(other="{status: 200, body: {installed: '2.2'}}"))
            + """
            t.page.init(); await t.advance(100);
            t.el('rcp-src-url').click(); t.el('rcpUrl').value = 'http://h/c6 rcp.bin';
            await t.page.install('rcp');
            const c = t.calls.find((x) => x.url.startsWith('/device/radio/firmware'));
            return [c.url, c.body === undefined, t.el('rcpMsg').textContent];
        """
        )
        assert r == ["/device/radio/firmware?from_url=http%3A%2F%2Fh%2Fc6%20rcp.bin", True, "Radio firmware installed: 2.2."]

    def test_https_url_is_refused_without_a_request(self):
        # On an https source: a message and no request.
        r = scenario(
            page("firmware", routes())
            + """
            t.page.init(); await t.advance(100); const n = t.calls.length;
            t.el('ota-src-url').click(); t.el('otaUrl').value = 'https://h/b.bin';
            await t.page.install('ota');
            return [t.calls.length - n, t.el('otaMsg').textContent];
        """
        )
        assert r[0] == 0
        assert "http" in r[1]

    def test_no_file_chosen_gets_a_message(self):
        # On Install without a file: "Choose a file first." and no request.
        r = scenario(
            page("firmware", routes())
            + "await t.page.install('rcp'); return [t.calls.length, t.el('rcpMsg').textContent];"
        )
        assert r == [0, "Choose a file first."]

    def test_oversize_file_is_refused_without_a_request(self):
        # On a radio file over the C6 slot: a Too large message, no request, not busy.
        r = scenario(
            page("firmware", routes())
            + """
            t.el('rcpFile').files = [{name: 'x', size: H.Common.RCP_MAX_BYTES + 1}];
            await t.page.install('rcp');
            return [t.calls.length, t.el('rcpMsg').textContent, t.page.kit.isBusy()];
        """
        )
        assert r[0] == 0
        assert "Too large" in r[1]
        assert r[2] is False

    def test_busy_disables_every_action_and_a_failure_enables_them_again(self):
        # On an upload in flight: all actions disabled and a second install sends nothing; after a 409 enabled again.
        r = scenario(
            page("firmware", "(u) => ({status: 409, body: {error: 'busy', detail: 'x'}})")
            + """
            const ids = ['otaInstall','rcpInstall','recoverBtn','logBtn','confirmBtn'];
            t.el('otaFile').files = [{name: 'b', size: 10}];
            const first = t.page.install('ota');
            const during = ids.map((i) => t.el(i).disabled);
            const second = await t.page.install('ota');
            await first;
            return {during: during.every(Boolean), second, calls: t.calls.length,
                    after: ids.map((i) => t.el(i).disabled), msg: t.el('otaMsg').textContent};
        """
        )
        assert r["during"] is True
        assert r["second"] is None
        assert r["calls"] == 1
        assert not any(r["after"])
        assert "busy" in r["msg"].lower()

    def test_dropped_file_becomes_the_cards_file(self):
        # On a file dropped on the radio drop zone: the default is prevented, the input takes it and its name shows.
        r = scenario(
            page("firmware", routes())
            + """
            t.page.init(); await t.advance(100);
            const file = {name: 'c6_rcp.bin', size: 10};
            const ev = t.el('rcpDrop').fire('drop', {dataTransfer: {files: [file]}});
            return [ev.defaultPrevented, t.el('rcpFile').files[0] === file, t.el('rcpDropName').textContent];
        """
        )
        assert r == [True, True, "c6_rcp.bin"]

    def test_recover_posts_and_shows_the_result(self):
        # On Recover C6: POST /device/radio/recover and its detail in recoverMsg.
        r = scenario(
            page("firmware", routes(other="{status: 200, body: {recovered: true, detail: 'base rewritten'}}"))
            + """
            await t.page.recover();
            const c = t.calls.find((x) => x.url === '/device/radio/recover');
            return [c.method, t.el('recoverMsg').textContent];
        """
        )
        assert r == ["POST", "base rewritten"]

    def test_console_log_is_shown_as_text(self):
        # On Show C6 console: GET /device/radio/log, its text in the log box.
        r = scenario(
            page("firmware", "(u) => ({status: 200, body: 'ESP-ROM:esp32c6\\nboot'})")
            + "await t.page.showLog(); return t.el('c6Log').textContent;"
        )
        assert r == "ESP-ROM:esp32c6\nboot"

    def test_init_polls_status_and_the_trial_banner(self):
        # On init: GET /device/status and GET /device/network.
        r = scenario(
            page("firmware", routes())
            + "t.page.init(); await t.advance(100); return [...new Set(t.calls.map((c) => c.url))].sort();"
        )
        assert r == ["/device/network", "/device/status"]
