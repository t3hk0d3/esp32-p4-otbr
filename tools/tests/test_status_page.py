"""Dashboard, index.html: the status page (BDD: StatusLogic, StatusPage). Runs static/status.js under node."""

import json

import pytest
from device_js import logic, needs_node, page, scenario


def st(expression):
    return logic("status", expression)


def status(**patch):
    return f"Object.assign(H.fixture(H.STATUS), {json.dumps(patch)})"


ROUTES = (
    "(u, o) => o.method === 'POST' ? ({status: 200, body: {status: 'rebooting'}}) : ({status: 200, body:"
    " u === '/device/diag' ? H.DIAG : u === '/device/network' ? H.NETWORK : u === '/get_properties' ? (t.p || H.PROPS)"
    " : u === '/ipaddr' ? (t.a || H.IPADDR) : (t.s || H.STATUS)})"
)


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

    def test_fault_hero_of_a_dongle_links_the_radios_page(self):
        # On RADIO_FAULT while Thread's radio is a dongle: the link goes to Radios, not Firmware (no update there).
        h = st(
            "L.heroFor({mode: 'RADIO_FAULT', fault_reason: 'spinel timeout', otbr_channel: 'usb-10c4-ea60-1', sniffer: {}})"
        )
        assert h["link"] == {"href": "/radios.html", "label": "Open Radios"}

    def test_fault_hero_of_a_dongle_does_not_ask_for_c6_firmware(self):
        # On "rcp incompatible" while Thread's radio is a dongle: the text names the USB radio, never c6_rcp.bin.
        h = st(
            "L.heroFor({mode: 'RADIO_FAULT', fault_reason: 'rcp incompatible', otbr_channel: 'usb-10c4-ea60-1', sniffer: {}})"
        )
        assert "c6_rcp.bin" not in h["text"]
        assert "USB radio" in h["text"] and "rcp incompatible" in h["text"]

    def test_fault_hero_of_the_c6_still_links_firmware(self):
        # On RADIO_FAULT with the C6 as Thread's radio: the firmware page, as before.
        h = st(
            "L.heroFor({mode: 'RADIO_FAULT', fault_reason: 'no rcp firmware', otbr_channel: 'c6', sniffer: {}})"
        )
        assert h["link"]["href"] == "/firmware.html"

    def test_no_radio_hero_says_why_and_links_the_radios_page(self):
        # On NO_RADIO: "No Thread radio", the reason, a link to Radios; never the raw mode name.
        h = st(
            "L.heroFor({mode: 'NO_RADIO', fault_reason: 'radio unplugged (usb-10c4-ea60-1)', otbr_channel: 'usb-10c4-ea60-1', sniffer: {}})"
        )
        assert h["title"] == "No Thread radio"
        assert h["tone"] == "idle"
        assert "radio unplugged" in h["text"]
        assert "NO_RADIO" not in h["title"] + h["text"]
        assert h["link"] == {"href": "/radios.html", "label": "Open Radios"}

    def test_no_radio_hero_without_a_reason_still_explains(self):
        # On NO_RADIO without a reason: a text that says Thread has no radio.
        h = st(
            "L.heroFor({mode: 'NO_RADIO', fault_reason: null, otbr_channel: null, sniffer: {}})"
        )
        assert "Thread" in h["text"] and "radio" in h["text"]

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
            page("index", ROUTES)
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
            page("index", ROUTES)
            + f"t.s = {status(mode='RADIO_FAULT', fault_reason='no rcp firmware')};"
            + "await t.page.refreshStatus(); return [t.el('hero').className, t.el('h-link').hidden(), t.el('h-link').getAttribute('href')];"
        )
        assert r == ["card hero hero-fault", False, "/firmware.html"]

    def test_base_image_mismatch_names_the_expected_one(self):
        # On a base image other than the expected one: both are named; without the jumper: a dash.
        r = scenario(
            page("index", ROUTES)
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
            page("index", ROUTES)
            + f"t.s = {status(mode='RADIO_FAULT', fault_reason='<img src=x>')};"
            + "await t.page.refreshStatus(); return t.el('h-text').textContent;"
        )
        assert "<img src=x>" in r

    def test_diag_fills_uptime_reset_and_load(self):
        # On a diag answer: uptime in words, reset reason, 10 s CPU per core, internal and PSRAM free.
        r = scenario(
            page("index", ROUTES)
            + "await t.page.refreshDiag(); return ['h-uptime','h-reset','t-cpu','t-mem-int','t-mem-psram','t-chip-temp'].map((i) => t.el(i).textContent);"
        )
        assert r == [
            "7 h 35 min",
            "software",
            "Core 0 0.5%, core 1 0.6%",
            "408.9 KB (lowest 350.7 KB)",
            "30.2 MB",
            "47.3 \u00b0C",
        ]

    @pytest.mark.parametrize("patch", ["{chip_temperature_c: null}", "{}", "{chip_temperature_c: 'hot'}"])
    def test_diag_without_a_chip_temperature_shows_a_dash(self, patch):
        # On null, a missing field (older firmware) or a non-number: the chip temperature is a dash.
        r = scenario(
            page(
                "index",
                "(u) => { const d = H.fixture(H.DIAG); delete d.chip_temperature_c; return {status: 200, body: Object.assign(d, "
                + patch
                + ")}; }",
            )
            + "await t.page.refreshDiag(); return t.el('t-chip-temp').textContent;"
        )
        assert r == "-"

    def test_diag_failure_leaves_dashes(self):
        # On a failing diag: the uptime stays a dash.
        r = scenario(
            page("index", "() => { throw new Error('down'); }")
            + "t.el('h-uptime').textContent = '-'; await t.page.refreshDiag(); return t.el('h-uptime').textContent;"
        )
        assert r == "-"

    def test_unanswered_status_shows_the_offline_line_with_backoff(self):
        # On failing status polls: the offline line with last-seen seconds; requests back off.
        r = scenario(
            page(
                "index",
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
            page("index", ROUTES)
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
                "index",
                "(u, o) => o.method === 'POST' ? ({status: 409, body: {error: 'busy', detail: 'update running'}}) : ({status: 200, body: H.STATUS})",
            )
            + "await t.page.reboot(); return [t.el('rebootMsg').textContent, t.page.kit.isBusy()];"
        )
        assert "update running" in r[0]
        assert r[1] is False

    def test_init_polls_status_diag_thread_and_the_trial_banner(self):
        # On init with a running border router: status, diag, network, names, and the Thread properties and addresses.
        r = scenario(
            page("index", ROUTES)
            + "t.page.init(); await t.advance(100); return [...new Set(t.calls.map((c) => c.url))].sort();"
        )
        assert r == [
            "/device/diag",
            "/device/names",
            "/device/network",
            "/device/status",
            "/get_properties",
            "/ipaddr",
        ]


