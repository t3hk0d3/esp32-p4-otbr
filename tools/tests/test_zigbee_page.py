"""Zigbee page, nav slot and Dashboard card (BDD: ZigbeeLogic, ZigbeePage, api.js nav, StatusPage).

Runs static/zigbee.js, status.js and api.js under node.
"""

import json
from copy import deepcopy

import pytest
from device_js import FRONTEND, STATIC, logic, needs_node, page, scenario

RUNNING = {
    "channel": "usb-303a-1001",
    "firmware": "spinel 1.2 efr32",
    "state": "running",
    "reason": "",
    "client": {"address": "192.168.1.20:51234", "since": 1790000000},
    "last_client": None,
    "radio": {
        "channel": 15,
        "pan_id": 10484,
        "short_address": 0,
        "extended_address": "d885acfffef9cb4c",
        "tx_power_dbm": 20,
    },
    "counters": {
        "rx": 48913,
        "rx_dropped": 0,
        "tx": 31004,
        "tx_no_ack": 212,
        "tx_cca": 9,
        "tx_failed": 2,
        "energy_scans": 16,
        "rcp_resets": 0,
        "frames_in": 18204,
        "frames_out": 21877,
    },
    "heap": {"current": 188416, "peak": 236544},
    "started": 1789999000,
}


def body(**patch):
    b = deepcopy(RUNNING)
    b.update(patch)
    return b


WAITING = body(
    client=None, last_client={"address": "192.168.1.10:40000", "left": 1790001000}
)
NO_RADIO = body(state="no_radio", client=None, radio=None)


def z(expression):
    return logic("zigbee", expression)


DASH_STATUS = "Object.assign(H.fixture(H.STATUS), {zigbee_channel: t.zc === undefined ? 'c6' : t.zc})"


def routes(b, status=200):
    """Fake Dashboard routes: /device/zigbee answers `b` with `status` (t.zb / t.z409 override it at run time)."""
    return (
        "(u) => u === '/device/zigbee' ? (t.zfail ? (() => { throw new Error('down'); })()"
        f" : t.z409 ? {{status: 409, body: {{}}}} : {{status: {status}, body: t.zb || {json.dumps(b)}}})"
        " : ({status: 200, body: u === '/device/diag' ? H.DIAG : u === '/device/network' ? H.NETWORK"
        " : u === '/get_properties' ? H.PROPS : u === '/ipaddr' ? H.IPADDR"
        f" : u === '/device/channels' ? {{channels: []}} : {DASH_STATUS}}})"
    )


def run(b, tail, status=200):
    """The Dashboard with a Zigbee channel: one status read, then one /device/zigbee read answering `b`."""
    return scenario(
        page("index", routes(b, status))
        + "await t.page.refreshStatus(); await t.page.refreshZigbee();"
        + tail
    )


@needs_node
class TestZigbeeLogic:
    """ZigbeeLogic pure functions"""

    def test_running_with_a_client_is_the_green_hero(self):
        # On running with a client: "Coordinator running", tone ok, the text names channel 15.
        h = z(f"L.heroFor({json.dumps(RUNNING)})")
        assert (h["title"], h["tone"]) == ("Coordinator running", "ok")
        assert "Channel 15" in h["text"] and "ZHA is connected" in h["text"]

    def test_running_without_a_client_waits_for_home_assistant(self):
        # On running without a client: "Waiting for Home Assistant", tone info.
        h = z(f"L.heroFor({json.dumps(WAITING)})")
        assert (h["title"], h["tone"]) == ("Waiting for Home Assistant", "info")

    def test_no_radio_is_a_fault(self):
        # On no_radio: "No radio", tone fault.
        h = z(f"L.heroFor({json.dumps(NO_RADIO)})")
        assert (h["title"], h["tone"]) == ("No radio", "fault")

    def test_starting_is_idle(self):
        # On starting: "Starting", tone idle.
        h = z(f"L.heroFor({json.dumps(body(state='starting'))})")
        assert (h["title"], h["tone"]) == ("Starting", "idle")

    @pytest.mark.parametrize("state", ["idle", "incompatible"])
    def test_idle_and_incompatible_show_the_reason(self, state):
        # On idle / incompatible: tone info, the text is the status reason.
        h = z(
            f"L.heroFor({json.dumps(body(state=state, reason='waiting for the radio'))})"
        )
        assert h["tone"] == "info"
        assert h["text"] == "waiting for the radio"

    def test_poll_period_is_2_s_while_starting_and_5_s_otherwise(self):
        # On starting: 2000 ms; on the other states and on none: 5000 ms.
        assert z("[L.pollMs('starting'), L.pollMs('running'), L.pollMs(null)]") == [
            2000,
            5000,
            5000,
        ]

    def test_epoch_zero_is_clock_not_set(self):
        # On 0: "clock not set"; on null: "-"; on a real time: a date.
        assert z("[L.epochText(0), L.epochText(null)]") == ["clock not set", "-"]
        assert "2026" in z("L.epochText(1790000000)")

    def test_zha_path_uses_the_page_host(self):
        # On host board.lan: ws://board.lan/zigbee.
        assert z("L.zhaPath('board.lan')") == "ws://board.lan/zigbee"

    def test_radio_ids_read_as_names(self):
        # On "c6": the on-board C6; on a USB id: that id.
        assert z("[L.radioName('c6'), L.radioName('usb-303a-1001')]") == [
            "on-board C6",
            "USB radio usb-303a-1001",
        ]

    def test_addresses_are_formatted(self):
        # On pan 10484 / short 0 / extended d885acfffef9cb4c: 0x28F4 / 0x0000 / d8:85:ac:ff:fe:f9:cb:4c.
        assert z(
            "[L.hex16(10484), L.hex16(0), L.extendedText('d885acfffef9cb4c')]"
        ) == ["0x28F4", "0x0000", "d8:85:ac:ff:fe:f9:cb:4c"]

    def test_counts_are_grouped_by_thousands(self):
        # On 48913 / 7: "48 913" / "7".
        assert z("[L.count(48913), L.count(7)]") == ["48 913", "7"]


