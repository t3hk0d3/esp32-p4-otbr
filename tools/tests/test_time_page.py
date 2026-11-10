"""Time section of the Ethernet page (BDD: time.js page, spec 2026-11-10-time-sync-design.md 4.3). Runs static/time.js
under node."""

import json

from device_js import logic, needs_node, page, scenario

# GET /device/time: synced from the two servers the management lease names.
GET = {
    "enabled": True,
    "mode": "auto",
    "server": "",
    "state": "synced",
    "now": 1800000000,
    "synced_age_s": 240,
    "source": "dhcp",
    "servers": [
        {"address": "10.0.10.1", "answered": True},
        {"address": "10.0.10.2", "answered": False},
    ],
    "dhcp_servers": ["10.0.10.1", "10.0.10.2"],
    "dhcp_iface": "mgmt",
}


def time_(expression):
    return logic("time", expression)


def get(**patch):
    return GET | patch


def call(fn, body):
    return time_(f"L.{fn}({json.dumps(body)})")


@needs_node
class TestTimeLogic:
    """chip, lastSynced, sourceLine, dhcpHint, ifaceRow, validate, toBody"""

    def test_chip_for_each_state(self):
        # On each spec 4.3 condition: that chip text and kind.
        cases = [
            (get(), ["Synced", "ok"]),
            (get(enabled=False, state="off", now=None), ["Off", ""]),
            (
                get(state="waiting", now=None, synced_age_s=None),
                ["Clock not set", "warn"],
            ),
            (get(state="unreachable"), ["Server not answering", "warn"]),
            (get(state="waiting", synced_age_s=None), ["Waiting for server", ""]),
        ]
        for body, want in cases:
            got = call("chip", body)
            assert [got["text"], got["kind"]] == want, body

    def test_last_synced_with_and_without_an_answer(self):
        # On 240 s: "4 minutes ago"; on null with the clock valid: "Before the last restart"; with it not: "Never since power-up".
        assert call("lastSynced", get()) == "4 minutes ago"
        assert call("lastSynced", get(synced_age_s=30)) == "Just now"
        assert call("lastSynced", get(synced_age_s=3600)) == "1 hour ago"
        assert call("lastSynced", get(synced_age_s=2 * 86400)) == "2 days ago"
        assert call("lastSynced", get(synced_age_s=None)) == "Before the last restart"
        assert (
            call("lastSynced", get(synced_age_s=None, now=None))
            == "Never since power-up"
        )

    def test_source_line_names_where_the_servers_came_from(self):
        # On dhcp with an interface, without, default and custom: the mockup's lines.
        assert call("sourceLine", get()) == "from your router (DHCP on Management)"
        assert (
            call("sourceLine", get(dhcp_iface="thread_bb"))
            == "from your router (DHCP on Thread backbone)"
        )
        assert call("sourceLine", get(dhcp_iface="single")) == "from your router (DHCP)"
        assert call("sourceLine", get(dhcp_iface=None)) == "from your router (DHCP)"
        assert (
            call("sourceLine", get(source="default"))
            == "default; your router names none"
        )
        assert call("sourceLine", get(source="custom")) == "set here"
        assert call("sourceLine", get(source=None)) == ""

    def test_dhcp_hint_names_what_the_router_offers(self):
        # On two DHCP servers: both named; on none: empty.
        hint = call("dhcpHint", get())
        assert "10.0.10.1" in hint and "10.0.10.2" in hint and "Automatic" in hint
        assert call("dhcpHint", get(dhcp_servers=[])) == ""

    def test_interface_row_on_the_lease_interface_only(self):
        # On mgmt: the row for mgmt with both servers; on no interface or no servers: null.
        assert call("ifaceRow", get()) == {
            "iface": "mgmt",
            "text": "10.0.10.1, 10.0.10.2",
        }
        assert call("ifaceRow", get(dhcp_iface=None)) is None
        assert call("ifaceRow", get(dhcp_servers=[])) is None

    def test_validate_requires_a_custom_server(self):
        # On Custom with an empty or bad server: an error on server; with a hostname or address: none.
        form = {"enabled": True, "mode": "custom", "server": ""}
        assert set(call("validate", form)) == {"server"}
        for bad in ["a b", "-x.lan", "x" * 64]:
            assert set(call("validate", form | {"server": bad})) == {"server"}, bad
        for good in ["ntp.home.lan", "192.168.1.1", "2001:db8::1"]:
            assert call("validate", form | {"server": good}) == {}, good
        assert (
            call("validate", {"enabled": True, "mode": "auto", "server": "a b"}) == {}
        )

    def test_body_drops_an_invalid_server_in_automatic(self):
        # On Automatic with a bad typed server: server ""; with a valid one: kept (the board keeps it for Custom).
        assert call("toBody", {"enabled": True, "mode": "auto", "server": "a b"}) == {
            "enabled": True,
            "mode": "auto",
            "server": "",
        }
        assert call(
            "toBody", {"enabled": False, "mode": "auto", "server": " ntp.lan "}
        ) == {
            "enabled": False,
            "mode": "auto",
            "server": "ntp.lan",
        }