@needs_node
class TestStatusPageReview:
    """findings of the whole-branch review"""

    def test_diag_failure_after_a_success_shows_dashes(self):
        # On a good diag then a failing one: uptime, reset and load go back to dashes.
        r = scenario(
            page(
                "index",
                "(u) => { if (t.fail) throw new Error('down'); return {status: 200, body: H.DIAG}; }",
            )
            + """
            await t.page.refreshDiag(); t.fail = true; await t.page.refreshDiag();
            return ['h-uptime','h-reset','t-cpu','t-mem-int','t-mem-psram','t-chip-temp'].map((i) => t.el(i).textContent);
        """
        )
        assert r == ["-"] * 6


@needs_node
class TestThreadLogic:
    """StatusLogic: the Thread part of the dashboard"""

    def test_thread_view_maps_the_properties(self):
        # On /get_properties: the fields of the two cards, "-" for a missing one.
        v = st(
            "L.threadView({'Network:Name': 'MyHome', 'Thread:Role': 'leader', 'RCP:Channel': '15', 'RCP:TxPower': '20 dBm',"
            " 'Network:PANID': '0x1234', 'Network:XPANID': 'dead', 'Network:PartitionID': '7', 'OpenThread:Version': 'ot',"
            " 'RCP:EUI64': 'e', 'Network:BorderAgentID': 'b', 'IPv6:RoutingLocalAddress': 'fd00::ff:fe00:7800'})"
        )
        assert v == {
            "name": "MyHome",
            "role": "leader",
            "channel": "15",
            "txpower": "20 dBm",
            "panid": "0x1234",
            "xpanid": "dead",
            "partition": "7",
            "otver": "ot",
            "eui64": "e",
            "baid": "b",
            "pskc": "-",
            "rloc": "fd00::ff:fe00:7800",
            "rloc16": "0x7800",
            "joined": True,
        }

    @pytest.mark.parametrize(
        ("address", "rloc16"),
        [
            ("fd00::ff:fe00:7800", "0x7800"),
            ("fd00::ff:fe00:0", "0x0000"),
            ("fd00::ff:fe00:c01", "0x0c01"),
            ("::", "-"),
            (None, "-"),
        ],
    )
    def test_rloc16_is_the_last_16_bits_of_the_rloc(self, address, rloc16):
        # On an RLOC address: its last group as 0x and four hex digits; on "::" or none: "-".
        props = (
            "{}" if address is None else f"{{'IPv6:RoutingLocalAddress': '{address}'}}"
        )
        assert st(f"L.threadView({props}).rloc16") == rloc16

    @pytest.mark.parametrize("role", ["disabled", "detached", ""])
    def test_no_network_when_disabled_detached_or_unknown(self, role):
        # On role disabled / detached / missing: not joined.
        assert st(f"L.threadView({{'Thread:Role': '{role}'}}).joined") is False

    def test_role_line_names_role_network_and_channel(self):
        # On leader of MyHome on 15: "Leader of MyHome on channel 15"; not joined: "".
        assert (
            st(
                "L.roleLine({role: 'leader', name: 'MyHome', channel: '15', joined: true})"
            )
            == "Leader of MyHome on channel 15"
        )
        assert st("L.roleLine({role: 'disabled', joined: false})") == ""

    def test_pskc_mask_hides_the_value_and_its_length(self):
        # On a short or a 32-character PSKc: the same 8 dots; "-" stays "-".
        masks = st(
            "[L.maskSecret('abcd'), L.maskSecret('a'.repeat(32)), L.maskSecret('-')]"
        )
        assert masks == ["\u2022" * 8, "\u2022" * 8, "-"]

    def test_address_rows_have_prefix_origin_and_state(self):
        # On two addresses: "address/prefix", origin, preferred or deprecated.
        rows = st(
            "L.addressRows([{address: 'fd00::1', prefixLength: 64, origin: 'thread', preferred: true},"
            " {address: 'fd11::2', prefixLength: 64, origin: 'slaac', preferred: false}])"
        )
        assert rows == [
            {"address": "fd00::1/64", "origin": "thread", "state": "preferred"},
            {"address": "fd11::2/64", "origin": "slaac", "state": "deprecated"},
        ]


