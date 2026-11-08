"""Shared code of the device pages (BDD: DeviceCommon, DeviceKit). Runs static/device_common.js under node."""

import json
import re

import pytest
from device_js import KIT_HTML, REPO, kit, logic, needs_node, scenario

C6_BOARD = REPO / "common" / "c6_board" / "c6_board.h"
PARTITIONS = REPO / "p4" / "partitions.csv"


def common(expression):
    return logic("device_common", expression)


@needs_node
class TestCommonLogic:
    """DeviceCommon pure functions"""

    def test_error_message_shows_the_detail_from_the_body(self):
        # On a JSON error body: the detail text is part of the message.
        msg = common(
            "L.errorMessage(400, {error: 'invalid_bundle', detail: 'bad sha'})"
        )
        assert "bad sha" in msg

    @pytest.mark.parametrize(
        ("status", "word"),
        [
            (409, "busy"),
            (412, "jumper"),
            (413, "large"),
            (502, "transfer"),
            (503, "available"),
        ],
    )
    def test_error_message_explains_the_status(self, status, word):
        # On 409/412/413/502/503: a human-readable explanation beside the detail.
        msg = common(f"L.errorMessage({status}, {{error: 'x', detail: 'd'}})")
        assert word in msg.lower()
        assert "d" in msg

    @pytest.mark.parametrize(
        ("status", "chip"),
        [
            (
                "{mode: 'BR_RUNNING', otbr_channel: 'c6'}",
                {"text": "Thread runs on: C6", "tone": "chip-ok"},
            ),
            (
                "{mode: 'SNIFFER', otbr_channel: 'c6'}",
                {"text": "Thread runs on: C6", "tone": "chip-ok"},
            ),
            (
                "{mode: 'NO_RADIO', otbr_channel: 'c6', fault_reason: 'radio unplugged (c6)'}",
                {"text": "Thread on C6: radio unplugged (c6)", "tone": "chip-warn"},
            ),
            (
                "{mode: 'NO_RADIO', otbr_channel: 'c6', fault_reason: null}",
                {"text": "Thread on C6: not available", "tone": "chip-warn"},
            ),
            (
                "{mode: 'RADIO_FAULT', otbr_channel: 'c6', fault_reason: 'rcp incompatible'}",
                {"text": "Thread on C6: fault: rcp incompatible", "tone": "chip-warn"},
            ),
            (
                "{mode: 'STARTING', otbr_channel: 'c6'}",
                {"text": "Thread on C6: starting", "tone": "chip-warn"},
            ),
            (
                "{mode: 'NO_RADIO', otbr_channel: null}",
                {"text": "No radio in Thread mode", "tone": "chip-warn"},
            ),
        ],
    )
    def test_thread_chip_says_runs_on_only_while_thread_runs(self, status, chip):
        # On BR_RUNNING or SNIFFER: "Thread runs on" (ok); otherwise a warning with the state or reason; with no
        # channel in otbr: "No radio in Thread mode".
        assert common(f"L.threadChip({status}, 'C6')") == chip

    def test_thread_chip_without_a_label_names_the_id_and_hides_for_an_older_board(
        self,
    ):
        # On no label: the channel id; on a status without otbr_channel (or none): hidden (null).
        assert common(
            "[L.threadChip({mode: 'BR_RUNNING', otbr_channel: 'usb-1'}, ''), L.threadChip({mode: 'BR_RUNNING'}, 'x'),"
            " L.threadChip(null, 'x')]"
        ) == [{"text": "Thread runs on: usb-1", "tone": "chip-ok"}, None, None]

    def test_error_message_without_json_body_still_names_the_status(self):
        # On a non-JSON body: the HTTP status is named.
        assert "500" in common("L.errorMessage(500, null)")

    def test_remaining_time_is_minutes_and_seconds(self):
        # On 299 s: "4:59"; on 0: "0:00".
        assert common("L.formatRemaining(299)") == "4:59"
        assert common("L.formatRemaining(0)") == "0:00"

    def test_trial_left_is_a_share_of_the_five_minute_trial(self):
        # On 300 s: 100; 150 s: 50; 0 s: 0; outside 0..300 it is clamped; whole percent.
        assert common("[300, 150, 0, 400, -5, 100].map(L.trialLeftPct)") == [
            100,
            50,
            0,
            100,
            0,
            33,
        ]

    @pytest.mark.parametrize(
        ("seconds", "text"),
        [(40, "40 s"), (720, "12 min"), (27303, "7 h 35 min"), (183600, "2 d 3 h")],
    )
    def test_duration_text_uses_the_two_largest_units(self, seconds, text):
        # On an uptime in seconds: words with the two largest units.
        assert common(f"L.durationText({seconds})") == text

    def test_whole_number_accepts_only_digits_in_range(self):
        # On "7" in 1..9: 7; on "0", "10", "1.5", "": null.
        assert common("L.wholeNumber('7', 1, 9)") == 7
        assert (
            common("['0','10','1.5',''].map(v => L.wholeNumber(v, 1, 9))") == [None] * 4
        )

    def test_polling_runs_only_while_visible_and_not_in_flight(self):
        # On hidden tab / section out of view / request in flight: no poll; otherwise poll.
        assert (
            common(
                "L.shouldPoll({hidden: false, sectionVisible: true, inFlight: false})"
            )
            is True
        )
        assert (
            common(
                "L.shouldPoll({hidden: true, sectionVisible: true, inFlight: false})"
            )
            is False
        )
        assert (
            common(
                "L.shouldPoll({hidden: false, sectionVisible: false, inFlight: false})"
            )
            is False
        )
        assert (
            common(
                "L.shouldPoll({hidden: false, sectionVisible: true, inFlight: true})"
            )
            is False
        )

    def test_core_label_names_a_number_or_any(self):
        # On task core 1 / "any": "1" / "any".
        assert common("L.coreLabel(1)") == "1"
        assert common("L.coreLabel('any')") == "any"

    def test_load_text_has_one_decimal_and_percent(self):
        # On a load object: "1s / 10s / 60s" values joined.
        assert (
            common("L.loadText({'1s': 12.5, '10s': 3, '60s': 0.04})")
            == "12.5% / 3.0% / 0.0%"
        )

    def test_bytes_text_is_kilobytes_or_megabytes_with_one_decimal(self):
        # On 1536 bytes: "1.5 KB"; on 3 MB: "3.0 MB".
        assert common("L.bytesText(1536)") == "1.5 KB"
        assert common("L.bytesText(3145728)") == "3.0 MB"


