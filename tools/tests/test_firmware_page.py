"""Firmware page (BDD: FirmwareLogic, FirmwarePage). Runs static/firmware.js under node."""

import json

import pytest
from device_js import FRONTEND, logic, needs_node, page, scenario


def fw(expression):
    return logic("firmware", expression)


REL = {
    "tag": "v0.4.0",
    "app": "20261108T120102Z-9f3a21c",
    "rcp": "20261108T115904Z-9f3a21c",
    "url": "https://github.com/t3hk0d3/esp32-p4-otbr/releases/tag/v0.4.0",
}


def upd(**patch):
    """GET /device/update body (JSON text): a fresh check of v0.4.0 that is newer, with the radio firmware differing."""
    body = {
        "state": "done",
        "checked_age_s": 30,
        "error": None,
        "release": dict(REL),
        "repo_url": "https://github.com/t3hk0d3/esp32-p4-otbr",
        "running": {"app": "1.0", "rcp": "2.1"},
        "app": "newer",
        "rcp_differs": True,
        "available": True,
    }
    body.update(patch)
    return json.dumps(body)


def rel(**patch):
    return dict(REL, **patch)


POST_OK = (
    "{status: 'ok', app: 'installed', rcp: 'installed', detail: '', rebooting: true}"
)


def routes(status_js="H.STATUS", other="{status: 200, body: {status: 'ok'}}"):
    # /device/update: t.upd(url, opts) when a test sets it; else the cached state t.u (default: up to date) and an OK install.
    return (
        "(u, o) => u === '/device/status' ? ({status: 200, body: " + status_js + "})"
        " : u === '/device/network' ? ({status: 200, body: H.NETWORK})"
        " : u.startsWith('/device/update') ? (t.upd ? t.upd(u, o) : o.method === 'POST' ? ({status: 200, body: "
        + POST_OK
        + "})"
        " : ({status: 200, body: t.u || "
        + upd(app="same", rcp_differs=False, available=False)
        + "}))"
        " : (" + other + ")"
    )


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

    def test_https_update_url_is_accepted(self):
        # On an https URL: no message.
        assert fw("L.checkUpdateUrl('https://example.org/x.bin')") is None

    def test_http_update_url_is_accepted(self):
        # On an http URL: no message.
        assert fw("L.checkUpdateUrl('http://example.org/x.bin')") is None

    def test_update_url_with_an_uppercase_scheme_gets_a_message(self):
        # On HTTPS:// or HTTP://: the board accepts only the lowercase scheme.
        assert fw("L.checkUpdateUrl('HTTPS://example.org/x.bin')")
        assert fw("L.checkUpdateUrl('Http://example.org/x.bin')")

    def test_empty_update_url_gets_a_message(self):
        # On an empty URL: a message.
        assert fw("L.checkUpdateUrl('  ')")

    @pytest.mark.parametrize(
        "reason", ["'no rcp firmware'", "'rcp incompatible'", "'esp-hosted firmware'"]
    )
    def test_radio_firmware_faults_highlight_the_radio_card(self, reason):
        # On "no rcp firmware" / "rcp incompatible" / "esp-hosted firmware": highlight with a text.
        hint = fw(f"L.faultHint({reason})")
        assert hint["highlight"] is True
        assert hint["text"]

    def test_esp_hosted_fault_asks_for_the_installer_first(self):
        # On "esp-hosted firmware": the text names c6_installer.bin before c6_rcp.bin.
        text = fw("L.faultHint('esp-hosted firmware')")["text"]
        assert text.index("c6_installer.bin") < text.index("c6_rcp.bin")

    @pytest.mark.parametrize(
        "reason", ["'no rcp firmware'", "'rcp incompatible'", "'esp-hosted firmware'"]
    )
    def test_fault_of_a_usb_radio_running_thread_gives_no_hint(self, reason):
        # On a fault while Thread runs on a USB radio: no highlight, no text (c6_rcp.bin would not help).
        hint = fw(f"L.faultHint({reason}, 'usb-10c4-ea60-1')")
        assert hint == {"highlight": False, "text": ""}

    def test_fault_with_thread_on_the_c6_still_highlights(self):
        # On "rcp incompatible" with otbr_channel "builtin-c6": the C6 card is highlighted, as without a channel.
        assert fw("L.faultHint('rcp incompatible', 'builtin-c6')")["highlight"] is True

    @pytest.mark.parametrize("reason", ["null", "'other'"])
    def test_other_fault_reasons_do_not_highlight(self, reason):
        # On no or another fault reason: no highlight.
        assert fw(f"L.faultHint({reason})")["highlight"] is False

    def test_recovery_follows_the_jumper(self):
        # On jumper present true/false/missing: true only for true.
        assert fw(
            "[L.jumperPresent({jumper: {present: true}}), L.jumperPresent({jumper: {present: false}}), L.jumperPresent({})]"
        ) == [True, False, False]

    def test_rcp_installed_text_tells_none_unknown_and_version(self):
        # On rcp.installed "" / null / "1.4": no firmware / unknown / the version.
        assert "no" in fw("L.rcpInstalledText('')").lower()
        assert "unknown" in fw("L.rcpInstalledText(null)").lower()
        assert fw("L.rcpInstalledText('1.4')") == "1.4"