@needs_node
class TestThreadCards:
    """status.js: the Thread cards of the dashboard"""

    def test_thread_cards_and_banner_line_fill_from_the_properties(self):
        # On a running border router: network and identifier fields, the banner line, PSKc hidden.
        r = scenario(
            page("index", ROUTES)
            + """
            await t.page.refreshStatus(); await t.page.refreshThread();
            return ['t-net-name','t-net-role','t-net-rloc16','t-id-rloc','t-net-channel','t-net-txpower','t-net-panid','t-net-xpanid','t-net-partition',
                    't-id-eui64','t-id-baid','t-id-otver','h-role'].map((i) => t.el(i).textContent)
                   .concat([t.el('t-id-pskc').textContent.includes('aabb'), t.el('t-net-off').hidden()]);
        """
        )
        assert r == [
            "MyHome",
            "leader",
            "0x7800",
            "fdde:ad00:beef:0:0:ff:fe00:7800",
            "15",
            "20 dBm",
            "0x1234",
            "dead00beef00cafe",
            "1745592081",
            "1122334455667788",
            "0123456789abcdef0123456789abcdef",
            "openthread/1.4.0; esp32p4",
            "Leader of MyHome on channel 15",
            False,
            True,
        ]

    def test_pskc_shows_only_after_show(self):
        # On Show: the PSKc in clear and the button says Hide; on Hide: dots again.
        r = scenario(
            page("index", ROUTES)
            + """
            t.page.init(); await t.advance(100);
            t.el('pskcBtn').click(); const shown = [t.el('t-id-pskc').textContent, t.el('pskcBtn').textContent];
            t.el('pskcBtn').click();
            return [shown, t.el('t-id-pskc').textContent.includes('aabb'), t.el('pskcBtn').textContent];
        """
        )
        assert r == [["aabbccddeeff00112233445566778899", "Hide"], False, "Show"]

    def test_address_table_lists_the_thread_addresses(self):
        # On /ipaddr: one row per address with prefix, origin and state.
        r = scenario(
            page("index", ROUTES)
            + "await t.page.refreshStatus(); await t.page.refreshThread(); return t.el('addr-body').children.map((tr) => tr.allText());"
        )
        assert r == [
            "fdde:ad00:beef:0:0:ff:fe00:fc00/64 thread preferred",
            "fd11:22::1/64 slaac deprecated",
        ]

    def test_no_network_yet_says_so(self):
        # On role disabled and no addresses: the not-in-a-network line, no rows.
        r = scenario(
            page("index", ROUTES)
            + """
            t.p = {error: 0, message: 'OK', result: {'Thread:Role': 'disabled'}}; t.a = {error: 0, message: 'OK', result: []};
            await t.page.refreshStatus(); await t.page.refreshThread();
            return [t.el('t-net-none').hidden(), t.el('addr-body').children.length];
        """
        )
        assert r == [False, 0]

    def test_outside_br_running_the_thread_data_is_not_asked_for(self):
        # On SNIFFER: no /get_properties or /ipaddr request; the cards say they show while running.
        r = scenario(
            page("index", ROUTES)
            + f"t.s = {status(mode='SNIFFER', sniffer={'active': True, 'channel': 15, 'frames': 3, 'drops': 0})};"
            + """
            t.page.init(); await t.advance(25000);
            return [t.calls.filter((c) => c.url === '/get_properties' || c.url === '/ipaddr').length, t.el('t-net-off').hidden()];
        """
        )
        assert r == [0, False]

    def test_network_name_is_rendered_as_text(self):
        # On a network name with markup: it lands in textContent unchanged.
        r = scenario(
            page("index", ROUTES)
            + "t.p = H.fixture(H.PROPS); t.p.result['Network:Name'] = '<b>x</b>'; await t.page.refreshStatus(); await t.page.refreshThread(); return t.el('t-net-name').textContent;"
        )
        assert r == "<b>x</b>"