@needs_node
class TestZigbeeSection:
    """ZigbeeSection: the Dashboard's Zigbee section, run against a fake DOM and fetch"""

    def test_running_page_is_filled_from_the_body(self):
        # On a running body: hero, radio, network, traffic, memory and firmware cards show its figures.
        r = run(
            RUNNING,
            """
            return [t.el('zg-title').textContent, t.el('zg-hero').className, t.el('zg-radio').textContent,
                    t.el('zg-resets').textContent, t.el('zg-client').allText(), t.el('zg-net').allText(),
                    t.el('zg-traffic').allText(), t.el('zg-heap').textContent, t.el('zg-heap-use').textContent,
                    t.el('zg-fw').allText(), t.el('zigbee-on').hidden(), t.el('zigbee-off').hidden()];
        """,
        )
        assert r[0] == "Coordinator running"
        assert "hero-ok" in r[1]
        assert r[2] == "USB radio usb-303a-1001"
        assert r[3] == "0"
        assert "192.168.1.20:51234" in r[4] and "18 204 / 21 877" in r[4]
        assert (
            "15" in r[5]
            and "0x28F4" in r[5]
            and "d8:85:ac:ff:fe:f9:cb:4c" in r[5]
            and "20 dBm" in r[5]
        )
        assert "48 913" in r[6] and "212" in r[6] and "16" in r[6]
        assert r[7] == "184.0 KB" and "peak 231.0 KB" in r[8]
        assert "spinel 1.2 efr32" in r[9]
        assert r[10] is False and r[11] is True

    def test_waiting_page_shows_the_last_client(self):
        # On no client: hero "Waiting for Home Assistant", chip "not connected", the last client and when it left.
        r = run(
            WAITING,
            """
            return [t.el('zg-title').textContent, t.el('zg-client-chip').textContent,
                    t.el('zg-client').allText()];
        """,
        )
        assert r[0] == "Waiting for Home Assistant"
        assert r[1] == "not connected"
        assert "192.168.1.10:40000" in r[2] and "Last client" in r[2]

    def test_waiting_with_no_last_client_says_none_yet(self):
        # On neither client nor last client: "none since start".
        r = run(
            body(client=None),
            "return t.el('zg-client').allText();",
        )
        assert "none since start" in r

    def test_no_radio_page_has_a_fault_hero_and_no_network_figures(self):
        # On no_radio: fault hero "No radio"; the network card shows dashes (no radio config).
        r = run(
            NO_RADIO,
            "return [t.el('zg-title').textContent, t.el('zg-hero').className, t.el('zg-net').allText()];",
        )
        assert r[0] == "No radio" and "hero-fault" in r[1]
        assert "0x" not in r[2]

    def test_clock_not_set_is_written_out(self):
        # On started 0 and a client since 0: "clock not set" instead of 1970.
        r = run(
            body(started=0, client={"address": "a", "since": 0}),
            "return [t.el('zg-since').textContent, t.el('zg-client').allText()];",
        )
        assert r[0] == "clock not set" and "clock not set" in r[1]

    def test_409_shows_the_placeholder(self):
        # On 409 (no Zigbee coordinator although the status named a channel): the section shows its placeholder.
        r = scenario(
            page("index", routes({"error": "not_zigbee"}, 409))
            + """
            await t.page.refreshStatus(); await t.page.refreshZigbee();
            return [t.el('zigbee-off').hidden(), t.el('zigbee-on').hidden()];
        """
        )
        assert r == [False, True]

    def test_body_after_409_brings_the_cards_back(self):
        # On a 409 followed by a running body: the section content shows again and the placeholder hides.
        r = run(
            RUNNING,
            """
            t.z409 = true; await t.page.refreshZigbee(); t.z409 = false; await t.page.refreshZigbee();
            return [t.el('zigbee-off').hidden(), t.el('zigbee-on').hidden()];
        """,
        )
        assert r == [True, False]

    def test_copy_writes_the_zha_path(self):
        # On Copy: ws://board.lan/zigbee goes to the clipboard and the button reads "Copied", then "Copy" again.
        r = run(
            RUNNING,
            """
            t.page.init(); await t.advance(10); const path = t.el('zg-path').textContent; t.el('zg-copy').click(); await t.advance(10);
            const shown = t.el('zg-copy').textContent; await t.advance(3000);
            return [path, t.clipboard.written, shown, t.el('zg-copy').textContent];
        """,
        )
        assert r == [
            "ws://board.lan/zigbee",
            ["ws://board.lan/zigbee"],
            "Copied",
            "Copy",
        ]

    def test_copy_failure_reads_copy_failed(self):
        # On a clipboard that refuses: "Copy failed".
        r = run(
            RUNNING,
            "t.page.init(); await t.advance(10); t.clipboard.fail = true; t.el('zg-copy').click(); await t.advance(10); return t.el('zg-copy').textContent;",
        )
        assert r == "Copy failed"

    def test_section_warns_that_there_is_no_login(self):
        # On the Dashboard: the no-login warning is in the Zigbee section's markup.
        html = (FRONTEND / "index.html").read_text()
        assert "No login" in html[html.index('id="sec-zigbee"') :]

    def test_section_polls_every_5_s_and_every_2_s_while_starting(self):
        # On running: a request about every 5 s; on starting: about every 2 s.
        r = scenario(
            page("index", routes(RUNNING))
            + f"""
            t.page.init(); await t.advance(10);
            const n = () => t.calls.filter((c) => c.url === '/device/zigbee').length;
            const a = n(); await t.advance(5100); const b = n(); await t.advance(5100); const c = n();
            t.zb = {json.dumps(body(state="starting"))}; await t.advance(5100);
            const d = n(); await t.advance(6100);
            return [a, b - a, c - b, n() - d];
        """
        )
        assert r[0] == 1 and r[1] == 1 and r[2] == 1 and r[3] >= 3

    def test_failed_request_says_so(self):
        # On a dropped connection: the note shows the error, the cards stay as they were.
        r = run(
            RUNNING,
            "t.zfail = true; await t.page.refreshZigbee(); return [t.el('zg-note').textContent, t.el('zg-radio').textContent];",
        )
        assert r[0] and r[1] == "USB radio usb-303a-1001"

    def test_starting_state_hero(self):
        # On starting: the hero reads "Starting".
        r = run(
            body(state="starting", client=None, radio=None),
            "return t.el('zg-title').textContent;",
        )
        assert r == "Starting"

    def test_no_zigbee_channel_shows_the_placeholder_and_asks_nothing(self):
        # On zigbee_channel null: placeholder shown with its link, content hidden, /device/zigbee never requested.
        r = scenario(
            page("index", routes(RUNNING))
            + """
            t.zc = null; await t.page.refreshStatus(); await t.page.refreshZigbee();
            return [t.el('zigbee-off').hidden(), t.el('zigbee-on').hidden(),
                    t.el('zigbee-off-link').getAttribute('href'),
                    t.calls.filter((c) => c.url === '/device/zigbee').length];
        """
        )
        assert r == [False, True, "/radios.html", 0]
        html = " ".join((FRONTEND / "index.html").read_text().split())
        assert "Zigbee is not active: no radio runs the Zigbee coordinator." in html
        assert ">Set a radio to Zigbee on the Radios page</a>" in html

    def test_older_firmware_without_the_field_is_treated_as_no_zigbee(self):
        # On a status without zigbee_channel (older firmware): the placeholder, and no /device/zigbee request.
        r = scenario(
            page(
                "index",
                "(u) => ({status: 200, body: u === '/device/zigbee' ? {} : u === '/device/channels' ? {channels: []}"
                " : (() => { const s = H.fixture(H.STATUS); delete s.zigbee_channel; return s; })()})",
            )
            + """
            await t.page.refreshStatus(); await t.page.refreshZigbee();
            return [t.el('zigbee-off').hidden(), t.calls.filter((c) => c.url === '/device/zigbee').length];
        """
        )
        assert r == [False, 0]

    def test_zigbee_stopping_clears_the_section_and_stops_asking(self):
        # On zigbee_channel going from set to null: placeholder, values cleared, no further /device/zigbee requests.
        r = run(
            RUNNING,
            """
            t.zc = null; await t.page.refreshStatus();
            const before = t.calls.filter((c) => c.url === '/device/zigbee').length;
            await t.page.refreshZigbee();
            return [t.el('zigbee-off').hidden(), t.el('zigbee-on').hidden(), t.el('zg-radio').textContent,
                    t.el('zg-net').allText(), t.calls.filter((c) => c.url === '/device/zigbee').length - before];
        """,
        )
        assert r == [False, True, "-", "", 0]