@needs_node
class TestRadioPreference:
    """radioPref, trelLow, preferenceParts: OpenThread's per-radio preference of a Thread neighbour"""

    def test_preference_is_shown_as_a_whole_percent_of_255(self):
        assert common("[0, 55, 200, 220, 230, 255].map(L.preferencePercent)") == [
            "0%",
            "22%",
            "78%",
            "86%",
            "90%",
            "100%",
        ]

    def test_trel_below_the_radio_is_low(self):
        assert common("L.trelLow({ieee802154: 230, trel: 55})") is True

    def test_trel_at_or_above_the_radio_is_not_low(self):
        assert common("L.trelLow({ieee802154: 200, trel: 255})") is False
        assert common("L.trelLow({ieee802154: 200, trel: 200})") is False

    def test_trel_alone_or_no_preference_is_not_low(self):
        assert common("L.trelLow({ieee802154: null, trel: 55})") is False
        assert common("L.trelLow(null)") is False

    def test_parts_read_15_4_then_trel_with_the_low_figure_marked(self):
        r = common("L.preferenceParts({ieee802154: 230, trel: 55})")
        assert "".join(p["text"] for p in r) == "15.4 90% \u00b7 TREL 22%"
        assert [p["text"] for p in r if p["low"]] == ["22%"]

    def test_parts_of_one_radio_or_none(self):
        assert (
            common(
                "L.preferenceParts({ieee802154: null, trel: 255}).map((p) => p.text).join('')"
            )
            == "TREL 100%"
        )
        assert (
            common(
                "L.preferenceParts({ieee802154: 255, trel: null}).map((p) => p.text).join('')"
            )
            == "15.4 100%"
        )
        assert common("L.preferenceParts(null)") == []


@needs_node
class TestLimits:
    """Client-side size limits against the device's own"""

    def test_firmware_limit_equals_the_c6_slot_size(self):
        # On C6_OTA_SLOT_SIZE in c6_board.h: same number as RCP_MAX_BYTES.
        slot = int(
            re.search(
                r"define C6_OTA_SLOT_SIZE\s+(0x[0-9a-fA-F]+)", C6_BOARD.read_text()
            ).group(1),
            16,
        )
        assert common("L.RCP_MAX_BYTES") == slot

    def test_bundle_limit_is_header_plus_web_plus_app_partition(self):
        # On partitions.csv: 44 + web_0 size + ota_0 size.
        def size(name):
            row = re.search(
                rf"^{name},.*,\s*(\d+)([KMG]),", PARTITIONS.read_text(), re.MULTILINE
            )
            return (
                int(row.group(1))
                * {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30}[row.group(2)]
            )

        assert common("L.BUNDLE_MAX_BYTES") == 44 + size("web_0") + size("ota_0")

    def test_file_over_the_limit_is_refused_before_sending(self):
        # On a file larger than the limit: a message naming the limit; at the limit: none; empty: a message.
        assert "Too large" in common("L.checkUploadSize('rcp', L.RCP_MAX_BYTES + 1)")
        assert common("L.checkUploadSize('rcp', L.RCP_MAX_BYTES)") is None
        assert common("L.checkUploadSize('bundle', L.BUNDLE_MAX_BYTES)") is None
        assert "empty" in common("L.checkUploadSize('bundle', 0)")