@needs_node
class TestUpdateLogic:
    """FirmwareLogic: the Updates card"""

    @pytest.mark.parametrize(
        ("patch", "expected"),
        [
            ({"state": "never", "checked_age_s": None, "release": None}, True),
            ({"checked_age_s": 3601}, True),
            ({"checked_age_s": 3600}, False),
            ({"state": "checking", "checked_age_s": 3601}, False),
        ],
    )
    def test_update_check_is_needed_when_never_run_or_older_than_an_hour(
        self, patch, expected
    ):
        # On state never / age 3601 / age 3600 / checking: true / true / false / false.
        assert fw(f"L.updateNeedsCheck({upd(**patch)})") is expected

    @pytest.mark.parametrize(
        ("patch", "text", "cls"),
        [
            ({"state": "checking"}, "Checking", ""),
            ({"state": "failed"}, "Check failed", "chip-bad"),
            ({"app": "newer"}, "v0.4.0 available", "chip-warn"),
            ({"app": "unknown"}, "v0.4.0 available", "chip-warn"),
            (
                {"app": "unknown", "release": rel(tag=None)},
                "Update available",
                "chip-warn",
            ),
            ({"app": "same"}, "Radio firmware differs", "chip-warn"),
            ({"app": "older"}, "Newer than the release", ""),
            ({"app": "same", "rcp_differs": False}, "Up to date", "chip-ok"),
            ({"state": "never", "release": None, "app": None}, "Not checked", ""),
        ],
    )
    def test_chip_tells_the_state_of_the_check(self, patch, text, cls):
        # On each state of the check: the chip text and class.
        assert fw(f"L.updateChip({upd(**patch)})") == {"text": text, "cls": cls}

    def test_version_row_shows_the_installed_one_under_a_different_release_version(
        self,
    ):
        # On different versions: the release value and "installed: <installed>"; newer appends " (newer)".
        assert fw("L.versionRow('A', 'B', false)") == {
            "value": "A",
            "was": "installed: B",
        }
        assert fw("L.versionRow('A', 'B', true)") == {
            "value": "A",
            "was": "installed: B (newer)",
        }

    def test_version_row_for_equal_versions(self):
        # On equal versions: "same as installed".
        assert fw("L.versionRow('A', 'A', false)") == {
            "value": "A",
            "was": "same as installed",
        }

    def test_version_row_without_a_release_version(self):
        # On a release without a c6_rcp line: a dash and nothing under it.
        assert fw("L.versionRow(null, 'B', false)") == {"value": "-", "was": ""}

    def test_version_row_with_an_unknown_installed_version(self):
        # On installed null: "installed: unknown".
        assert fw("L.versionRow('A', null, false)")["was"] == "installed: unknown"

    def test_checked_text_without_an_age_is_a_dash(self):
        assert fw(f"L.checkedText({upd(checked_age_s=None)})") == "-"

    @pytest.mark.parametrize(
        ("age", "text"), [(10, "just now"), (720, "12 min ago"), (10800, "3 h ago")]
    )
    def test_checked_text_tells_the_age(self, age, text):
        # On age 10 s / 720 s / 10800 s: just now / 12 min ago / 3 h ago.
        assert fw(f"L.checkedText({upd(checked_age_s=age)})") == text

    def test_checked_text_of_a_failed_check_says_so(self):
        # On a failed check with an age: the age text and " (the last check failed)".
        assert (
            fw(f"L.checkedText({upd(state='failed', checked_age_s=10800)})")
            == "3 h ago (the last check failed)"
        )

    @pytest.mark.parametrize(
        ("patch", "expected"),
        [
            (
                {"app": "newer"},
                {"label": "Install v0.4.0 and reboot", "primary": True, "force": False},
            ),
            (
                {"app": "unknown"},
                {"label": "Install v0.4.0 and reboot", "primary": True, "force": False},
            ),
            (
                {"app": "same"},
                {
                    "label": "Install the radio firmware of v0.4.0",
                    "primary": True,
                    "force": False,
                },
            ),
            (
                {"app": "older"},
                {"label": "Install v0.4.0 anyway", "primary": False, "force": True},
            ),
            (
                {"app": "newer", "release": rel(tag=None)},
                {
                    "label": "Install the latest release and reboot",
                    "primary": True,
                    "force": False,
                },
            ),
        ],
    )
    def test_action_offers_the_install_that_fits(self, patch, expected):
        # On app newer / unknown / same with the radio differing / older / no tag: the label, primary and force.
        assert fw(f"L.updateAction({upd(**patch)})") == expected

    @pytest.mark.parametrize(
        "patch",
        [
            {"app": "same", "rcp_differs": False},
            {"release": None, "app": None, "state": "never"},
            {"state": "checking"},
        ],
    )
    def test_no_action_when_up_to_date_without_a_release_or_checking(self, patch):
        # On up to date / no release / checking: no action (the Install button is hidden).
        assert fw(f"L.updateAction({upd(**patch)})") is None

    def test_hint_for_both_pieces_names_everything(self):
        # On app and rcp to install: the bundle, the radio firmware, the half minute and the reboot.
        h = fw(f"L.updateHint({upd()})")
        assert (
            "bundle" in h
            and "radio firmware" in h
            and "half a minute" in h
            and "reboot" in h
        )

    def test_hint_for_the_app_only_has_no_radio_sentence(self):
        h = fw(f"L.updateHint({upd(rcp_differs=False)})")
        assert "radio" not in h.lower() and "reboot" in h

    def test_hint_for_the_radio_only_has_no_reboot_sentence(self):
        h = fw(f"L.updateHint({upd(app='same')})")
        assert "reboot" not in h and "half a minute" in h

    def test_hint_for_a_newer_running_build_names_the_radio_rewrite_when_the_release_has_one(
        self,
    ):
        # On "Install anyway" with a c6_rcp in the release: force rewrites the radio even when it matches.
        h = fw(f"L.updateHint({upd(app='older', rcp_differs=False)})")
        assert "rewrites the radio firmware" in h and "half a minute" in h

    def test_hint_for_a_newer_running_build_without_radio_firmware_has_no_radio_sentence(
        self,
    ):
        h = fw(
            f"L.updateHint({upd(app='older', rcp_differs=False, release=rel(rcp=''))})"
        )
        assert (
            h
            == "This board runs a build that is newer than v0.4.0. Installing goes back to the release."
        )

    def test_hint_when_up_to_date(self):
        assert (
            fw(f"L.updateHint({upd(app='same', rcp_differs=False)})")
            == "Nothing to install: the board runs the latest release."
        )

    def test_question_for_both_pieces(self):
        # On app and rcp: the tag, the running stamp, the radio rewrite, the Thread stop and the reboot.
        q = fw(f"L.updateQuestion({upd()}, '20261103T185311Z-e41c14c')")
        for part in (
            "v0.4.0",
            "20261103T185311Z-e41c14c",
            "radio firmware",
            "half a minute",
            "reboot",
        ):
            assert part in q

    def test_question_for_the_radio_only_has_no_reboot(self):
        q = fw(f"L.updateQuestion({upd(app='same')}, 'X')")
        assert "v0.4.0" in q and "half a minute" in q and "reboot" not in q

    def test_question_for_a_newer_running_build(self):
        q = fw(f"L.updateQuestion({upd(app='older')}, 'X')")
        assert "newer than this release" in q

    def test_question_for_a_newer_running_build_names_the_radio_rewrite_when_the_release_has_one(
        self,
    ):
        q = fw(f"L.updateQuestion({upd(app='older', rcp_differs=False)}, 'X')")
        assert "radio firmware is rewritten too" in q

    def test_question_for_a_newer_running_build_without_radio_firmware_has_no_radio_sentence(
        self,
    ):
        q = fw(
            f"L.updateQuestion({upd(app='older', rcp_differs=False, release=rel(rcp=''))}, 'X')"
        )
        assert "radio" not in q

    def test_radio_failure_after_a_rollback_does_not_claim_the_border_router_was_updated(
        self,
    ):
        assert (
            fw("L.rcpFailedText('cut off', false)")
            == "The radio firmware was not updated either: cut off."
        )

    def test_check_failed_text_names_the_cause(self):
        assert (
            fw(
                f"L.checkFailedText({upd(state='failed', error='TLS handshake failed (url)')})"
            )
            == "The check failed: TLS handshake failed (url). The board needs internet access and DNS."
        )

    def test_reply_with_a_reboot(self):
        # On rebooting true, rcp installed: the reload text and no radio failure to keep.
        r = fw(
            "L.updateReplyText({status: 200, body: {status: 'ok', app: 'installed', rcp: 'installed', detail: '', rebooting: true}}, 'S')"
        )
        assert (
            r["text"]
            == "Update installed. The board is rebooting; this page reloads when it is back."
        )
        assert r["rebooting"] is True and r["rcpFailed"] is None

    def test_reply_with_a_failed_radio_keeps_the_detail(self):
        r = fw(
            "L.updateReplyText({status: 200, body: {status: 'ok', app: 'installed', rcp: 'failed', detail: 'cut off', rebooting: true}}, 'S')"
        )
        assert (
            r["text"]
            == "Update installed. The board is rebooting; this page reloads when it is back."
        )
        assert r["rcpFailed"] == "cut off"

    def test_reply_without_a_reboot_names_the_stamp_of_the_check(self):
        r = fw(
            "L.updateReplyText({status: 200, body: {status: 'ok', app: 'unchanged', rcp: 'installed', detail: '', rebooting: false}}, 'S1')"
        )
        assert r["text"] == "Radio firmware installed: S1." and r["rebooting"] is False

    def test_reply_that_is_not_200_is_left_to_the_failure_text(self):
        assert (
            fw("L.updateReplyText({status: 409, body: {error: 'busy'}}, 'S')") is None
        )

    def test_repo_label_drops_the_host(self):
        assert (
            fw("L.repoLabel('https://github.com/t3hk0d3/esp32-p4-otbr')")
            == "t3hk0d3/esp32-p4-otbr"
        )


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
        [
            ("null", "unknown (probe failed)"),
            ("''", "none (no valid RCP firmware)"),
            ("'2.1'", "2.1"),
        ],
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
            return [t.el('ota-url-pane').hidden(), t.el('ota-file-pane').hidden(), JSON.parse(c.body), c.headers['Content-Type'],
                    t.page.kit.isBusy(), t.page.kit.isWaiting()];
        """
        )
        assert r == [
            False,
            True,
            {"url": "http://h/b.bin"},
            "application/json",
            True,
            True,
        ]

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
        assert r == [
            "/device/radio/firmware?from_url=http%3A%2F%2Fh%2Fc6%20rcp.bin",
            True,
            "Radio firmware installed: 2.2.",
        ]

    def test_ftp_url_is_refused_without_a_request(self):
        # On a source that is neither http nor https: a message and no request.
        r = scenario(
            page("firmware", routes())
            + """
            t.page.init(); await t.advance(100); const n = t.calls.length;
            t.el('ota-src-url').click(); t.el('otaUrl').value = 'ftp://h/b.bin';
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
            page(
                "firmware", "(u) => ({status: 409, body: {error: 'busy', detail: 'x'}})"
            )
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
            page(
                "firmware",
                routes(
                    other="{status: 200, body: {recovered: true, detail: 'base rewritten'}}"
                ),
            )
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
        assert r == ["/device/network", "/device/status", "/device/update"]