API = STATIC / "api.js"


def nav_scenario(status_body, path="/index.html", fetch_fails=False, slot=True):
    """Run api.js with a fake fetch answering /device/status; return the requests made and #nav-slot-zigbee after the
    answer (null when the page has no slot)."""
    answer = (
        "Promise.reject(new Error('down'))"
        if fetch_fails
        else f"Promise.resolve({{ok: true, text: () => Promise.resolve({json.dumps(json.dumps(status_body))})}})"
    )
    source = f"""
        const vm = require('vm'); const fs = require('fs');
        const nav = {{innerHTML: ''}}; const thread = {{hidden: false}};
        const slot = {json.dumps(slot)} ? {{innerHTML: '', hidden: true}} : null;
        const fetched = [];
        const ids = {{'nav': nav, 'nav-thread': thread, 'nav-slot-zigbee': slot}};
        const ctx = {{document: {{getElementById: (id) => ids[id] || null}},
                     location: {{pathname: {json.dumps(path)}}},
                     sessionStorage: {{getItem: () => null, setItem: () => {{}}}},
                     fetch: (u) => {{ fetched.push(u); return {answer}; }}}};
        vm.runInNewContext(fs.readFileSync({json.dumps(str(API))}, 'utf8'), ctx);
        setTimeout(() => process.stdout.write(JSON.stringify({{fetched, html: nav.innerHTML, slot}})), 50);
    """
    import subprocess

    out = subprocess.run(
        ["node", "-e", source], capture_output=True, text=True, check=True, timeout=20
    )
    return json.loads(out.stdout)