def routes(get_js="H.fixture(H.TIME)", put="{status: 200, body: H.fixture(H.TIME)}"):
    return (
        f"(u, o) => u === '/device/time' ? (o.method === 'PUT' ? ({put}) : ({{status: 200, body: {get_js}}}))"
        " : ({status: 200, body: H.NETWORK})"
    )


START = "t.page.init(); await t.advance(100);"


@needs_node
class TestTimePage:
    """time.js run against the Ethernet page's fake DOM and fetch"""

    def test_load_shows_the_status_and_the_form(self):
        # On load: chip Synced, last synced, the servers, the source line, Automatic pressed, the server field hidden.
        r = scenario(
            page("time", routes())
            + START
            + """
            return [t.el('tm-chip').textContent, t.el('tm-chip').classList.contains('chip-ok'),
                    t.el('tm-last').textContent, t.el('tm-servers').allText().includes('10.0.10.2'),
                    t.el('tm-source').textContent, t.el('tm-auto').getAttribute('aria-pressed'),
                    t.el('tm-server-field').classList.contains('hidden'), t.el('tm-enabled').checked,
                    t.el('tm-save').disabled];
        """
        )
        assert r == [
            "Synced",
            True,
            "4 minutes ago",
            True,
            "from your router (DHCP on Management)",
            "true",
            True,
            True,
            True,
        ]

    def test_clock_not_set_shows_not_set(self):
        # On now null: board time "Not set" and the Clock not set chip.
        r = scenario(
            page(
                "time",
                routes(
                    "H.fixture(H.TIME, {state: 'waiting', now: null, synced_age_s: null})"
                ),
            )
            + START
            + "return [t.el('tm-now').textContent, t.el('tm-chip').textContent];"
        )
        assert r == ["Not set", "Clock not set"]

    def test_custom_shows_the_field_and_the_dhcp_hint(self):
        # On a click on Custom server: the field shows, the hint names what the router offers.
        r = scenario(
            page("time", routes())
            + START
            + """
            t.el('tm-custom').fire('click'); await t.advance(10);
            return [t.el('tm-server-field').classList.contains('hidden'),
                    t.el('tm-dhcp-hint').textContent.includes('10.0.10.1')];
        """
        )
        assert r == [False, True]

    def test_custom_with_an_empty_field_shows_an_error_and_sends_nothing(self):
        # On Custom with no server: an error under the field, Save disabled, no PUT.
        r = scenario(
            page("time", routes())
            + START
            + """
            t.el('tm-custom').fire('click'); await t.advance(10);
            t.el('tm-save').fire('click'); await t.advance(100);
            return [t.el('tm-server-err').textContent !== '', t.el('tm-save').disabled,
                    t.calls.filter((c) => c.method === 'PUT').length];
        """
        )
        assert r == [True, True, 0]

    def test_switching_back_and_forth_keeps_the_typed_server(self):
        # On Custom, typing, Automatic, Custom again: the typed server is still there.
        r = scenario(
            page("time", routes())
            + START
            + """
            t.el('tm-custom').fire('click');
            t.el('tm-server').value = 'ntp.home.lan'; t.el('tm-server').fire('input');
            t.el('tm-auto').fire('click');
            t.el('tm-custom').fire('click'); await t.advance(10);
            return t.el('tm-server').value;
        """
        )
        assert r == "ntp.home.lan"

    def test_save_sends_the_three_fields(self):
        # On a valid custom server and Save: one PUT with enabled, mode and server, then "Saved."
        r = scenario(
            page("time", routes())
            + START
            + """
            t.el('tm-custom').fire('click');
            t.el('tm-server').value = 'ntp.home.lan'; t.el('tm-server').fire('input');
            t.el('tm-save').fire('click'); await t.advance(100);
            const put = t.calls.filter((c) => c.method === 'PUT');
            return [put.length, JSON.parse(put[0].body), t.el('tm-msg').textContent];
        """
        )
        assert r == [
            1,
            {"enabled": True, "mode": "custom", "server": "ntp.home.lan"},
            "Saved.",
        ]

    def test_a_400_with_a_field_shows_under_that_field(self):
        # On 400 invalid_config field server: the board's detail under the server field.
        put = "{status: 400, body: {error: 'invalid_config', field: 'server', detail: 'server: not a hostname'}}"
        r = scenario(
            page("time", routes(put=put))
            + START
            + """
            t.el('tm-custom').fire('click');
            t.el('tm-server').value = 'ntp.home.lan'; t.el('tm-server').fire('input');
            t.el('tm-save').fire('click'); await t.advance(100);
            return t.el('tm-server-err').textContent;
        """
        )
        assert r == "server: not a hostname"

    def test_interface_row_on_the_lease_interface_only(self):
        # On dhcp_iface mgmt: the Management card's row shows the servers; the others stay hidden.
        r = scenario(
            page("time", routes())
            + START
            + """
            return [t.el('if-mgmt-ntprow').classList.contains('hidden'), t.el('if-mgmt-ntp').textContent,
                    t.el('if-single-ntprow').classList.contains('hidden'),
                    t.el('if-thread_bb-ntprow').classList.contains('hidden')];
        """
        )
        assert r == [False, "10.0.10.1, 10.0.10.2", True, True]