@needs_node
class TestFirmwarePageReview:
    """findings of the whole-branch review"""

    def test_file_dropped_beside_the_drop_zone_does_not_leave_the_page(self):
        # On dragover and drop on the document: the browser default (open the file) is prevented.
        r = scenario(
            page("firmware", routes())
            + "t.page.init(); await t.advance(100); return [t.doc.fire('dragover').defaultPrevented, t.doc.fire('drop').defaultPrevented];"
        )
        assert r == [True, True]

    def test_console_box_is_hidden_until_the_log_is_shown(self):
        # On the page: the empty console box is hidden; after Show C6 console it is shown.
        r = scenario(
            page("firmware", "(u) => ({status: 200, body: 'boot'})")
            + "const before = t.el('c6Log').hidden(); await t.page.showLog(); return [before, t.el('c6Log').hidden()];"
        )
        assert r == [True, False]


def test_recovery_text_asks_for_a_reinstall_only_without_radio_firmware():
    # On the recovery card: the reinstall is conditional on the Radio Co-Processor card showing no firmware.
    from device_js import FRONTEND

    html = (FRONTEND / "firmware.html").read_text()
    assert "Afterwards install the radio firmware again" not in html
    assert "If the Radio Co-Processor card then shows no installed firmware" in html


@needs_node
class TestInstallConfirms:
    """UX review: every install and the recovery ask first; the version change is shown after the reboot"""

    def test_bundle_question_names_the_file_and_the_running_version(self):
        q = fw("L.installQuestion('ota', 'otbr.bundle', '0.2.0')")
        assert "otbr.bundle" in q and "0.2.0" in q and "reboot" in q

    def test_radio_question_says_thread_stops(self):
        assert "stops" in fw("L.installQuestion('rcp', 'c6_rcp.bin', '0.2.0')")

    def test_installer_question_warns_not_to_power_off(self):
        q = fw(
            "L.installQuestion('rcp', 'c6_installer.bin', '0.2.0', 'esp-hosted firmware')"
        )
        assert "power off" in q

    def test_page_passes_the_fault_reason_and_skips_the_note_while_waiting(self):
        js = (FRONTEND / "static" / "firmware.js").read_text()
        assert js.count("running, faultReason)))") == 2
        assert "kit.isWaiting() ? null : L.afterUpdateText(" in js

    def test_after_update_text(self):
        assert fw("L.afterUpdateText(null, '0.3.0')") is None
        assert (
            fw("L.afterUpdateText('0.2.0', '0.3.0')")
            == "Update done: the board now runs 0.3.0 (before: 0.2.0)."
        )
        assert "still runs 0.2.0" in fw("L.afterUpdateText('0.2.0', '0.2.0')")

    @pytest.mark.parametrize("card", ["ota", "rcp"])
    def test_cancelled_install_sends_nothing(self, card):
        r = scenario(
            page("firmware", routes())
            + f"""
            t.page.init(); await t.advance(100);
            t.env.confirm = () => false;
            t.el('{card}File').files = [{{name: 'x.bin', size: 1234}}];
            await t.page.install('{card}');
            return t.calls.filter((c) => c.method === 'POST').length;
        """
        )
        assert r == 0

    def test_cancelled_recovery_sends_nothing(self):
        r = scenario(
            page("firmware", routes())
            + """
            t.env.confirm = () => false;
            await t.page.recover();
            return t.calls.filter((c) => c.url === '/device/radio/recover').length;
        """
        )
        assert r == 0

    def test_version_before_the_update_is_kept_and_reported_after_the_reload(self):
        r = scenario(
            page("firmware", routes())
            + """
            const store = {};
            t.env.storage = {getItem: (k) => k in store ? store[k] : null, setItem: (k, v) => { store[k] = v; },
                             removeItem: (k) => { delete store[k]; }};
            t.page.init(); await t.advance(100);
            t.el('otaFile').files = [{name: 'otbr.bundle', size: 1234}];
            await t.page.install('ota');
            const kept = store['ota:before'];
            await t.page.refreshStatus();          // still waiting for the reboot: the note is kept for later
            const keptWhileWaiting = store['ota:before'];
            store['ota:before'] = '0.9';           // as if the board came back on 1.0 after the reload
            const t2 = H.makeEnv('firmware', ROUTES_AGAIN);
            t2.env.storage = t.env.storage;
            t2.page.init(); await t2.advance(100);
            return [kept, keptWhileWaiting, t2.el('otaMsg').textContent, 'ota:before' in store];
        """.replace("ROUTES_AGAIN", routes())
        )
        assert r == [
            "1.0",
            "1.0",
            "Update done: the board now runs 1.0 (before: 0.9).",
            False,
        ]