@needs_node
class TestZigbeeNav:
    """api.js: the Zigbee link in the nav slot"""

    def test_slot_gets_the_link_and_is_shown_when_a_channel_is_zigbee(self):
        # On /device/status with zigbee_channel "c6": one status request; the slot holds a Zigbee link and is shown.
        r = nav_scenario({"otbr_channel": None, "zigbee_channel": "c6"})
        assert r["fetched"] == ["/device/status"]
        assert r["slot"]["hidden"] is False
        assert r["slot"]["innerHTML"] == '<a href="/zigbee.html">Zigbee</a>'

    def test_link_is_active_on_the_zigbee_page(self):
        # On /zigbee.html: the link is marked active.
        r = nav_scenario({"zigbee_channel": "c6"}, "/zigbee.html")
        assert (
            r["slot"]["innerHTML"] == '<a href="/zigbee.html" class="active">Zigbee</a>'
        )

    @pytest.mark.parametrize("status", [{"zigbee_channel": None}, {}])
    def test_slot_stays_hidden_and_empty_without_a_zigbee_channel(self, status):
        # On a null or missing zigbee_channel: the slot stays hidden and empty.
        assert nav_scenario(status)["slot"] == {"innerHTML": "", "hidden": True}

    def test_slot_stays_hidden_when_the_status_cannot_be_read(self):
        # On a failing fetch: the slot stays hidden and empty, and nothing throws.
        assert nav_scenario(None, fetch_fails=True)["slot"] == {
            "innerHTML": "",
            "hidden": True,
        }

    def test_no_slot_in_the_page_is_not_an_error(self):
        # On a nav without the slot element: nothing throws.
        assert nav_scenario({"zigbee_channel": "c6"}, slot=False)["slot"] is None

    def test_static_menu_has_the_empty_hidden_slot_and_no_link(self):
        # On the static nav: the slot is an empty hidden <li>; the Zigbee link is not in the markup.
        html = nav_scenario({"zigbee_channel": None})["html"]
        assert '<li id="nav-slot-zigbee" hidden></li>' in html
        assert "zigbee.html" not in html
