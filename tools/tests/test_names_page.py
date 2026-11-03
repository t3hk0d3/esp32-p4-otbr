"""Names section of the Ethernet page (BDD: NamesLogic, NamesPage). Runs static/names.js under node."""

import json

from device_js import logic, needs_node, page, scenario


def names(expression):
    return logic("names", expression)


GET = {
    "hostname": "kitchen",
    "hostname_default": "esp-otbr-a1b2c3",
    "hostname_live": "kitchen",
    "ha_name_follows_hostname": True,
    "ha_name": "",
    "ha_name_effective": "kitchen",
    "http_advert": False,
    "http_advert_available": True,
    "dhcp_name": "",
    "dhcp_name_default": "esp-otbr",
    "mode": "single",
}

FORM = {
    "hostname": "kitchen",
    "follows": True,
    "ha_name": "",
    "http_advert": False,
    "dhcp_name": "",
}


def form(**patch):
    return FORM | patch


def errors_of(f):
    return names(f"L.validate({json.dumps(f)}).errors")


@needs_node
class TestNamesLogic:
    """fromApi, normaliseHostname, utf8Bytes, validate, dirty, toBody, redirectTarget, conflict"""

    def test_form_from_the_api(self):
        # On a GET body: the five form fields, the follows checkbox from ha_name_follows_hostname.
        got = names(
            f"L.fromApi({json.dumps(GET | {'ha_name': 'Kitchen OTBR', 'http_advert': True})})"
        )
        assert got == form(ha_name="Kitchen OTBR", http_advert=True)

    def test_trailing_local_is_stripped_from_a_hostname(self):
        # On "kitchen.local", "Kitchen.LOCAL", " kitchen.local. ": the name without .local, case kept.
        assert names(
            "['kitchen.local', 'Kitchen.LOCAL', ' kitchen.local. ', 'local'].map(L.normaliseHostname)"
        ) == ["kitchen", "Kitchen", "kitchen", "local"]

    def test_utf8_bytes_are_counted_not_characters(self):
        # On a Cyrillic letter and an emoji: 2 and 4 bytes.
        assert names("[L.utf8Bytes('Ж'), L.utf8Bytes('😀'), L.utf8Bytes('ab')]") == [
            2,
            4,
            2,
        ]

    def test_a_valid_form_has_no_errors(self):
        # On the sample form, an empty hostname, a pasted .local and upper case: no errors.
        for f in [
            form(),
            form(hostname=""),
            form(hostname="Kitchen.local"),
            form(dhcp_name="Router-1"),
        ]:
            assert errors_of(f) == {}, f

    def test_bad_hostnames_are_refused_with_the_field(self):
        # On a leading/trailing hyphen, a dot, an underscore, a space or 64 characters: an error on hostname.
        for bad in ["-a", "a-", "a.b", "a_b", "a b", "a" * 64]:
            assert set(errors_of(form(hostname=bad))) == {"hostname"}, bad

    def test_bad_dhcp_name_is_refused_with_the_field(self):
        # On "bad.name": an error on dhcp_name.
        assert set(errors_of(form(dhcp_name="bad.name"))) == {"dhcp_name"}

    def test_dhcp_name_limit_is_32_characters(self):
        # On 32 characters: valid; on 33: refused with the limit in the message (esp_netif refuses longer names).
        assert errors_of(form(dhcp_name="d" * 32)) == {}
        err = errors_of(form(dhcp_name="d" * 33))
        assert set(err) == {"dhcp_name"}
        assert "32 characters" in err["dhcp_name"]

    def test_ha_name_is_checked_only_when_it_does_not_follow(self):
        # On an empty or space-padded HA name: an error only with follows off.
        assert errors_of(form(follows=True, ha_name=" x")) == {}
        assert set(errors_of(form(follows=False, ha_name=""))) == {"ha_name"}
        assert set(errors_of(form(follows=False, ha_name=" Kitchen"))) == {"ha_name"}
        assert set(errors_of(form(follows=False, ha_name="Kit\tchen"))) == {"ha_name"}

    def test_ha_name_limit_is_63_utf8_bytes(self):
        # On 31 Cyrillic letters plus one ASCII (63 bytes): valid; on 32 Cyrillic letters (64 bytes): refused.
        assert errors_of(form(follows=False, ha_name="Ж" * 31 + "x")) == {}
        err = errors_of(form(follows=False, ha_name="Ж" * 32))
        assert set(err) == {"ha_name"}
        assert "63 bytes" in err["ha_name"]

    def test_dirty_compares_every_field(self):
        # On an unchanged form: false; on any changed field: true.
        assert names(f"L.dirty({json.dumps(FORM)}, {json.dumps(FORM)})") is False
        for k, v in [
            ("hostname", "x"),
            ("follows", False),
            ("ha_name", "y"),
            ("http_advert", True),
            ("dhcp_name", "z"),
        ]:
            assert (
                names(f"L.dirty({json.dumps(FORM)}, {json.dumps(form(**{k: v}))})")
                is True
            ), k

    def test_request_body_has_the_five_fields_without_local(self):
        # On a form with a pasted ".local" hostname: the PUT body with the five API fields.
        got = names(
            f"L.toBody({json.dumps(form(hostname=' Kitchen.local ', follows=False, ha_name='Kitchen OTBR', http_advert=True, dhcp_name=' router '))})"
        )
        assert got == {
            "hostname": "Kitchen",
            "ha_name_follows_hostname": False,
            "ha_name": "Kitchen OTBR",
            "http_advert": True,
            "dhcp_name": "router",
        }

    def test_redirect_when_the_page_was_opened_by_the_old_name(self):
        # On a page at kitchen.local:8080/ethernet.html?x and a new live name: the same path on the new name.
        loc = {
            "protocol": "http:",
            "hostname": "Kitchen.local",
            "port": "8080",
            "pathname": "/ethernet.html",
            "search": "?x",
        }
        after = GET | {"hostname": "garage", "hostname_live": "garage"}
        assert (
            names(
                f"L.redirectTarget({json.dumps(loc)}, {json.dumps(GET)}, {json.dumps(after)})"
            )
            == "http://garage.local:8080/ethernet.html?x"
        )

    def test_redirect_goes_to_the_live_name_after_a_conflict(self):
        # On a new name the board had to rename (garage -> garage-2): the live name, not the typed one.
        loc = {
            "protocol": "http:",
            "hostname": "kitchen.local",
            "port": "",
            "pathname": "/ethernet.html",
            "search": "",
        }
        after = GET | {"hostname": "garage", "hostname_live": "garage-2"}
        assert (
            names(
                f"L.redirectTarget({json.dumps(loc)}, {json.dumps(GET)}, {json.dumps(after)})"
            )
            == "http://garage-2.local/ethernet.html"
        )

    def test_no_redirect_by_ip_or_unchanged_name(self):
        # On a page opened by IP, or a live name that did not change: null.
        ip = {
            "protocol": "http:",
            "hostname": "192.168.1.117",
            "port": "",
            "pathname": "/",
            "search": "",
        }
        local = ip | {"hostname": "kitchen.local"}
        after = GET | {"hostname": "garage", "hostname_live": "garage"}
        assert (
            names(
                f"L.redirectTarget({json.dumps(ip)}, {json.dumps(GET)}, {json.dumps(after)})"
            )
            is None
        )
        assert (
            names(
                f"L.redirectTarget({json.dumps(local)}, {json.dumps(GET)}, {json.dumps(GET)})"
            )
            is None
        )

    def test_conflict_when_the_live_name_differs(self):
        # On live = effective hostname: false; on a renamed host, or the default renamed: true.
        assert names(f"L.conflict({json.dumps(GET)})") is False
        assert (
            names(f"L.conflict({json.dumps(GET | {'hostname_live': 'kitchen-2'})})")
            is True
        )
        default = GET | {"hostname": "", "hostname_live": "esp-otbr-a1b2c3"}
        assert names(f"L.conflict({json.dumps(default)})") is False


