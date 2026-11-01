"""Device page (BDD: device page; spec 5.7, 5.13). Host-only static analysis of device.html and static/device.js.

The pure logic of device.js (URL building, error messages, form to JSON, poll gating) runs under node when it is installed;
those tests are skipped otherwise. Serving checks against the board are in p4/pytest/test_device_page.py (deferred to V3).
"""

import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
FRONTEND = REPO / "p4" / "components" / "esp_ot_br_server" / "frontend"
DEVICE_API = REPO / "p4" / "components" / "device_api" / "device_api.c"
HTML = FRONTEND / "device.html"
JS = FRONTEND / "static" / "device.js"
API_JS = FRONTEND / "static" / "api.js"
WEB_BUDGET_BYTES = (
    256 * 1024
)  # of the 2 MB web partition; the whole frontend today is about 130 KB

ENDPOINTS = [
    "/device/status",
    "/device/diag",
    "/device/network",
    "/device/network/confirm",
    "/device/sniffer.pcap",
    "/device/sniffer",
    "/device/ota",
    "/device/ota/url",
    "/device/radio/firmware",
    "/device/radio/recover",
    "/device/radio/log",
    "/device/reboot",
]

HARNESS = Path(__file__).resolve().parent / "device_page_harness.js"
C6_BOARD = REPO / "common" / "c6_board" / "c6_board.h"
PARTITIONS = REPO / "p4" / "partitions.csv"

needs_node = pytest.mark.skipif(
    shutil.which("node") is None, reason="node is not installed"
)


def logic(expression):
    """Evaluate a JS expression against the exported pure functions of device.js, return its JSON value."""
    script = f"const L = require({json.dumps(str(JS))}); process.stdout.write(JSON.stringify({expression}));"
    out = subprocess.run(
        ["node", "-e", script], capture_output=True, text=True, check=True, timeout=20
    )
    return json.loads(out.stdout) if out.stdout else None


class TestPageFiles:
    """device.html and static/device.js"""

    def test_page_links_the_shared_style_api_and_device_script(self):
        # On device.html: uses the shared stylesheet, nav placeholder, api.js and device.js.
        html = HTML.read_text()
        assert '<link rel="stylesheet" href="/static/style.css">' in html
        assert '<nav class="nav" id="nav"></nav>' in html
        assert '<script src="/static/api.js"></script>' in html
        assert '<script src="/static/device.js"></script>' in html

    @pytest.mark.parametrize("endpoint", ENDPOINTS)
    def test_script_references_each_device_endpoint(self, endpoint):
        # On device.js: names the endpoint as a quoted path.
        assert re.search(rf"['\"]{re.escape(endpoint)}['\"?]", JS.read_text())

    def test_script_never_inserts_server_data_as_markup(self):
        # On device.js: no innerHTML/outerHTML/insertAdjacentHTML/document.write; text goes through textContent.
        source = JS.read_text()
        assert not re.search(
            r"innerHTML|outerHTML|insertAdjacentHTML|document\.write", source
        )
        assert "textContent" in source

    def test_script_loads_no_library_and_no_remote_resource(self):
        # On both files: no script/link/src/href to a remote host, no import.
        for path in (HTML, JS):
            text = path.read_text()
            assert not re.search(r"(?:src|href)=[\"']?(?:https?:)?//", text)
            assert "import " not in text

    def test_every_element_id_the_script_looks_up_exists_in_the_page(self):
        # On device.js: each $('id') names an id in device.html (the net-* fields are created by the script).
        used = set(re.findall(r"\$\('([A-Za-z0-9_-]+)'\)", JS.read_text()))
        used |= set(re.findall(r"poller\([^)]*'([A-Za-z]+Section)'\)", JS.read_text()))
        present = set(re.findall(r'id="([^"]+)"', HTML.read_text()))
        assert used
        assert used <= present, used - present

    def test_navigation_links_the_device_page(self):
        # On api.js: the shared nav bar has a Device entry for /device.html.
        assert re.search(
            r"href : '/device\.html', label : 'Device'", API_JS.read_text()
        )

    def test_every_html_page_that_builds_the_nav_gets_it_from_api_js(self):
        # On the existing pages: the nav is built by api.js only, so no page needs its own link.
        for page in FRONTEND.glob("*.html"):
            text = page.read_text()
            if '<nav class="nav" id="nav">' in text:
                assert "/static/api.js" in text, page.name

    def test_frontend_stays_within_the_web_image_budget(self):
        # On the frontend directory: total size below the budget.
        total = sum(p.stat().st_size for p in FRONTEND.rglob("*") if p.is_file())
        assert total < WEB_BUDGET_BYTES