@needs_node
class TestBackoffAndWait:
    """nextDelay, lastSeenText, bootObserved"""

    def test_delay_doubles_per_failure_up_to_30_seconds(self):
        # On period 3000 and failures 0..5: 3000, 6000, 12000, 24000, 30000, 30000.
        assert common("[0,1,2,3,4,5].map(f => L.nextDelay(3000, f))") == [
            3000,
            6000,
            12000,
            24000,
            30000,
            30000,
        ]

    def test_last_seen_text_names_the_seconds(self):
        # On a last success 12 s ago: "(last seen 12 s ago)"; never seen: no number.
        assert "last seen 12 s ago" in common("L.lastSeenText(1000, 13000)")
        assert "last seen" not in common("L.lastSeenText(0, 13000)")

    def test_boot_is_observed_after_an_outage_and_a_success(self):
        # On fail then ok: true on the ok.
        assert common(
            "(()=>{const s={outage:false,baseline:100};return [L.bootObserved(s,{ok:false}),L.bootObserved(s,{ok:true,uptime:500})]})()"
        ) == [False, True]

    def test_boot_is_observed_when_uptime_goes_backwards_without_outage(self):
        # On a baseline of 100 s and a probe with 5 s: true.
        assert (
            common("L.bootObserved({outage:false,baseline:100},{ok:true,uptime:5})")
            is True
        )

    def test_boot_is_not_observed_while_the_old_board_keeps_answering(self):
        # On a baseline of 100 s and a probe with 103 s: false.
        assert (
            common("L.bootObserved({outage:false,baseline:100},{ok:true,uptime:103})")
            is False
        )

    def test_first_probe_without_baseline_only_sets_the_baseline(self):
        # On no baseline: the first success is not accepted.
        assert common(
            "(()=>{const s={outage:false,baseline:null};return [L.bootObserved(s,{ok:true,uptime:50}),s.baseline]})()"
        ) == [False, 50]


NET = "(rem) => ({status: 200, body: Object.assign(H.fixture(H.NETWORK), {trial: rem === null ? null : {mode: 'single', single: {vlan: 0, ipv4: {dhcp: true}}, remaining_s: rem}})})"