@needs_node
class TestDashboardPrReview:
    """PR review findings"""

    def test_leaving_br_running_clears_the_thread_data(self):
        # On Thread data shown, then a SNIFFER status: fields back to "-", no address rows.
        r = scenario(
            page("index", ROUTES)
            + """
            await t.page.refreshStatus(); await t.page.refreshThread();
            t.s = H.fixture(H.STATUS, {mode: 'SNIFFER'}); await t.page.refreshStatus();
            return [t.el('t-net-name').textContent, t.el('t-id-rloc').textContent, t.el('addr-body').children.length];
        """
        )
        assert r == ["-", "-", 0]

    def test_a_late_answer_after_the_mode_changed_is_dropped(self):
        # On /get_properties answering after the status went to SNIFFER: nothing rendered, no /ipaddr request.
        r = scenario(
            page("index", ROUTES)
            + """
            await t.page.refreshStatus();
            const fetch = t.env.fetch; let release;
            t.env.fetch = (u, o) => u === '/get_properties'
                ? new Promise((res) => { release = () => res({status: 200, text: () => Promise.resolve(JSON.stringify(H.PROPS))}); })
                : fetch(u, o);
            const pending = t.page.refreshThread();
            t.s = H.fixture(H.STATUS, {mode: 'SNIFFER'}); await t.page.refreshStatus();
            release(); await pending;
            return [t.el('t-net-name').textContent, t.calls.filter((c) => c.url === '/ipaddr').length];
        """
        )
        assert r[0] in ("-", "")
        assert r[1] == 0

    def test_failed_address_list_clears_the_table(self):
        # On a good answer and then /ipaddr failing: the old rows are gone.
        r = scenario(
            page("index", ROUTES)
            + """
            await t.page.refreshStatus(); await t.page.refreshThread();
            t.a = {error: 1, message: 'busy', result: null}; await t.page.refreshThread();
            return t.el('addr-body').children.length;
        """
        )
        assert r == 0