class TestRouting:
    """device_api URI table against the static file handler"""

    def test_registered_device_routes_are_exact_paths_under_device_slash(self):
        # On the route table: every URI starts with /device/ and has no wildcard, so /device.html reaches the /* file handler.
        source = DEVICE_API.read_text()
        uris = re.findall(r'\{"(/[^"]*)", HTTP_[A-Z]+, ', source)
        assert uris
        assert all(u.startswith("/device/") and "*" not in u for u in uris)

    def test_every_endpoint_the_page_uses_is_registered_by_the_api(self):
        # On the route table: all endpoints of the page exist in the device_api table.
        source = DEVICE_API.read_text()
        for endpoint in ENDPOINTS:
            assert f'"{endpoint}"' in source, endpoint


@needs_node
class TestLogic:
    """device.js pure functions"""

    def test_capture_url_carries_channel_and_duration(self):
        # On a valid channel and duration: /device/sniffer.pcap with both query parameters.
        assert (
            logic("L.captureUrl(15, 60)")
            == "/device/sniffer.pcap?channel=15&duration=60"
        )

    @pytest.mark.parametrize("channel", ["10", "27", "abc", ""])
    def test_capture_url_refuses_a_channel_outside_11_to_26(self, channel):
        # On a channel outside 11..26: no URL, a message.
        assert logic(f"L.captureUrl({json.dumps(channel)}, 60)") is None

    @pytest.mark.parametrize("duration", ["0", "3601", "x"])
    def test_capture_url_refuses_a_duration_outside_1_to_3600(self, duration):
        # On a duration outside 1..3600: no URL.
        assert logic(f"L.captureUrl(15, {json.dumps(duration)})") is None

    def test_radio_url_percent_encodes_the_source_url(self):
        # On a from_url: the query holds the encoded URL and the path is the single firmware endpoint.
        url = logic("L.radioFirmwareUrl('http://h/a b.bin?x=1&y=2')")
        assert (
            url
            == "/device/radio/firmware?from_url=http%3A%2F%2Fh%2Fa%20b.bin%3Fx%3D1%26y%3D2"
        )

    def test_radio_url_without_source_is_the_plain_endpoint(self):
        # On an upload: no query.
        assert logic("L.radioFirmwareUrl('')") == "/device/radio/firmware"

    def test_https_update_url_gets_a_clear_message(self):
        # On an https URL: the check says http only.
        assert "http://" in logic("L.checkUpdateUrl('https://example.org/x.bin')")

    def test_http_update_url_is_accepted(self):
        # On an http URL: no message.
        assert logic("L.checkUpdateUrl('http://example.org/x.bin')") is None

    def test_empty_update_url_gets_a_message(self):
        # On an empty URL: a message.
        assert logic("L.checkUpdateUrl('  ')")

    def test_error_message_shows_the_detail_from_the_body(self):
        # On a JSON error body: the detail text is part of the message.
        msg = logic("L.errorMessage(400, {error: 'invalid_bundle', detail: 'bad sha'})")
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
        msg = logic(f"L.errorMessage({status}, {{error: 'x', detail: 'd'}})")
        assert word in msg.lower()
        assert "d" in msg

    def test_error_message_without_json_body_still_names_the_status(self):
        # On a non-JSON body: the HTTP status is named.
        assert "500" in logic("L.errorMessage(500, null)")

    def test_fault_no_rcp_firmware_highlights_the_upload(self):
        # On fault reason "no rcp firmware": the hint asks for the RCP upload and marks it.
        hint = logic("L.faultHint('no rcp firmware')")
        assert hint["highlight"] is True
        assert hint["text"]

    def test_fault_rcp_incompatible_highlights_the_upload(self):
        # On fault reason "rcp incompatible": same.
        assert logic("L.faultHint('rcp incompatible')")["highlight"] is True

    @pytest.mark.parametrize("reason", ["null", "'other'"])
    def test_other_fault_reasons_do_not_highlight(self, reason):
        # On no or another fault reason: no highlight.
        assert logic(f"L.faultHint({reason})")["highlight"] is False

    def test_recover_controls_follow_the_jumper(self):
        # On jumper present true/false/missing: shown only for true.
        assert logic("L.jumperPresent({jumper: {present: true}})") is True
        assert logic("L.jumperPresent({jumper: {present: false}})") is False
        assert logic("L.jumperPresent({})") is False

    def test_rcp_installed_text_tells_none_unknown_and_version(self):
        # On rcp.installed "" / null / "1.4": no firmware / unknown / the version.
        assert "no" in logic("L.rcpInstalledText('')").lower()
        assert "unknown" in logic("L.rcpInstalledText(null)").lower()
        assert logic("L.rcpInstalledText('1.4')") == "1.4"

    def test_remaining_time_is_minutes_and_seconds(self):
        # On 299 s: "4:59".
        assert logic("L.formatRemaining(299)") == "4:59"
        assert logic("L.formatRemaining(0)") == "0:00"

    def test_network_form_for_single_dhcp_gives_the_api_shape(self):
        # On single mode with DHCP: {"mode","single":{"vlan","ipv4":{"dhcp":true}}}.
        form = {"mode": "single", "single": {"vlan": "0", "dhcp": True}}
        assert logic(f"L.networkConfig({json.dumps(form)})") == {
            "mode": "single",
            "single": {"vlan": 0, "ipv4": {"dhcp": True}},
        }

    def test_network_form_for_split_static_sends_both_sections_with_addresses(self):
        # On split mode with a static thread_bb: both sections, gw/dns omitted when empty.
        form = {
            "mode": "split",
            "mgmt": {"vlan": "10", "dhcp": True},
            "thread_bb": {
                "vlan": "20",
                "dhcp": False,
                "addr": "10.0.0.2",
                "mask": "255.255.255.0",
                "gw": "",
                "dns": "10.0.0.1",
            },
        }
        assert logic(f"L.networkConfig({json.dumps(form)})") == {
            "mode": "split",
            "mgmt": {"vlan": 10, "ipv4": {"dhcp": True}},
            "thread_bb": {
                "vlan": 20,
                "ipv4": {
                    "dhcp": False,
                    "addr": "10.0.0.2",
                    "mask": "255.255.255.0",
                    "dns": "10.0.0.1",
                },
            },
        }

    @pytest.mark.parametrize("vlan", ["-1", "4095", "1.5", "x", ""])
    def test_network_form_with_a_bad_vlan_throws(self, vlan):
        # On a VLAN outside 0..4094 or not an integer: throws with a message.
        form = {"mode": "single", "single": {"vlan": vlan, "dhcp": True}}
        assert logic(
            f"(()=>{{try{{L.networkConfig({json.dumps(form)})}}catch(e){{return e.message}}}})()"
        )

    def test_network_form_static_without_address_throws(self):
        # On static IPv4 without addr or mask: throws.
        form = {
            "mode": "single",
            "single": {"vlan": "0", "dhcp": False, "addr": "", "mask": ""},
        }
        assert logic(
            f"(()=>{{try{{L.networkConfig({json.dumps(form)})}}catch(e){{return e.message}}}})()"
        )

    def test_polling_runs_only_while_visible_and_not_in_flight(self):
        # On hidden tab / section out of view / request in flight: no poll; otherwise poll.
        assert (
            logic(
                "L.shouldPoll({hidden: false, sectionVisible: true, inFlight: false})"
            )
            is True
        )
        assert (
            logic("L.shouldPoll({hidden: true, sectionVisible: true, inFlight: false})")
            is False
        )
        assert (
            logic(
                "L.shouldPoll({hidden: false, sectionVisible: false, inFlight: false})"
            )
            is False
        )
        assert (
            logic("L.shouldPoll({hidden: false, sectionVisible: true, inFlight: true})")
            is False
        )

    def test_core_label_names_a_number_or_any(self):
        # On task core 1 / "any": "1" / "any".
        assert logic("L.coreLabel(1)") == "1"
        assert logic("L.coreLabel('any')") == "any"

    def test_load_text_has_one_decimal_and_percent(self):
        # On a load object: "1s / 10s / 60s" values joined.
        assert (
            logic("L.loadText({'1s': 12.5, '10s': 3, '60s': 0.04})")
            == "12.5% / 3.0% / 0.0%"
        )

    def test_bytes_text_is_kilobytes_with_one_decimal(self):
        # On 1536 bytes: "1.5 KB".
        assert logic("L.bytesText(1536)") == "1.5 KB"