def routes(
    get_js="H.fixture(H.NAMES)",
    put="{status: 200, body: H.fixture(H.NAMES, {applied: true})}",
):
    return (
        f"(u, o) => u === '/device/names' ? (o.method === 'PUT' ? ({put}) : ({{status: 200, body: {get_js}}}))"
        " : ({status: 200, body: H.NETWORK})"
    )


START = "t.page.init(); await t.advance(100);"


@needs_node
class TestNamesPage:
    """names.js run against the Ethernet page's fake DOM and fetch"""

    def test_load_fills_the_form_with_save_and_discard_disabled(self):
        # On load: the fields from GET, follows ticked, HA input disabled showing the hostname, buttons disabled.
        r = scenario(
            page("names", routes())
            + START
            + """
            return [t.el('nm-hostname').value, t.el('nm-follows').checked, t.el('nm-ha').disabled, t.el('nm-ha').value,
                    t.el('nm-http').checked, t.el('nm-dhcp').value, t.el('nm-save').disabled, t.el('nm-discard').disabled,
                    t.el('nm-hostname-hint').textContent.includes('esp-otbr-a1b2c3'),
                    t.el('nm-dhcp-hint').textContent.includes('esp-otbr')];
        """
        )
        assert r == [
            "kitchen",
            True,
            True,
            "kitchen",
            False,
            "",
            True,
            True,
            True,
            True,
        ]

    def test_an_edit_enables_save_and_discard(self):
        # On typing a new hostname: Save and Discard enabled, the following HA input shows the new name.
        r = scenario(
            page("names", routes())
            + START
            + """
            t.el('nm-hostname').type('garage');
            return [t.el('nm-save').disabled, t.el('nm-discard').disabled, t.el('nm-ha').value];
        """
        )
        assert r == [False, False, "garage"]

    def test_an_invalid_field_shows_its_error_and_blocks_save(self):
        # On a hostname "a.b": the error under the hostname, Save disabled.
        r = scenario(
            page("names", routes())
            + START
            + """
            t.el('nm-hostname').type('a.b');
            return [t.el('nm-hostname-err').allText(), t.el('nm-save').disabled];
        """
        )
        assert "letters, digits and hyphens" in r[0]
        assert r[1] is True

    def test_unticking_follows_enables_the_ha_input(self):
        # On unticking "Same as hostname": the HA input enabled, prefilled with the effective name.
        r = scenario(
            page("names", routes())
            + START
            + """
            t.el('nm-follows').tick(false);
            return [t.el('nm-ha').disabled, t.el('nm-ha').value, t.el('nm-save').disabled];
        """
        )
        assert r == [False, "kitchen", False]

    def test_discard_goes_back_to_the_saved_names(self):
        # On Discard after edits: the loaded values, buttons disabled again.
        r = scenario(
            page("names", routes())
            + START
            + """
            t.el('nm-hostname').type('garage'); t.el('nm-http').tick(true); t.el('nm-discard').click();
            return [t.el('nm-hostname').value, t.el('nm-http').checked, t.el('nm-save').disabled];
        """
        )
        assert r == ["kitchen", False, True]

    def test_split_mode_greys_out_the_advert_and_explains_the_hostname(self):
        # On mode split: the advert checkbox disabled with its hint, the hostname split note shown.
        r = scenario(
            page(
                "names",
                routes(
                    "H.fixture(H.NAMES, {mode: 'split', http_advert_available: false})"
                ),
            )
            + START
            + """
            return [t.el('nm-http').disabled, t.el('nm-http-split').hidden(), t.el('nm-hostname-split').hidden()];
        """
        )
        assert r == [True, False, False]

    def test_single_mode_hides_the_split_notes(self):
        # On mode single: the advert checkbox enabled, both split notes hidden.
        r = scenario(
            page("names", routes())
            + START
            + "return [t.el('nm-http').disabled, t.el('nm-http-split').hidden(), t.el('nm-hostname-split').hidden()];"
        )
        assert r == [False, True, True]

    def test_no_conflict_shows_the_announced_chip(self):
        # On hostname_live = the hostname: the Announced chip, no Renamed chip, no banner.
        r = scenario(
            page("names", routes())
            + START
            + "return [t.el('nm-announced').hidden(), t.el('nm-renamed').hidden(), t.el('nm-conflict').hidden()];"
        )
        assert r == [False, True, True]

    def test_conflict_shows_the_chip_and_banner(self):
        # On hostname_live kitchen-2: the Renamed chip and a banner naming both names.
        r = scenario(
            page("names", routes("H.fixture(H.NAMES, {hostname_live: 'kitchen-2'})"))
            + START
            + "return [t.el('nm-renamed').hidden(), t.el('nm-conflict').hidden(), t.el('nm-conflict').allText(), t.el('nm-announced').hidden()];"
        )
        assert r[0:2] == [False, False]
        assert r[3] is True
        assert "kitchen.local" in r[2]
        assert "kitchen-2.local" in r[2]

    def test_save_puts_the_body_then_reloads_the_section(self):
        # On Save (page opened by IP): PUT /device/names with the five fields, then a GET about 2 s later, no navigation.
        r = scenario(
            page("names", routes())
            + START
            + """
            t.el('nm-hostname').type('Garage.local'); t.el('nm-dhcp').type('router');
            const before = t.calls.length;
            t.el('nm-save').click();
            await t.advance(2500);
            const put = t.calls.find((c) => c.method === 'PUT');
            const after = t.calls.slice(before).map((c) => c.method + ' ' + c.url);
            return [put.url, JSON.parse(put.body), after, t.navigations()];
        """
        )
        assert r[0] == "/device/names"
        assert r[1] == {
            "hostname": "Garage",
            "ha_name_follows_hostname": True,
            "ha_name": "",
            "http_advert": False,
            "dhcp_name": "router",
        }
        assert r[2][0] == "PUT /device/names"
        assert "GET /device/names" in r[2][1:]
        assert r[3] == []

    def test_save_moves_to_the_new_name_when_opened_by_the_old_one(self):
        # On Save from kitchen.local with the board now answering as garage-2: navigation to garage-2.local.
        r = scenario(
            page(
                "names",
                """(u, o) => u === '/device/names' ? (o.method === 'PUT'
                    ? {status: 200, body: H.fixture(H.NAMES, {hostname: 'garage', hostname_live: 'garage', applied: true})}
                    : {status: 200, body: H.fixture(H.NAMES, t.saved ? {hostname: 'garage', hostname_live: 'garage-2'} : {})})
                  : {status: 200, body: H.NETWORK}""",
            ).replace("const t =", "var t; t =")
            + """
            t.env.location = {protocol: 'http:', hostname: 'kitchen.local', port: '', pathname: '/ethernet.html', search: ''};
            t.page.init(); await t.advance(100);
            t.el('nm-hostname').type('garage');
            t.saved = true;
            t.el('nm-save').click();
            await t.advance(2500);
            return t.navigations();
        """
        )
        assert r == ["http://garage-2.local/ethernet.html"]

    def test_after_a_conflict_the_same_name_can_be_saved_again(self):
        # On hostname_live kitchen-2 and an unchanged form: Save enabled; Save sends the same hostname to retry it.
        r = scenario(
            page("names", routes("H.fixture(H.NAMES, {hostname_live: 'kitchen-2'})"))
            + START
            + """
            const enabled = !t.el('nm-save').disabled;
            t.el('nm-save').click(); await t.advance(2500);
            const put = t.calls.find((c) => c.method === 'PUT');
            return [enabled, put && JSON.parse(put.body).hostname, t.el('nm-discard').disabled];
        """
        )
        assert r == [True, "kitchen", True]

    def test_save_moves_to_the_new_name_even_when_the_fresh_read_fails(self):
        # On Save from kitchen.local, the PUT answered with live garage, then the GET failing: navigation to garage.local.
        r = scenario(
            page(
                "names",
                """(u, o) => {
                    if (u !== '/device/names') return {status: 200, body: H.NETWORK};
                    if (o.method === 'PUT') return {status: 200, body: H.fixture(H.NAMES, {hostname: 'garage', hostname_live: 'garage', applied: true})};
                    if (t.saved) throw new Error('connection dropped');
                    return {status: 200, body: H.fixture(H.NAMES)};
                }""",
            ).replace("const t =", "var t; t =")
            + """
            t.env.location = {protocol: 'http:', hostname: 'kitchen.local', port: '', pathname: '/ethernet.html', search: ''};
            t.page.init(); await t.advance(100);
            t.el('nm-hostname').type('garage');
            t.saved = true;
            t.el('nm-save').click();
            await t.advance(2500);
            return t.navigations();
        """
        )
        assert r == ["http://garage.local/ethernet.html"]

    def test_save_refused_shows_the_error_under_the_field(self):
        # On a 400 invalid_config for ha_name: the detail under the HA input, buttons usable again.
        r = scenario(
            page(
                "names",
                routes(
                    put="{status: 400, body: {error: 'invalid_config', field: 'ha_name', detail: 'ha_name: is not valid UTF-8'}}"
                ),
            )
            + START
            + """
            t.el('nm-follows').tick(false); t.el('nm-ha').type('Kitchen');
            t.el('nm-save').click(); await t.advance(100);
            return [t.el('nm-ha-err').allText(), t.el('nm-save').disabled];
        """
        )
        assert "not valid UTF-8" in r[0]
        assert r[1] is False

    def test_save_not_applied_shows_the_detail_as_a_warning(self):
        # On applied false: the detail in the message line.
        r = scenario(
            page(
                "names",
                routes(
                    put="{status: 200, body: H.fixture(H.NAMES, {applied: false, detail: 'DHCP name not applied: ESP_FAIL; it applies at the next boot'})}"
                ),
            )
            + START
            + """
            t.el('nm-dhcp').type('router'); t.el('nm-save').click(); await t.advance(2500);
            return t.el('nm-msg').textContent;
        """
        )
        assert "DHCP name not applied" in r

    def test_polls_do_not_overwrite_an_edit(self):
        # On an edit followed by a poll: the edit stays.
        r = scenario(
            page("names", routes())
            + START
            + """
            t.el('nm-hostname').type('garage');
            await t.page.refresh();
            return t.el('nm-hostname').value;
        """
        )
        assert r == "garage"

    def test_section_is_disabled_while_the_ethernet_page_is_busy(self):
        # On isBlocked() true: every input and both buttons disabled, Save does nothing.
        r = scenario(
            page("names", routes())
            + """
            t.blocked = true; t.page.init(); await t.advance(100);
            t.el('nm-hostname').type('garage'); t.el('nm-save').click(); await t.advance(100);
            return [t.el('nm-hostname').disabled, t.el('nm-save').disabled, t.calls.filter((c) => c.method === 'PUT').length];
        """
        )
        assert r == [True, True, 0]