@needs_node
class TestKit:
    """DeviceKit run against a fake DOM and fetch"""

    def test_busy_disables_every_action_and_the_confirm_button(self):
        # On setBusy(true): the page's action ids and confirmBtn disabled; on false: enabled again.
        r = scenario(
            kit()
            + """
            t.kit.setBusy(true); const on = ['actA','actB','confirmBtn'].map((i) => t.el(i).disabled);
            t.kit.setBusy(false); const off = ['actA','actB','confirmBtn'].map((i) => t.el(i).disabled);
            return [on, off];
        """
        )
        assert r == [[True, True, True], [False, False, False]]

    def test_run_op_refuses_a_second_operation_and_a_failure_releases_the_lock(self):
        # On runOp while busy: null, the second function never runs; after a rejection: not busy.
        r = scenario(
            kit()
            + """
            let ran = 0;
            const first = t.kit.runOp(() => new Promise((res, rej) => setImmediate(() => rej(new Error('x')))));
            const second = await t.kit.runOp(() => { ran++; return Promise.resolve(1); });
            await first.catch(() => {});
            return [second, ran, t.kit.isBusy()];
        """
        )
        assert r == [None, 0, False]

    def test_handoff_keeps_the_lock(self):
        # On an operation resolving to {handoff: true}: still busy (the wait for the reboot took over).
        r = scenario(
            kit()
            + "await t.kit.runOp(() => Promise.resolve({handoff: true})); return t.kit.isBusy();"
        )
        assert r is True

    def test_dropped_upload_connection_says_the_board_closed_it(self):
        # On a connection error during an upload request: the message names a rejected upload, not a reboot.
        r = scenario(
            kit("() => { throw new Error('reset'); }")
            + "try { await t.kit.request('POST', '/device/ota', {upload: true}); } catch (e) { return e.message; }"
        )
        assert "closed the connection" in r
        assert "rebooting" not in r

    def test_poller_does_not_overlap_requests(self):
        # On a slow answer: never more than one request in flight.
        r = scenario(
            kit("(u) => ({status: 200, body: H.STATUS})")
            + """
            t.kit.poller(() => t.kit.request('GET', '/device/status').then(() => true), 100, 'sec');
            await t.advance(2000);
            return [t.maxInFlight(), t.calls.length > 3];
        """
        )
        assert r == [1, True]

    def test_poller_pauses_while_the_tab_is_hidden_and_resumes(self):
        # On a hidden tab: no new request; on visibilitychange to visible: requests continue.
        r = scenario(
            kit("(u) => ({status: 200, body: H.STATUS})")
            + """
            t.kit.poller(() => t.kit.request('GET', '/device/status').then(() => true), 100, 'sec');
            await t.advance(500);
            t.doc.hidden = true; await t.advance(500);
            const n = t.calls.length; await t.advance(5000);
            const hiddenCalls = t.calls.length - n;
            t.doc.hidden = false; t.doc.fire('visibilitychange'); await t.advance(500);
            return [hiddenCalls, t.calls.length > n];
        """
        )
        assert r == [0, True]

    def test_poller_backs_off_on_failure_and_resets_on_success(self):
        # On failing answers: gaps grow (3 s, 6 s, ...); the first success resets the failure count.
        r = scenario(
            kit()
            + """
            t.fail = true;
            const p = t.kit.poller(() => Promise.resolve(!t.fail), 3000, 'sec');
            await t.advance(60000);
            const failures = p.failures();
            t.fail = false; await t.advance(31000);
            return [failures, p.failures()];
        """
        )
        assert r[0] >= 3
        assert r[1] == 0

    def test_polling_stops_while_a_long_operation_runs(self):
        # On busy: no requests; after it ends they resume.
        r = scenario(
            kit("(u) => ({status: 200, body: H.STATUS})")
            + """
            t.kit.poller(() => t.kit.request('GET', '/device/status').then(() => true), 100, 'sec');
            await t.advance(300);
            t.kit.setBusy(true); await t.advance(300);
            const n = t.calls.length; await t.advance(3000);
            const during = t.calls.length - n;
            t.kit.setBusy(false); await t.advance(500);
            return [during, t.calls.length > n];
        """
        )
        assert r == [0, True]

    def test_wait_for_board_reloads_after_an_outage_and_a_success(self):
        # On diag failing twice then answering: reload once, and only one waiter can run.
        r = scenario(
            kit(
                "(u) => { t.n = (t.n || 0) + 1; if (t.n === 2 || t.n === 3) throw new Error('down'); return {status: 200, body: {uptime_s: t.n}}; }"
            )
            + """
            t.kit.setBusy(true);
            t.kit.waitForBoard('msg'); t.kit.waitForBoard('msg');
            await t.advance(20000);
            return [t.reloads(), t.calls.filter((c) => c.url === '/device/diag').length >= 4];
        """
        )
        assert r == [1, True]

    def test_wait_for_board_gives_up_with_a_hint_and_enables_the_buttons(self):
        # On a board that never goes away: after 3 minutes a message about the address/VLAN and the 5 minute revert.
        r = scenario(
            kit("(u) => ({status: 200, body: {uptime_s: 100}})")
            + """
            t.kit.setBusy(true);
            t.kit.waitForBoard('msg');
            await t.advance(200000);
            return [t.reloads(), t.el('msg').textContent, t.kit.isBusy(), t.kit.isWaiting()];
        """
        )
        assert r[0] == 0
        assert "VLAN" in r[1]
        assert "5 minutes" in r[1]
        assert r[2:] == [False, False]

    def test_trial_countdown_resyncs_from_the_server_each_answer(self):
        # On remaining_s 100, 40 s pass, then the server says 90: the banner shows 1:30 again.
        r = scenario(
            kit()
            + f"""
            const net = {NET};
            t.kit.renderTrial(net(100).body); const first = t.el('trialRemaining').textContent;
            await t.advance(40000); t.kit.tickTrial(); const later = t.el('trialRemaining').textContent;
            t.kit.renderTrial(net(90).body);
            return [first, later, t.el('trialRemaining').textContent, !t.el('trialBanner').hidden()];
        """
        )
        assert r == ["1:40", "1:00", "1:30", True]

    def test_trial_bar_shows_the_share_of_the_trial_left(self):
        # On remaining_s 150 of the 300 s trial, then 60 s later: the bar is half full, then a third full.
        r = scenario(
            kit()
            + f"""
            const net = {NET};
            t.kit.renderTrial(net(150).body); const first = t.el('trialFill').style.width;
            await t.advance(60000); t.kit.tickTrial();
            return [first, t.el('trialFill').style.width];
        """
        )
        assert r == ["50%", "30%"]

    def test_no_trial_hides_the_banner(self):
        # On trial null after a trial: the banner is hidden.
        r = scenario(
            kit()
            + f"""
            const net = {NET};
            t.kit.renderTrial(net(5).body); t.kit.renderTrial(net(null).body);
            return t.el('trialBanner').hidden();
        """
        )
        assert r is True

    def test_started_kit_polls_the_network_for_the_banner(self):
        # On start(): GET /device/network and the banner follows its trial.
        r = scenario(
            kit(f"(u) => ({NET})(42)")
            + """
            t.kit.start(); await t.advance(100);
            return [t.calls[0].url, !t.el('trialBanner').hidden(), t.el('trialRemaining').textContent];
        """
        )
        assert r == ["/device/network", True, "0:42"]

    def test_page_that_renders_the_banner_itself_gets_no_banner_polling(self):
        # On trial: false: start() sends no request.
        r = scenario(
            kit(
                options="{actionIds: [], trial: false, refreshNetwork: () => Promise.resolve(true)}"
            )
            + "t.kit.start(); await t.advance(30000); return t.calls.length;"
        )
        assert r == 0

    def test_keep_new_settings_posts_confirm_and_shows_the_result(self):
        # On Keep new settings answered 403: POST /device/network/confirm, confirmMsg shows the detail.
        r = scenario(
            kit(
                f"(u, o) => o.method === 'POST' ? ({{status: 403, body: {{error: 'wrong_interface', detail: 'use mgmt'}}}}) : ({NET})(null)"
            )
            + """
            t.kit.start(); await t.advance(100);
            t.el('confirmBtn').click(); await t.advance(100);
            const post = t.calls.find((c) => c.method === 'POST');
            return [post.url, t.el('confirmMsg').textContent];
        """
        )
        assert r[0] == "/device/network/confirm"
        assert "use mgmt" in r[1]