def scenario(body):
    """Run `body` (async JS using H = harness) under node; return its JSON result."""
    script = (
        f"const H = require({json.dumps(str(HARNESS))});"
        f"(async () => {{ const r = await (async () => {{ {body} }})(); process.stdout.write(JSON.stringify(r)); }})()"
        ".catch(e => { console.error(e); process.exit(1); });"
    )
    out = subprocess.run(
        ["node", "-e", script], capture_output=True, text=True, check=True, timeout=60
    )
    return json.loads(out.stdout) if out.stdout else None


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
        assert logic("L.RCP_MAX_BYTES") == slot

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

        assert logic("L.BUNDLE_MAX_BYTES") == 44 + size("web_0") + size("ota_0")

    def test_file_over_the_limit_is_refused_before_sending(self):
        # On a file larger than the limit: a message naming the limit; at the limit: none; empty: a message.
        assert "Too large" in logic("L.checkUploadSize('rcp', L.RCP_MAX_BYTES + 1)")
        assert logic("L.checkUploadSize('rcp', L.RCP_MAX_BYTES)") is None
        assert logic("L.checkUploadSize('bundle', L.BUNDLE_MAX_BYTES)") is None
        assert "empty" in logic("L.checkUploadSize('bundle', 0)")


@needs_node
class TestNetworkValidation:
    """networkConfig client-side validation"""

    @staticmethod
    def error_of(form):
        return logic(
            f"(()=>{{try{{L.networkConfig({json.dumps(form)});return null}}catch(e){{return e.message}}}})()"
        )

    @staticmethod
    def static(**kw):
        return {
            "vlan": "0",
            "dhcp": False,
            "addr": "10.0.0.2",
            "mask": "255.255.255.0",
        } | kw

    def test_split_with_equal_vlan_ids_is_refused(self):
        # On mgmt and thread_bb both VLAN 20: a message about different VLAN IDs.
        form = {
            "mode": "split",
            "mgmt": {"vlan": "20", "dhcp": True},
            "thread_bb": {"vlan": "20", "dhcp": True},
        }
        assert "different VLAN" in self.error_of(form)

    def test_split_with_two_untagged_interfaces_is_refused(self):
        # On both VLAN 0: refused as well.
        form = {
            "mode": "split",
            "mgmt": {"vlan": "0", "dhcp": True},
            "thread_bb": {"vlan": "0", "dhcp": True},
        }
        assert "different VLAN" in self.error_of(form)

    def test_split_with_one_untagged_and_one_tagged_is_accepted(self):
        # On VLAN 0 and 20: no error.
        form = {
            "mode": "split",
            "mgmt": {"vlan": "0", "dhcp": True},
            "thread_bb": {"vlan": "20", "dhcp": True},
        }
        assert self.error_of(form) is None

    @pytest.mark.parametrize("vlan", ["0", "4094"])
    def test_vlan_bounds_are_accepted(self, vlan):
        # On VLAN 0 and 4094: no error.
        assert (
            self.error_of({"mode": "single", "single": {"vlan": vlan, "dhcp": True}})
            is None
        )

    @pytest.mark.parametrize(
        "addr",
        ["10.0.0", "10.0.0.256", "a.b.c.d", "10.0.0.1.5", "10..0.1", "1e1.0.0.1"],
    )
    def test_malformed_address_is_refused(self, addr):
        # On an address that is not a dotted quad: a message about the address.
        assert "address" in self.error_of(
            {"mode": "single", "single": self.static(addr=addr)}
        )

    @pytest.mark.parametrize(
        "mask", ["255.0.255.0", "0.0.0.0", "255.255.255", "255.255.255.1", "256.0.0.0"]
    )
    def test_invalid_netmask_is_refused(self, mask):
        # On a non-contiguous, zero or malformed mask: a message about the netmask.
        assert "netmask" in self.error_of(
            {"mode": "single", "single": self.static(mask=mask)}
        )

    @pytest.mark.parametrize(
        "mask", ["255.255.255.0", "255.255.0.0", "255.255.255.252", "128.0.0.0"]
    )
    def test_contiguous_netmask_is_accepted(self, mask):
        # On a contiguous mask: no error.
        assert (
            self.error_of({"mode": "single", "single": self.static(mask=mask)}) is None
        )

    @pytest.mark.parametrize("field", ["gw", "dns"])
    def test_malformed_gateway_or_dns_is_refused(self, field):
        # On a bad optional gw/dns: refused; empty is fine.
        assert self.error_of(
            {"mode": "single", "single": self.static(**{field: "10.0.0.999"})}
        )
        assert (
            self.error_of({"mode": "single", "single": self.static(**{field: ""})})
            is None
        )