@needs_node
class TestUxReview:
    """UX review: first-boot fault texts, the compatibility row and the Home Assistant tile"""

    @pytest.mark.parametrize(
        ("reason", "word"),
        [
            ("esp-hosted firmware", "c6_installer.bin"),
            ("rom recovery failed", "Recover radio"),
            ("storage unavailable", "storage"),
        ],
    )
    def test_first_boot_faults_say_what_to_do(self, reason, word):
        h = st(
            f"L.heroFor({{mode: 'RADIO_FAULT', fault_reason: '{reason}', sniffer: {{}}}})"
        )
        assert word in h["text"]
        assert "stopped answering" not in h["text"]

    @pytest.mark.parametrize(
        ("rcp", "text"),
        [
            ("{installed: null, compatible: false}", "unknown"),
            ("{installed: '', compatible: false}", "-"),
            ("{installed: '2.1', compatible: false}", "No"),
            ("{installed: '2.1', compatible: true}", "Yes"),
            ("{installed: '2.1', compatible: null}", "unknown"),
        ],
    )
    def test_compat_text(self, rcp, text):
        assert logic("device_common", f"L.compatText({rcp})") == text

    def test_ha_single_uses_the_port_address_and_the_mdns_name(self):
        v = st(
            "L.haView({mode: 'BR_RUNNING', eth: {ipv4: '10.0.0.5'}},"
            " {mode: 'single', active: {single: {ipv4: {dhcp: true}}},"
            "  interfaces: [{name: 'eth', vlan: 0, ipv4: '192.168.1.117'}]},"
            " {hostname_live: 'esp-otbr-d2cafc', ha_name_effective: 'Kitchen BR'})"
        )
        assert v["url"] == "http://192.168.1.117"
        assert v["mdns"] == "esp-otbr-d2cafc.local"
        assert v["haName"] == "Kitchen BR"
        assert v["iface"] == "Ethernet port"
        assert "reservation" in v["note"]

    def test_ha_split_uses_the_thread_backbone_not_the_management_address(self):
        v = st(
            "L.haView({mode: 'BR_RUNNING', eth: {ipv4: '10.0.0.5'}},"
            " {mode: 'split', active: {thread_bb: {ipv4: {dhcp: false}}},"
            "  interfaces: [{name: 'mgmt', vlan: 10, ipv4: '10.0.0.5'}, {name: 'thread_bb', vlan: 20, ipv4: '10.0.20.7'}]},"
            " null)"
        )
        assert v["url"] == "http://10.0.20.7"
        assert v["iface"] == "Thread backbone (VLAN 20)"
        assert "reservation" not in v["note"]

    def test_ha_without_network_or_names_answer(self):
        # No /device/network snapshot: no URL, rather than the status address that may be the wrong interface.
        v = st("L.haView({mode: 'RADIO_FAULT', eth: {ipv4: '10.0.0.5'}}, null, null)")
        assert v["url"] == "-"
        assert v["mdns"] == "-"
        assert v["haName"] == "-"
        assert "once the border router runs" in v["note"]

    def test_ha_tile_fills_from_status_network_and_names(self):
        routes = ROUTES.replace(
            "u === '/device/network' ? H.NETWORK",
            "u === '/device/network' ? H.NETWORK : u === '/device/names' ?"
            " {hostname_live: 'esp-otbr-d2cafc', ha_name_effective: 'esp-otbr-d2cafc'}",
        )
        r = scenario(
            page("index", routes)
            + """
            await t.page.refreshStatus(); await t.page.refreshNetwork();
            return [t.el('t-ha-url').textContent, t.el('t-ha-mdns').textContent, t.el('t-ha-name').textContent,
                    t.el('t-ha-iface').textContent];
        """
        )
        assert r == [
            "http://192.168.1.117",
            "esp-otbr-d2cafc.local",
            "esp-otbr-d2cafc",
            "Ethernet port",
        ]