STORE = """const store = {};
            t.env.storage = {getItem: (k) => k in store ? store[k] : null, setItem: (k, v) => { store[k] = v; },
                             removeItem: (k) => { delete store[k]; }};"""


@needs_node
class TestUpdatesCard:
    """FirmwarePage Updates card"""

    def test_page_opens_never_checked_starts_a_check_and_polls_until_it_is_done(self):
        # On a page opened with no check: GET, then check=true, then plain GETs until done; the line and buttons follow.
        r = scenario(
            page("firmware", routes())
            + f"""
            const never = {upd(state="never", checked_age_s=None, release=None, app=None, available=False, rcp_differs=False)};
            const checking = Object.assign({{}}, never, {{state: 'checking'}});
            const done = {upd()};
            let n = 0;
            t.upd = (u) => u.endsWith('check=true') ? {{status: 200, body: checking}}
                         : {{status: 200, body: ++n === 1 ? never : n < 4 ? checking : done}};
            t.page.init(); await t.advance(100);
            const urls = t.calls.map((c) => c.url).filter((x) => x.startsWith('/device/update'));
            const during = [t.el('updMsg').textContent, t.el('updInstall').disabled, t.el('updCheck').disabled,
                            t.el('updChip').textContent];
            await t.advance(30000);
            const all = t.calls.map((c) => c.url).filter((x) => x.startsWith('/device/update'));
            return [urls.slice(0, 2), during, all.slice(2).every((x) => x === '/device/update'),
                    t.el('updChip').textContent, t.el('updMsg').textContent, t.el('updCheck').disabled];
        """
        )
        assert r[0] == ["/device/update", "/device/update?check=true"]
        assert r[1] == [
            "Checking GitHub for the latest release...",
            True,
            True,
            "Checking",
        ]
        assert r[2] is True
        assert r[3:] == ["v0.4.0 available", "", False]

    def test_running_check_is_followed_while_only_the_updates_card_is_on_screen(self):
        # On the manual cards off screen and the Updates card on it: plain GETs follow the check to its end.
        r = scenario(
            page("firmware", routes())
            + f"""
            const observed = [];
            t.env.IntersectionObserver = function(cb) {{ this.observe = (el) => observed.push({{cb, el}}); }};
            const checking = Object.assign({{}}, {upd()}, {{state: 'checking'}});
            let n = 0;
            t.upd = () => ({{status: 200, body: ++n < 3 ? checking : {upd()}}});
            t.page.init();
            for (const o of observed)
              o.cb([{{isIntersecting: o.el === t.el('updCard')}}]);
            await t.advance(30000);
            return [observed.map((o) => o.el === t.el('updCard') ? 'updCard' : o.el === t.el('firmwareSection') ? 'firmwareSection' : '?'),
                    n >= 3, t.el('updChip').textContent];
        """
        )
        assert sorted(r[0]) == ["firmwareSection", "updCard"]
        assert r[1:] == [True, "v0.4.0 available"]

    def test_fresh_check_is_not_repeated_on_open(self):
        # On a page opened with a check 30 s old: only the plain GET.
        r = scenario(
            page("firmware", routes())
            + """
            t.page.init(); await t.advance(100);
            return t.calls.map((c) => c.url).filter((x) => x.startsWith('/device/update'));
        """
        )
        assert r == ["/device/update"]

    def test_card_shows_the_release_and_what_is_installed(self):
        r = scenario(
            page("firmware", routes())
            + f"""
            t.u = {upd()};
            t.page.init(); await t.advance(100);
            return ['updChip', 'updChecked', 'updHint', 'updInstall'].map((i) => t.el(i).textContent)
                .concat([t.el('updApp').allText(), t.el('updRcp').allText(), t.el('updRelease').allText(), t.el('updInstall').hidden()]);
        """
        )
        assert r[0] == "v0.4.0 available"
        assert r[1] == "just now"
        assert "half a minute" in r[2]
        assert r[3] == "Install v0.4.0 and reboot"
        assert "20261108T120102Z-9f3a21c" in r[4] and "installed: 1.0" in r[4]
        assert "20261108T115904Z-9f3a21c" in r[5] and "installed: 2.1" in r[5]
        assert "v0.4.0" in r[6] and "release notes" in r[6]
        assert r[7] is False

    def test_install_is_hidden_when_up_to_date(self):
        r = scenario(
            page("firmware", routes())
            + "t.page.init(); await t.advance(100); return [t.el('updInstall').hidden(), t.el('updCheck').hidden(), t.el('updChip').textContent];"
        )
        assert r == [True, False, "Up to date"]

    def test_check_again_asks_for_a_check_and_a_busy_reply_is_shown_red(self):
        # On Check again: GET ?check=true; a 409 busy shows the failure text in red.
        r = scenario(
            page("firmware", routes())
            + """
            t.page.init(); await t.advance(100);
            t.upd = (u) => ({status: 409, body: {error: 'busy', detail: 'another operation runs'}});
            t.el('updCheck').click(); await t.advance(100);
            const c = t.calls.filter((x) => x.url === '/device/update?check=true');
            return [c.length, c[0].method, t.el('updMsg').textContent, t.el('updMsg').style.color];
        """
        )
        assert r[:2] == [1, "GET"]
        assert "busy" in r[2].lower() or "another operation" in r[2]
        assert r[3] == "var(--danger)"

    def test_failed_check_shows_the_cause_in_red(self):
        r = scenario(
            page("firmware", routes())
            + f"""
            t.u = {upd(state="failed", error="TLS handshake failed (u)")};
            t.page.init(); await t.advance(100);
            return [t.el('updChip').textContent, t.el('updMsg').textContent, t.el('updMsg').style.color,
                    t.el('updChecked').textContent, t.el('updInstall').hidden()];
        """
        )
        assert r == [
            "Check failed",
            "The check failed: TLS handshake failed (u). The board needs internet access and DNS.",
            "var(--danger)",
            "just now (the last check failed)",
            False,
        ]

    def test_cancelled_install_sends_nothing(self):
        r = scenario(
            page("firmware", routes())
            + f"""
            t.u = {upd()};
            t.page.init(); await t.advance(100);
            t.env.confirm = () => false;
            await t.page.installUpdate();
            return [t.calls.filter((c) => c.method === 'POST').length, t.page.kit.isBusy()];
        """
        )
        assert r == [0, False]

    def test_confirmed_install_posts_inside_a_long_operation(self):
        # On a confirmed install: POST /device/update, every action disabled, the line names the tag.
        r = scenario(
            page("firmware", routes())
            + f"""
            t.u = {upd()};
            t.page.init(); await t.advance(100);
            const ids = ['otaInstall','rcpInstall','updInstall','updCheck','recoverBtn','logBtn'];
            const p = t.page.installUpdate();
            const during = [ids.map((i) => t.el(i).disabled), t.el('updMsg').textContent, t.confirms().length];
            await p;
            const c = t.calls.find((x) => x.method === 'POST');
            return [c.url, during, t.confirms()[0]];
        """
        )
        assert r[0] == "/device/update"
        assert all(r[1][0])
        assert (
            r[1][1]
            == "The device is downloading v0.4.0 from GitHub and installing it ..."
        )
        assert "v0.4.0" in r[2] and "1.0" in r[2]

    def test_install_of_an_older_release_is_forced(self):
        r = scenario(
            page("firmware", routes())
            + f"""
            t.u = {upd(app="older")};
            t.page.init(); await t.advance(100);
            await t.page.installUpdate();
            return [t.calls.find((x) => x.method === 'POST').url, t.el('updInstall').classes.has('btn-primary')];
        """
        )
        assert r == ["/device/update?force=1", False]

    def test_reboot_reply_stores_the_keys_and_waits_for_the_board(self):
        r = scenario(
            page("firmware", routes())
            + f"""
            {STORE}
            t.upd = (u, o) => o.method === 'POST' ? {{status: 200, body: {{status: 'ok', app: 'installed', rcp: 'failed', detail: 'cut off', rebooting: true}}}}
                                                  : {{status: 200, body: {upd()}}};
            t.page.init(); await t.advance(100);
            await t.page.installUpdate();
            return [store['ota:before'], JSON.parse(store['update:after']), t.page.kit.isWaiting(), t.page.kit.isBusy(),
                    t.el('updMsg').textContent];
        """
        )
        assert r == [
            "1.0",
            {"rcpFailed": "cut off"},
            True,
            True,
            "Update installed. The board is rebooting; this page reloads when it is back.",
        ]

    def test_after_the_reload_the_note_goes_to_the_card_and_the_keys_are_cleared(self):
        r = scenario(
            page("firmware", routes())
            + f"""
            {STORE}
            store['ota:before'] = '0.9'; store['update:after'] = JSON.stringify({{rcpFailed: 'cut off'}});
            t.page.init(); await t.advance(100);
            return [t.el('updMsg').textContent, t.el('updMsg').style.color, t.el('otaMsg').textContent, Object.keys(store)];
        """
        )
        assert r[0].startswith("Update done: the board now runs 1.0 (before: 0.9).")
        assert (
            "The Border Router was updated, but the radio firmware was not: cut off. "
            "Install again to update the radio firmware only." in r[0]
        )
        assert r[1] == "var(--danger)"
        assert r[2:] == ["", []]

    def test_after_a_rolled_back_reload_the_radio_failure_does_not_say_the_border_router_was_updated(
        self,
    ):
        # On the board coming back on the version it ran before: no "Border Router was updated", no "install again".
        r = scenario(
            page("firmware", routes())
            + f"""
            {STORE}
            store['ota:before'] = '1.0'; store['update:after'] = JSON.stringify({{rcpFailed: 'cut off'}});
            t.page.init(); await t.advance(100);
            return [t.el('updMsg').textContent, t.el('updMsg').style.color];
        """
        )
        assert r[0].startswith("The board is back and still runs 1.0")
        assert "The radio firmware was not updated either: cut off." in r[0]
        assert "Border Router was updated" not in r[0]
        assert r[1] == "var(--danger)"

    def test_after_the_reload_without_a_radio_failure_the_note_is_plain(self):
        r = scenario(
            page("firmware", routes())
            + f"""
            {STORE}
            store['ota:before'] = '0.9'; store['update:after'] = JSON.stringify({{rcpFailed: null}});
            t.page.init(); await t.advance(100);
            return [t.el('updMsg').textContent, t.el('updMsg').style.color, Object.keys(store)];
        """
        )
        assert r == ["Update done: the board now runs 1.0 (before: 0.9).", "", []]

    def test_reply_without_a_reboot_shows_the_radio_text_and_refreshes(self):
        # On rebooting false: the radio text with the stamp of the check, a status refresh and a new check.
        r = scenario(
            page("firmware", routes())
            + f"""
            t.upd = (u, o) => o.method === 'POST' ? {{status: 200, body: {{status: 'ok', app: 'unchanged', rcp: 'installed', detail: '', rebooting: false}}}}
                                                  : {{status: 200, body: {upd(app="same")}}};
            t.page.init(); await t.advance(100);
            const n = t.calls.length;
            await t.page.installUpdate(); await t.advance(100);
            const after = t.calls.slice(n).map((c) => c.url);
            return [t.el('updMsg').textContent, after.includes('/device/status'), after.includes('/device/update?check=true'),
                    t.page.kit.isBusy()];
        """
        )
        assert r == [
            "Radio firmware installed: 20261108T115904Z-9f3a21c.",
            True,
            True,
            False,
        ]

    def test_install_error_is_shown_red_and_the_buttons_come_back(self):
        r = scenario(
            page("firmware", routes())
            + f"""
            t.upd = (u, o) => o.method === 'POST' ? {{status: 409, body: {{error: 'up_to_date', detail: 'nothing to install'}}}}
                                                  : {{status: 200, body: {upd()}}};
            t.page.init(); await t.advance(100);
            await t.page.installUpdate();
            return [t.el('updMsg').textContent, t.el('updMsg').style.color, t.el('updInstall').disabled, t.el('updCheck').disabled];
        """
        )
        assert "nothing to install" in r[0] or "up_to_date" in r[0]
        assert r[1:] == ["var(--danger)", False, False]

    def test_links_are_made_only_for_https_urls(self):
        r = scenario(
            page("firmware", routes())
            + f"""
            t.u = {upd(release=rel(url="http://h/releases/tag/v0.4.0"), repo_url="javascript:alert(1)")};
            t.page.init(); await t.advance(100);
            const none = [t.el('updRepo').getAttribute('href'), t.el('updRelease').findAll((e) => e.tag === 'a').length];
            t.u = {upd()};
            t.el('updCheck').click(); await t.advance(100);
            const some = [t.el('updRepo').getAttribute('href'), t.el('updRepo').textContent,
                          t.el('updRelease').findAll((e) => e.tag === 'a').map((e) => e.getAttribute('href'))];
            return [none, some];
        """
        )
        assert r[0] == [None, 0]
        assert r[1] == [
            "https://github.com/t3hk0d3/esp32-p4-otbr",
            "t3hk0d3/esp32-p4-otbr",
            ["https://github.com/t3hk0d3/esp32-p4-otbr/releases/tag/v0.4.0"],
        ]

    def test_server_text_on_the_card_is_rendered_as_text(self):
        r = scenario(
            page("firmware", routes())
            + f"""
            t.u = {upd(release=rel(tag="<b>x</b>", app="<i>y</i>"))};
            t.page.init(); await t.advance(100);
            return [t.el('updChip').textContent, t.el('updApp').allText(), t.el('updRelease').allText()];
        """
        )
        assert r[0] == "<b>x</b> available"
        assert "<i>y</i>" in r[1]
        assert "<b>x</b>" in r[2]

    def test_manual_cards_are_outline_buttons_and_the_card_install_is_primary(self):
        html = (FRONTEND / "firmware.html").read_text()
        for i in ("otaInstall", "rcpInstall"):
            assert f'class="btn btn-outline" id="{i}"' in html
        r = scenario(
            page("firmware", routes())
            + f"""
            t.u = {upd()};
            t.page.init(); await t.advance(100);
            return t.el('updInstall').classes.has('btn-primary');
        """
        )
        assert r is True

    def test_card_sits_above_the_cards_it_does_not_replace(self):
        html = (FRONTEND / "firmware.html").read_text()
        assert html.index('id="updCard"') < html.index('id="firmwareSection"')