@needs_node
class TestBackoffAndWait:
    """nextDelay, lastSeenText, bootObserved"""

    def test_delay_doubles_per_failure_up_to_30_seconds(self):
        # On period 3000 and failures 0..5: 3000, 6000, 12000, 24000, 30000, 30000.
        assert logic("[0,1,2,3,4,5].map(f => L.nextDelay(3000, f))") == [
            3000,
            6000,
            12000,
            24000,
            30000,
            30000,
        ]

    def test_last_seen_text_names_the_seconds(self):
        # On a last success 12 s ago: "(last seen 12 s ago)"; never seen: no number.
        assert "last seen 12 s ago" in logic("L.lastSeenText(1000, 13000)")
        assert "last seen" not in logic("L.lastSeenText(0, 13000)")

    def test_boot_is_observed_after_an_outage_and_a_success(self):
        # On fail then ok: true on the ok.
        assert logic(
            "(()=>{const s={outage:false,baseline:100};return [L.bootObserved(s,{ok:false}),L.bootObserved(s,{ok:true,uptime:500})]})()"
        ) == [False, True]

    def test_boot_is_observed_when_uptime_goes_backwards_without_outage(self):
        # On a baseline of 100 s and a probe with 5 s: true.
        assert (
            logic("L.bootObserved({outage:false,baseline:100},{ok:true,uptime:5})")
            is True
        )

    def test_boot_is_not_observed_while_the_old_board_keeps_answering(self):
        # On a baseline of 100 s and a probe with 103 s: false.
        assert (
            logic("L.bootObserved({outage:false,baseline:100},{ok:true,uptime:103})")
            is False
        )

    def test_first_probe_without_baseline_only_sets_the_baseline(self):
        # On no baseline: the first success is not accepted.
        assert logic(
            "(()=>{const s={outage:false,baseline:null};return [L.bootObserved(s,{ok:true,uptime:50}),s.baseline]})()"
        ) == [False, 50]