@needs_node
class TestKitReview:
    """findings of the whole-branch review"""

    def test_banner_hides_when_the_countdown_runs_out(self):
        # On remaining_s 5 and 6 s passing: the banner is hidden (the board is rebooting back).
        r = scenario(
            kit("() => { throw new Error('rebooting'); }")
            + f"""
            t.kit.start(); t.kit.renderTrial(({NET})(5).body);
            await t.advance(6000);
            return t.el('trialBanner').hidden();
        """
        )
        assert r is True


@needs_node
class TestKitPrReview:
    """PR review findings"""

    def test_page_without_the_trial_bar_still_counts_down_and_hides_the_banner(self):
        # On a cached page from before the bar (no #trialFill), remaining_s 5 and 6 s: the countdown ends and the
        # banner is hidden, no error.
        old_page = KIT_HTML.replace('<div id="trialFill"></div>', "")
        r = scenario(
            f"const t = H.makeEnv({{html: {json.dumps(old_page)}, absent: ['trialFill'], kit: {{actionIds: []}}}}, "
            + "() => { throw new Error('rebooting'); });"
            + f"""
            t.kit.start(); t.kit.renderTrial(({NET})(5).body);
            await t.advance(6000);
            return [t.el('trialRemaining').textContent, t.el('trialBanner').hidden()];
        """
        )
        assert r == ["0:00", True]

    def test_a_wake_while_a_request_runs_does_not_start_a_second_one(self):
        # On visibilitychange while the first request is pending: no second request until it answers.
        r = scenario(
            kit()
            + """
            let release; let n = 0;
            t.env.fetch = () => { n++; return new Promise((res) => { release = () => res({status: 200, text: () => Promise.resolve('{}')}); }); };
            t.kit.poller(() => t.kit.request('GET', '/device/status').then(() => true), 100, 'sec');
            await t.advance(10);
            t.doc.fire('visibilitychange'); await t.advance(500);
            const during = n; release(); await t.advance(150);
            return [during, n];
        """
        )
        assert r == [1, 2]

    def test_wait_for_board_can_give_up_with_its_own_text(self):
        # On waitForBoard(msg, text) with a board that never reboots: that text after 3 minutes.
        r = scenario(
            kit("(u) => ({status: 200, body: {uptime_s: 100}})")
            + """
            t.kit.setBusy(true); t.kit.waitForBoard('msg', 'Open http://10.0.0.2 now.');
            await t.advance(200000);
            return [t.el('msg').textContent, t.kit.isBusy()];
        """
        )
        assert r == ["Open http://10.0.0.2 now.", False]
