"""Zigbee page, nav link and Dashboard card (BDD: ZigbeeLogic, ZigbeePage, api.js nav, StatusPage).

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


def routes(b, status=200):
    """Fake routes: /device/zigbee answers `b` (a JS expression is not needed: the body is data)."""
    return f"(u) => ({{status: {status}, body: {json.dumps(b)}}})"


def run(b, tail, status=200):
    return scenario(page("zigbee", routes(b, status)) + "await t.page.refresh();" + tail)


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
        h = z(f"L.heroFor({json.dumps(body(state=state, reason='waiting for the radio'))})")
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
class TestZigbeePage:
    """zigbee.js run against a fake DOM and fetch"""

    def test_running_page_is_filled_from_the_body(self):
        # On a running body: hero, radio, network, traffic, memory and firmware cards show its figures.
        r = run(
            RUNNING,
            """
            return [t.el('zg-title').textContent, t.el('zg-hero').className, t.el('zg-radio').textContent,
                    t.el('zg-resets').textContent, t.el('zg-client').allText(), t.el('zg-net').allText(),
                    t.el('zg-traffic').allText(), t.el('zg-heap').textContent, t.el('zg-heap-use').textContent,
                    t.el('zg-fw').allText(), t.el('zg-cards').hidden(), t.el('zg-none').hidden()];
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

    def test_409_shows_that_no_radio_is_zigbee(self):
        # On 409: the info hero "No radio is a Zigbee coordinator" and no cards.
        r = scenario(
            page("zigbee", routes({"error": "not_zigbee"}, 409))
            + """
            await t.page.refresh();
            return [t.el('zg-none').hidden(), t.el('zg-cards').hidden(), t.el('zg-hero').hidden()];
        """
        )
        assert r == [False, True, True]
        assert "No radio is a Zigbee coordinator" in (FRONTEND / "zigbee.html").read_text()

    def test_body_after_409_brings_the_cards_back(self):
        # On a 409 followed by a running body: the cards show again and the 409 hero hides.
        r = scenario(
            page(
                "zigbee",
                f"(u) => (t.mode409 ? {{status: 409, body: {{}}}} : {{status: 200, body: {json.dumps(RUNNING)}}})",
            )
            + """
            t.mode409 = true; await t.page.refresh(); t.mode409 = false; await t.page.refresh();
            return [t.el('zg-none').hidden(), t.el('zg-cards').hidden()];
        """
        )
        assert r == [True, False]

    def test_copy_writes_the_zha_path(self):
        # On Copy: ws://board.lan/zigbee goes to the clipboard and the button reads "Copied", then "Copy" again.
        r = run(
            RUNNING,
            """
            t.page.init(); const path = t.el('zg-path').textContent; t.el('zg-copy').click(); await t.advance(10);
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
            "t.page.init(); t.clipboard.fail = true; t.el('zg-copy').click(); await t.advance(10); return t.el('zg-copy').textContent;",
        )
        assert r == "Copy failed"

    def test_page_warns_that_there_is_no_login(self):
        # On the page: the no-login warning is in the markup.
        assert "No login" in (FRONTEND / "zigbee.html").read_text()

    def test_page_polls_every_5_s_and_every_2_s_while_starting(self):
        # On running: a request about every 5 s; on starting: about every 2 s.
        r = scenario(
            page(
                "zigbee",
                f"(u) => (u === '/device/zigbee' ? {{status: 200, body: t.b}} : {{status: 200, body: {{}}}})",
            )
            + f"""
            t.b = {json.dumps(RUNNING)}; t.page.init(); await t.advance(0);
            const n = () => t.calls.filter((c) => c.url === '/device/zigbee').length;
            const a = n(); await t.advance(5100); const b = n(); await t.advance(5100); const c = n();
            t.b = {json.dumps(body(state='starting'))}; await t.advance(5100);
            const d = n(); await t.advance(6100);
            return [a, b - a, c - b, n() - d];
        """
        )
        assert r[0] == 1 and r[1] == 1 and r[2] == 1 and r[3] >= 3

    def test_failed_request_says_so(self):
        # On a dropped connection: the note shows the error, the cards stay as they were.
        r = scenario(
            page("zigbee", "() => { throw new Error('x'); }")
            + "await t.page.refresh(); return t.el('zg-note').textContent;"
        )
        assert r

    def test_starting_state_hero(self):
        # On starting: the hero reads "Starting".
        r = run(
            body(state="starting", client=None, radio=None),
            "return t.el('zg-title').textContent;",
        )
        assert r == "Starting"


API = STATIC / "api.js"


def nav_scenario(status_body, path="/index.html", fetch_fails=False):
    """Run api.js with a nav stand-in and a fake fetch; return what was inserted after the Dashboard entry."""
    answer = (
        "Promise.reject(new Error('down'))"
        if fetch_fails
        else f"Promise.resolve({{ok: true, json: () => Promise.resolve({json.dumps(status_body)})}})"
    )
    source = f"""
        const vm = require('vm'); const fs = require('fs');
        const dash = {{id: 'dash', nextSibling: {{id: 'next'}}}};
        const ul = {{firstChild: dash, inserted: [], insertBefore(li, ref) {{ this.inserted.push({{li, ref: ref && ref.id}}); return li; }}}};
        const nav = {{innerHTML: '', querySelector: (s) => s === '.nav-links' ? ul : null}};
        const fetched = [];
        const el = () => ({{children: [], attrs: {{}}, textContent: '', className: '',
                          appendChild(c) {{ this.children.push(c); return c; }},
                          setAttribute(k, v) {{ this.attrs[k] = v; }}}});
        const ctx = {{document: {{getElementById: (id) => id === 'nav' ? nav : null, createElement: el}},
                     location: {{pathname: {json.dumps(path)}}},
                     fetch: (u) => {{ fetched.push(u); return {answer}; }}}};
        vm.runInNewContext(fs.readFileSync({json.dumps(str(API))}, 'utf8'), ctx);
        setTimeout(() => {{
          const shape = (e) => ({{cls: e.className, a: e.children.map((c) => ({{text: c.textContent, href: c.attrs.href, cls: c.className}}))}});
          process.stdout.write(JSON.stringify({{fetched, html: nav.innerHTML, inserted: ul.inserted.map((i) => ({{ref: i.ref, ...shape(i.li)}}))}}));
        }}, 20);
    """
    import subprocess

    out = subprocess.run(
        ["node", "-e", source], capture_output=True, text=True, check=True, timeout=20
    )
    return json.loads(out.stdout)


@needs_node
class TestZigbeeNav:
    """api.js: the Zigbee entry after the Dashboard"""

    def test_entry_is_added_after_the_dashboard_when_a_channel_is_zigbee(self):
        # On /device/status with zigbee_channel "c6": a Zigbee link to /zigbee.html goes in right after Dashboard.
        r = nav_scenario({"zigbee_channel": "c6"})
        assert r["fetched"] == ["/device/status"]
        assert len(r["inserted"]) == 1
        assert r["inserted"][0]["ref"] == "next"
        assert r["inserted"][0]["a"][0]["text"] == "Zigbee"
        assert r["inserted"][0]["a"][0]["href"] == "/zigbee.html"

    def test_entry_is_active_on_the_zigbee_page(self):
        # On /zigbee.html: the entry is marked active.
        r = nav_scenario({"zigbee_channel": "c6"}, "/zigbee.html")
        assert r["inserted"][0]["a"][0]["cls"] == "active"

    @pytest.mark.parametrize("status", [{"zigbee_channel": None}, {}])
    def test_no_entry_without_a_zigbee_channel(self, status):
        # On a null or missing zigbee_channel: nothing is added.
        assert nav_scenario(status)["inserted"] == []

    def test_no_entry_when_the_status_cannot_be_read(self):
        # On a failing fetch: nothing is added and nothing throws.
        assert nav_scenario(None, fetch_fails=True)["inserted"] == []

    def test_static_menu_is_unchanged(self):
        # On the static nav: it still lists the Dashboard first and has no Zigbee link of its own.
        html = nav_scenario({"zigbee_channel": None})["html"]
        assert html.index("Dashboard") < html.index("Management")
        assert "zigbee" not in html.lower()


STATUS_WITH_ZIGBEE = "Object.assign(H.fixture(H.STATUS), {zigbee_channel: 'c6'})"


def dashboard_routes(zbody):
    return (
        "(u) => ({status: 200, body: u === '/device/diag' ? H.DIAG : u === '/device/network' ? H.NETWORK"
        f" : u === '/device/zigbee' ? {json.dumps(zbody)} : u === '/get_properties' ? H.PROPS : u === '/ipaddr' ? H.IPADDR"
        f" : {STATUS_WITH_ZIGBEE}}})"
    )


@needs_node
class TestDashboardZigbeeCard:
    """status.js: the Zigbee summary card"""

    def test_card_shows_radio_state_channel_and_client(self):
        # On zigbee_channel "c6" and a running coordinator with a client: the card is shown with its figures.
        r = scenario(
            page("index", dashboard_routes(RUNNING))
            + """
            await t.page.refreshStatus(); await t.page.refreshZigbee();
            return [t.el('t-zg-card').hidden(), t.el('t-zg-radio').textContent, t.el('t-zg-state').textContent,
                    t.el('t-zg-channel').textContent, t.el('t-zg-client').textContent];
        """
        )
        assert r == [
            False,
            "on-board C6",
            "Running, ZHA connected",
            "15",
            "192.168.1.20:51234",
        ]

    def test_card_says_waiting_without_a_client(self):
        # On a running coordinator without a client: state "Waiting for Home Assistant", client "-".
        r = scenario(
            page("index", dashboard_routes(WAITING))
            + """
            await t.page.refreshStatus(); await t.page.refreshZigbee();
            return [t.el('t-zg-state').textContent, t.el('t-zg-client').textContent];
        """
        )
        assert r == ["Waiting for Home Assistant", "-"]

    def test_card_is_hidden_and_not_polled_without_a_zigbee_channel(self):
        # On zigbee_channel null: the card stays hidden and /device/zigbee is never asked.
        r = scenario(
            page("index", "(u) => ({status: 200, body: H.STATUS})")
            + """
            await t.page.refreshStatus(); await t.page.refreshZigbee();
            return [t.el('t-zg-card').hidden(), t.calls.filter((c) => c.url === '/device/zigbee').length];
        """
        )
        assert r == [True, 0]

    def test_card_reports_an_unreadable_coordinator(self):
        # On a refused /device/zigbee: the state reads "unknown" and the channel and client are dashes.
        r = scenario(
            page(
                "index",
                "(u) => u === '/device/zigbee' ? ({status: 500, body: {}}) : ({status: 200, body: "
                f"{STATUS_WITH_ZIGBEE}}})",
            )
            + """
            await t.page.refreshStatus(); await t.page.refreshZigbee();
            return [t.el('t-zg-state').textContent, t.el('t-zg-channel').textContent, t.el('t-zg-client').textContent];
        """
        )
        assert r == ["unknown", "-", "-"]