def make(routes_js="() => ({status: 200, body: {}})", prelude=""):
    return f"const t = H.makeEnv({routes_js}); {prelude}"


@needs_node
class TestPageCode:
    """device.js run against a fake DOM and fetch"""

    def test_bundle_upload_sends_the_file_object_as_body_with_octet_stream(self):
        # On a File-like object: POST /device/ota with that same object (not FormData, not chunked), then busy hands off.
        r = scenario(
            make("(u) => ({status: 200, body: {status: 'ok'}})")
            + """
            const file = {name: 'b.bin', size: 1234};
            await t.page.sendFile('/device/ota', file, 'otaMsg', 'bundle');
            const c = t.calls[0];
            return {url: c.url, method: c.method, same: c.body === file, type: c.headers['Content-Type']};
        """
        )
        assert r == {
            "url": "/device/ota",
            "method": "POST",
            "same": True,
            "type": "application/octet-stream",
        }

    def test_stop_sends_delete_to_the_sniffer_endpoint(self):
        # On Stop: DELETE /device/sniffer.
        r = scenario(
            make(
                "(u) => ({status: 200, body: u === '/device/status' ? H.STATUS : {status: 'ok'}})"
            )
            + """
            await t.page.stopCapture();
            return [t.calls[0].method, t.calls[0].url, t.el('capMsg').textContent];
        """
        )
        assert r == ["DELETE", "/device/sniffer", "Capture stopped."]

    def test_ota_by_url_sends_json_with_the_url_only(self):
        # On OTA by URL: POST /device/ota/url with body {"url": ...} and JSON content type.
        r = scenario(
            make()
            + """
            await t.page.sendUrl('/device/ota/url', ' http://h/b.bin ', 'otaMsg', 'bundle', true);
            const c = t.calls[0];
            return [c.url, c.method, JSON.parse(c.body), c.headers['Content-Type']];
        """
        )
        assert r == [
            "/device/ota/url",
            "POST",
            {"url": "http://h/b.bin"},
            "application/json",
        ]

    def test_firmware_from_url_sends_an_empty_body_and_the_encoded_query(self):
        # On radio firmware by URL: POST with no body and from_url percent-encoded.
        r = scenario(
            make()
            + """
            const u = 'http://h/c6 rcp.bin?a=1';
            await t.page.sendUrl(t.Device.radioFirmwareUrl(u), u, 'rcpMsg', 'rcp', false);
            const c = t.calls[0];
            return [c.url, c.method, c.body === undefined];
        """
        )
        assert r == [
            "/device/radio/firmware?from_url=http%3A%2F%2Fh%2Fc6%20rcp.bin%3Fa%3D1",
            "POST",
            True,
        ]

    def test_https_url_is_refused_without_a_request(self):
        # On an https source: a message and no request.
        r = scenario(
            make()
            + """
            await t.page.sendUrl('/device/ota/url', 'https://h/b.bin', 'otaMsg', 'bundle', true);
            return [t.calls.length, t.el('otaMsg').textContent];
        """
        )
        assert r[0] == 0
        assert "http" in r[1]

    @pytest.mark.parametrize(
        ("installed", "expected"),
        [
            ("null", "unknown (probe failed)"),
            ("''", "none (no valid RCP firmware)"),
            ("'2.1'", "2.1"),
        ],
    )
    def test_rcp_installed_renders_through_the_status_card(self, installed, expected):
        # On rcp.installed null / "" / string: the RCP row shows unknown / none / the version.
        r = scenario(
            make()
            + f"""
            const s = JSON.parse(JSON.stringify(H.STATUS)); s.rcp.installed = {installed};
            t.page.renderStatus(s);
            return t.el('s-rcp').textContent;
        """
        )
        assert r == expected

    def test_server_text_is_rendered_as_text_not_markup(self):
        # On a fault reason with markup: it lands in textContent unchanged.
        r = scenario(
            make()
            + """
            const s = JSON.parse(JSON.stringify(H.STATUS)); s.fault_reason = '<img src=x onerror=alert(1)>';
            t.page.renderStatus(s);
            return t.el('s-fault').textContent;
        """
        )
        assert r == "<img src=x onerror=alert(1)>"

    def test_recover_controls_are_hidden_without_the_jumper(self):
        # On jumper.present false: jumperControls hidden; true: shown.
        r = scenario(
            make()
            + """
            const s = JSON.parse(JSON.stringify(H.STATUS)); s.jumper.present = false;
            t.page.renderStatus(s); const off = t.el('jumperControls').hidden();
            t.page.renderStatus(H.STATUS); const on = !t.el('jumperControls').hidden();
            return [off, on];
        """
        )
        assert r == [True, True]

    def test_fault_no_rcp_firmware_outlines_the_upload_control(self):
        # On "no rcp firmware": outline set and hint shown; cleared after.
        r = scenario(
            make()
            + """
            const s = JSON.parse(JSON.stringify(H.STATUS)); s.fault_reason = 'no rcp firmware';
            t.page.renderStatus(s); const on = [t.el('radioUpload').style.outline, !t.el('radioHint').hidden()];
            t.page.renderStatus(H.STATUS);
            return [on, t.el('radioUpload').style.outline];
        """
        )
        assert r[0][0] and r[0][1] is True
        assert r[1] == ""

    def test_trial_countdown_resyncs_from_the_server_each_answer(self):
        # On remaining_s 100, 40 s pass, then the server says 90: the banner shows 1:30 again.
        r = scenario(
            make()
            + """
            const net = (rem) => ({mode: 'single', active: {mode: 'single', single: {vlan: 0, ipv4: {dhcp: true}}},
              trial: {mode: 'single', single: {vlan: 0, ipv4: {dhcp: true}}, remaining_s: rem},
              interfaces: [], fallback: false, vlan_rx_dropped: 0});
            t.page.buildNetForm(); t.page.renderNetwork(net(100), true);
            const first = t.el('trialRemaining').textContent;
            await t.advance(40000); t.page.tick(); const later = t.el('trialRemaining').textContent;
            t.page.renderNetwork(net(90), false);
            return [first, later, t.el('trialRemaining').textContent, !t.el('trialBanner').hidden()];
        """
        )
        assert r == ["1:40", "1:00", "1:30", True]

    def test_no_trial_hides_the_banner(self):
        # On trial null: the banner is hidden.
        r = scenario(
            make()
            + """
            const base = {mode: 'single', active: {mode: 'single', single: {vlan: 0, ipv4: {dhcp: true}}},
              interfaces: [], fallback: false, vlan_rx_dropped: 0};
            t.page.buildNetForm(); t.page.renderNetwork(Object.assign({trial: {mode: 'single', single: {vlan: 0, ipv4: {dhcp: true}}, remaining_s: 5}}, base), true);
            t.page.renderNetwork(Object.assign({trial: null}, base), false);
            return t.el('trialBanner').hidden();
        """
        )
        assert r is True

    def test_busy_disables_every_action_and_a_failure_enables_them_again(self):
        # On an upload in flight: all action buttons disabled and a second upload sends nothing; after a 409 they are enabled.
        r = scenario(
            make("(u) => ({status: 409, body: {error: 'busy', detail: 'x'}})")
            + """
            const ids = ['capStart','capStop','otaUpload','otaFetch','rcpUpload','rcpFetch','recoverBtn','logBtn','rebootBtn','netApply','confirmBtn'];
            const file = {name: 'b.bin', size: 10};
            const first = t.page.sendFile('/device/ota', file, 'otaMsg', 'bundle');
            const during = ids.map((i) => t.el(i).disabled);
            const second = await t.page.sendFile('/device/ota', file, 'otaMsg', 'bundle');
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

    def test_successful_bundle_install_keeps_the_buttons_disabled_for_the_reboot_wait(
        self,
    ):
        # On a 200 from /device/ota: still busy (the wait for the reboot took over).
        r = scenario(
            make("(u) => ({status: 200, body: {status: 'ok'}})")
            + """
            await t.page.sendFile('/device/ota', {name: 'b', size: 10}, 'otaMsg', 'bundle');
            return [t.page.isBusy(), t.page.isWaiting(), t.el('rebootBtn').disabled];
        """
        )
        assert r == [True, True, True]

    def test_oversize_file_is_refused_without_a_request(self):
        # On a firmware file over the C6 slot: a Too large message and no request.
        r = scenario(
            make()
            + """
            await t.page.sendFile('/device/radio/firmware', {name: 'x', size: H.Device.RCP_MAX_BYTES + 1}, 'rcpMsg', 'rcp');
            return [t.calls.length, t.el('rcpMsg').textContent, t.page.isBusy()];
        """
        )
        assert r[0] == 0
        assert "Too large" in r[1]
        assert r[2] is False

    def test_dropped_upload_connection_says_the_board_closed_it(self):
        # On a connection error during an upload: the message names a rejected upload, not a reboot.
        r = scenario(
            make("() => { throw new Error('reset'); }")
            + """
            await t.page.sendFile('/device/ota', {name: 'b', size: 10}, 'otaMsg', 'bundle');
            return [t.el('otaMsg').textContent, t.page.isBusy()];
        """
        )
        assert "closed the connection" in r[0]
        assert "rebooting" not in r[0]
        assert r[1] is False

    def test_network_apply_with_invalid_input_sends_nothing(self):
        # On an invalid VLAN in the form: message in netMsg, no request, not busy.
        r = scenario(
            make()
            + """
            t.page.buildNetForm();
            t.el('netMode').value = 'single';
            t.el('net-single-vlan').value = '5000'; t.el('net-single-dhcp').checked = true;
            await t.page.applyNetwork();
            return [t.calls.length, t.el('netMsg').textContent, t.page.isBusy()];
        """
        )
        assert r[0] == 0
        assert "VLAN" in r[1]
        assert r[2] is False

    def test_network_apply_puts_the_json_and_waits_for_the_board(self):
        # On a valid form: PUT /device/network with the config, then busy with a single waiter.
        r = scenario(
            make("(u, o) => ({status: 200, body: {status: 'trial'}})")
            + """
            t.page.buildNetForm();
            t.el('netMode').value = 'single';
            t.el('net-single-vlan').value = '0'; t.el('net-single-dhcp').checked = true;
            await t.page.applyNetwork();
            const put = t.calls.find((c) => c.method === 'PUT');
            return [put.url, JSON.parse(put.body), t.page.isBusy(), t.page.isWaiting()];
        """
        )
        assert r == [
            "/device/network",
            {"mode": "single", "single": {"vlan": 0, "ipv4": {"dhcp": True}}},
            True,
            True,
        ]

    def test_poller_does_not_overlap_requests(self):
        # On a slow answer: never more than one status request in flight.
        r = scenario(
            make("(u) => ({status: 200, body: H.STATUS})")
            + """
            t.page.poller(t.page.refreshStatus, 100, 'statusSection');
            await t.advance(2000);
            return [t.maxInFlight(), t.calls.length > 3];
        """
        )
        assert r == [1, True]

    def test_poller_pauses_while_the_tab_is_hidden_and_resumes(self):
        # On a hidden tab: no new request; on visibilitychange to visible: requests continue.
        r = scenario(
            make("(u) => ({status: 200, body: H.STATUS})")
            + """
            t.page.poller(t.page.refreshStatus, 100, 'statusSection');
            await t.advance(500);
            t.doc.hidden = true; await t.advance(500);
            const n = t.calls.length; await t.advance(5000);
            const hiddenCalls = t.calls.length - n;
            t.doc.hidden = false; t.doc.fire('visibilitychange'); await t.advance(500);
            return [hiddenCalls, t.calls.length > n];
        """
        )
        assert r == [0, True]

    def test_poller_backs_off_on_failure_and_shows_the_offline_state(self):
        # On failing answers: gaps grow (3 s, 6 s, ...) and the Status card says no answer with last-seen seconds;
        # the first success clears it and resets the gap.
        r = scenario(
            make(
                "(u) => { if (t.fail) throw new Error('down'); return {status: 200, body: H.STATUS}; }"
            )
            + """
            t.fail = false;
            const p = t.page.poller(t.page.refreshStatus, 3000, 'statusSection');
            await t.advance(100);
            t.fail = true;
            await t.advance(60000);
            const times = t.calls.length;
            const offline = [!t.el('s-offline').hidden(), t.el('s-offline').textContent, p.failures()];
            t.fail = false; await t.advance(31000);
            return {times, offline, cleared: t.el('s-offline').hidden(), failures: p.failures()};
        """
        )
        assert r["times"] <= 8  # 3 s fixed would be about 20 requests in 60 s
        assert r["offline"][0] is True
        assert "No answer from the device (last seen" in r["offline"][1]
        assert r["offline"][2] >= 3
        assert r["cleared"] is True
        assert r["failures"] == 0

    def test_polling_stops_while_a_long_operation_runs(self):
        # On busy: no status/diag/network requests; after the operation fails they resume.
        r = scenario(
            make(
                "(u) => ({status: u === '/device/ota' ? 409 : 200, body: u === '/device/ota' ? {error: 'busy', detail: ''} : H.STATUS})"
            )
            + """
            t.page.poller(t.page.refreshStatus, 100, 'statusSection');
            await t.advance(300);
            t.page.setBusy(true);
            await t.advance(300);
            const n = t.calls.length; await t.advance(3000);
            const during = t.calls.length - n;
            t.page.setBusy(false); await t.advance(500);
            return [during, t.calls.length > n];
        """
        )
        assert r == [0, True]

    def test_wait_for_board_reloads_after_an_outage_and_a_success(self):
        # On diag failing twice then answering: reload once, and only one waiter can run.
        r = scenario(
            make(
                "(u) => { t.n = (t.n || 0) + 1; if (t.n === 2 || t.n === 3) throw new Error('down'); return {status: 200, body: {uptime_s: t.n}}; }"
            )
            + """
            t.page.setBusy(true);
            t.page.waitForBoard('netMsg'); t.page.waitForBoard('netMsg');
            await t.advance(20000);
            return [t.reloads(), t.calls.filter((c) => c.url === '/device/diag').length >= 4];
        """
        )
        assert r == [1, True]

    def test_wait_for_board_gives_up_with_a_hint_and_enables_the_buttons(self):
        # On a board that never goes away: after 3 minutes a message about IP/VLAN and the 5 minute revert.
        r = scenario(
            make("(u) => ({status: 200, body: {uptime_s: 100}})")
            + """
            t.page.setBusy(true);
            t.page.waitForBoard('netMsg');
            await t.advance(200000);
            return [t.reloads(), t.el('netMsg').textContent, t.page.isBusy(), t.page.isWaiting()];
        """
        )
        assert r[0] == 0
        assert "VLAN" in r[1]
        assert "5 minutes" in r[1]
        assert r[2] is False
        assert r[3] is False

    def test_confirm_result_is_shown_beside_the_banner(self):
        # On a 403 from confirm: confirmMsg shows the detail.
        r = scenario(
            make(
                "(u, o) => o.method === 'POST' ? ({status: 403, body: {error: 'wrong_interface', detail: 'use mgmt'}}) : ({status: 200, body: {mode: 'single', active: {mode: 'single', single: {vlan: 0, ipv4: {dhcp: true}}}, trial: null, interfaces: [], fallback: false, vlan_rx_dropped: 0}})"
            )
            + """
            await t.page.confirmTrial();
            return t.el('confirmMsg').textContent;
        """
        )
        assert "use mgmt" in r

    def test_network_poll_error_does_not_overwrite_the_action_message(self):
        # On a failing network poll after an action message: netMsg is unchanged, n-stale shows the problem.
        r = scenario(
            make("() => { throw new Error('down'); }")
            + """
            t.el('netMsg').textContent = 'Stored.';
            await t.page.refreshNetwork();
            return [t.el('netMsg').textContent, !t.el('n-stale').hidden()];
        """
        )
        assert r == ["Stored.", True]

    def test_message_areas_are_live_regions(self):
        # On device.html: each message area carries aria-live.
        html = HTML.read_text()
        for ident in [
            "capMsg",
            "otaMsg",
            "rcpMsg",
            "recoverMsg",
            "rebootMsg",
            "netMsg",
            "confirmMsg",
            "s-offline",
        ]:
            assert re.search(rf'id="{ident}"[^>]*aria-live="polite"', html), ident
