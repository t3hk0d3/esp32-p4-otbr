"""Ethernet page (BDD: EthernetLogic, EthernetPage). Runs static/ethernet.js under node."""

import json

import pytest
from device_js import logic, needs_node, page, scenario


def eth(expression):
    return logic("ethernet", expression)


def draft(mode="single", **ifaces):
    base = {
        "single": {
            "tagged": False,
            "vlan": "",
            "dhcp": True,
            "cidr": "",
            "gw": "",
            "dns": "",
        },
        "mgmt": {
            "tagged": True,
            "vlan": "10",
            "dhcp": True,
            "cidr": "",
            "gw": "",
            "dns": "",
        },
        "thread_bb": {
            "tagged": True,
            "vlan": "20",
            "dhcp": True,
            "cidr": "",
            "gw": "",
            "dns": "",
        },
    }
    for k, v in ifaces.items():
        base[k] = base[k] | v
    return {"mode": mode} | base


SPLIT_CFG = {
    "mode": "split",
    "mgmt": {"vlan": 10, "ipv4": {"dhcp": True}},
    "thread_bb": {
        "vlan": 20,
        "ipv4": {
            "dhcp": False,
            "addr": "192.168.20.2",
            "mask": "255.255.255.0",
            "gw": "192.168.20.1",
        },
    },
}


@needs_node
class TestPrefixes:
    """prefixToMask, maskToPrefix, parseCidr"""

    @pytest.mark.parametrize(
        ("prefix", "mask"),
        [
            (24, "255.255.255.0"),
            (1, "128.0.0.0"),
            (32, "255.255.255.255"),
            (30, "255.255.255.252"),
        ],
    )
    def test_prefix_and_mask_convert_both_ways(self, prefix, mask):
        # On a prefix length 1..32: the dotted mask, and back.
        assert eth(f"[L.prefixToMask({prefix}), L.maskToPrefix('{mask}')]") == [
            mask,
            prefix,
        ]

    @pytest.mark.parametrize("prefix", [0, 33, -1])
    def test_prefix_outside_1_to_32_has_no_mask(self, prefix):
        # On 0, 33, -1: null.
        assert eth(f"L.prefixToMask({prefix})") is None

    @pytest.mark.parametrize("mask", ["255.0.255.0", "0.0.0.0", "255.255.255", "x"])
    def test_invalid_mask_has_no_prefix(self, mask):
        # On a non-contiguous, zero or malformed mask: null.
        assert eth(f"L.maskToPrefix('{mask}')") is None

    def test_cidr_splits_into_address_and_mask(self):
        # On " 10.0.0.2/24 ": address and mask.
        assert eth("L.parseCidr(' 10.0.0.2/24 ')") == {
            "addr": "10.0.0.2",
            "mask": "255.255.255.0",
        }

    @pytest.mark.parametrize(
        "text",
        ["10.0.0.2", "10.0.0.2/", "10.0.0.2/0", "10.0.0.2/33", "10.0.0/24", "a/24", ""],
    )
    def test_malformed_cidr_is_refused(self, text):
        # On a missing or bad prefix or address: null.
        assert eth(f"L.parseCidr({json.dumps(text)})") is None


@needs_node
class TestDraft:
    """draftFromConfig, validate, changes, requestBody, reachText, taggedVlans"""

    def test_draft_from_a_split_config(self):
        # On a split config: mode, tagged VLANs as text, the static address as CIDR, the single default kept.
        d = eth(f"L.draftFromConfig({json.dumps(SPLIT_CFG)})")
        assert d["mode"] == "split"
        assert d["mgmt"] == {
            "tagged": True,
            "vlan": "10",
            "dhcp": True,
            "cidr": "",
            "gw": "",
            "dns": "",
        }
        assert d["thread_bb"] == {
            "tagged": True,
            "vlan": "20",
            "dhcp": False,
            "cidr": "192.168.20.2/24",
            "gw": "192.168.20.1",
            "dns": "",
        }
        assert d["single"] == {
            "tagged": False,
            "vlan": "",
            "dhcp": True,
            "cidr": "",
            "gw": "",
            "dns": "",
        }

    def test_draft_from_no_config_is_single_untagged_dhcp(self):
        # On null: single, untagged, DHCP.
        d = eth("L.draftFromConfig(null)")
        assert d["mode"] == "single"
        assert d["single"]["tagged"] is False and d["single"]["dhcp"] is True

    def test_valid_draft_has_no_errors(self):
        # On a valid split draft: no errors.
        assert eth(f"L.validate({json.dumps(draft('split'))})") == {
            "ifaces": {"single": [], "mgmt": [], "thread_bb": []},
            "global": [],
            "count": 0,
        }

    @pytest.mark.parametrize("vlan", ["", "0", "4095", "1.5", "x"])
    def test_tagged_vlan_outside_1_to_4094_is_an_interface_error(self, vlan):
        # On a tagged VLAN that is not 1..4094: an error on that interface.
        v = eth(f"L.validate({json.dumps(draft('split', mgmt={'vlan': vlan}))})")
        assert v["count"] == 1
        assert "1 to 4094" in v["ifaces"]["mgmt"][0]

    @pytest.mark.parametrize("vlan", ["1", "4094"])
    def test_vlan_bounds_are_accepted(self, vlan):
        # On VLAN 1 and 4094: no error.
        assert (
            eth(f"L.validate({json.dumps(draft('split', mgmt={'vlan': vlan}))}).count")
            == 0
        )

    @pytest.mark.parametrize("cidr", ["", "10.0.0.2", "10.0.0.256/24", "10.0.0.2/40"])
    def test_static_without_a_valid_cidr_is_an_interface_error(self, cidr):
        # On static with a bad or missing address/prefix: the error names the 192.168.1.10/24 form.
        v = eth(
            f"L.validate({json.dumps(draft(single={'dhcp': False, 'cidr': cidr}))})"
        )
        assert "192.168.1.10/24" in v["ifaces"]["single"][0]

    @pytest.mark.parametrize("field", ["gw", "dns"])
    def test_bad_gateway_or_dns_is_an_interface_error(self, field):
        # On a malformed gateway or DNS: an error; empty is fine.
        bad = draft(single={"dhcp": False, "cidr": "10.0.0.2/24", field: "10.0.0.999"})
        ok = draft(single={"dhcp": False, "cidr": "10.0.0.2/24", field: ""})
        assert eth(f"L.validate({json.dumps(bad)}).count") == 1
        assert eth(f"L.validate({json.dumps(ok)}).count") == 0

    def test_errors_of_the_other_mode_are_ignored(self):
        # On single mode with a broken mgmt draft: no errors.
        assert (
            eth(f"L.validate({json.dumps(draft('single', mgmt={'vlan': 'x'}))}).count")
            == 0
        )

    def test_split_with_two_untagged_interfaces_is_a_global_error(self):
        # On both untagged: one global error.
        v = eth(
            f"L.validate({json.dumps(draft('split', mgmt={'tagged': False}, thread_bb={'tagged': False}))})"
        )
        assert v["count"] == 1
        assert "untagged" in v["global"][0]

    def test_split_with_equal_vlan_ids_is_a_global_error(self):
        # On mgmt and thread_bb both VLAN 20: a message about different VLAN IDs.
        v = eth(f"L.validate({json.dumps(draft('split', mgmt={'vlan': '20'}))})")
        assert "different VLAN" in v["global"][0]

    def test_split_with_one_untagged_and_one_tagged_is_accepted(self):
        # On mgmt untagged and thread_bb VLAN 20: no error.
        assert (
            eth(
                f"L.validate({json.dumps(draft('split', mgmt={'tagged': False}))}).count"
            )
            == 0
        )

    def test_no_changes_against_the_same_draft(self):
        # On a draft equal to the running one: no rows.
        d = json.dumps(draft())
        assert eth(f"L.changes({d}, {d})") == []

    def test_switching_to_split_lists_setup_vlan_and_ipv4_rows(self):
        # On single -> split: the Setup row, then VLAN and IPv4 rows for both interfaces, "Not in use" before.
        rows = eth(
            f"L.changes({json.dumps(draft())}, {json.dumps(draft('split', thread_bb={'dhcp': False, 'cidr': '192.168.20.2/24', 'gw': '192.168.20.1'}))})"
        )
        assert rows[0] == {
            "what": "Setup",
            "from": "Single port",
            "to": "Split management and Thread",
        }
        assert rows[1:] == [
            {"what": "Management VLAN", "from": "Not in use", "to": "VLAN 10"},
            {"what": "Management IPv4", "from": "Not in use", "to": "Automatic (DHCP)"},
            {"what": "Thread backbone VLAN", "from": "Not in use", "to": "VLAN 20"},
            {
                "what": "Thread backbone IPv4",
                "from": "Not in use",
                "to": "Static 192.168.20.2/24, gateway 192.168.20.1",
            },
        ]

    def test_one_field_change_is_one_row(self):
        # On single DHCP -> single static: one IPv4 row.
        rows = eth(
            f"L.changes({json.dumps(draft())}, {json.dumps(draft(single={'dhcp': False, 'cidr': '10.0.0.2/24'}))})"
        )
        assert rows == [
            {
                "what": "Ethernet port IPv4",
                "from": "Automatic (DHCP)",
                "to": "Static 10.0.0.2/24",
            }
        ]

    def test_request_body_has_the_api_shape_with_a_mask(self):
        # On a split draft with a static thread_bb: the PUT /device/network body of the API, mask not prefix.
        body = eth(
            f"L.requestBody({json.dumps(draft('split', thread_bb={'dhcp': False, 'cidr': '10.0.0.2/24', 'dns': '10.0.0.1'}))})"
        )
        assert body == {
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

    def test_request_body_for_single_untagged_dhcp(self):
        # On the default draft: {"mode":"single","single":{"vlan":0,"ipv4":{"dhcp":true}}}.
        assert eth(f"L.requestBody({json.dumps(draft())})") == {
            "mode": "single",
            "single": {"vlan": 0, "ipv4": {"dhcp": True}},
        }

    def test_reach_text_names_where_to_open_the_page(self):
        # On static mgmt: its http address; DHCP single: the DHCP server; DHCP split: the management VLAN.
        static = draft("split", mgmt={"dhcp": False, "cidr": "192.168.10.5/24"})
        assert eth(f"L.reachText({json.dumps(static)})") == "http://192.168.10.5"
        assert "DHCP server" in eth(f"L.reachText({json.dumps(draft())})")
        assert "VLAN 10" in eth(f"L.reachText({json.dumps(draft('split'))})")

    def test_tagged_vlans_of_the_draft_mode(self):
        # On split with both tagged: ["10", "20"]; on single untagged: [].
        assert eth(f"L.taggedVlans({json.dumps(draft('split'))})") == ["10", "20"]
        assert eth(f"L.taggedVlans({json.dumps(draft())})") == []


@needs_node
class TestNetworkConfig:
    """networkConfig, the API body builder behind requestBody"""

    @staticmethod
    def error_of(form):
        return eth(
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

    def test_unknown_mode_is_refused(self):
        # On a mode that is neither single nor split: a message.
        assert self.error_of({"mode": "x"})


NET_SPLIT_TRIAL = (
    "Object.assign(H.fixture(H.NETWORK), {mode: 'split', trial: Object.assign("
    + json.dumps(SPLIT_CFG)
    + ", {remaining_s: 200}),"
    " interfaces: [{name: 'mgmt', vlan: 10, link: true, ipv4: '192.168.10.5', ipv6: []},"
    " {name: 'thread_bb', vlan: 20, link: false, ipv4: null, ipv6: ['fe80::2']}]})"
)


def routes(
    net_js="H.NETWORK",
    put='{status: 200, body: {status: "trial"}}',
    status_js="H.STATUS",
):
    return (
        f"(u, o) => o.method === 'PUT' ? ({put}) : u === '/device/diag' ? ({{status: 200, body: H.DIAG}})"
        f" : u === '/device/status' ? ({{status: 200, body: {status_js}}}) : ({{status: 200, body: {net_js}}})"
    )


START = "t.page.init(); await t.advance(100);"


@needs_node
class TestEthernetPage:
    """ethernet.js run against a fake DOM and fetch"""

    def test_running_single_port_shows_its_live_state(self):
        # On single untagged DHCP: Single port pressed and in use, one card with link, IPv4 from DHCP and IPv6; no bar.
        r = scenario(
            page("ethernet", routes())
            + START
            + """
            return [t.el('mode-single').getAttribute('aria-pressed'), t.el('mode-single-now').hidden(),
                    t.el('mode-split-now').hidden(), t.el('if-single').hidden(), t.el('if-mgmt').hidden(),
                    t.el('if-single-vlanchip').textContent, t.el('if-single-linkchip').textContent,
                    t.el('if-single-ipv4').textContent, t.el('if-single-src').textContent,
                    t.el('if-single-ipv6').textContent, t.el('if-single-live').hidden(), t.el('actionBar').hidden()];
        """
        )
        assert r == [
            "true",
            False,
            True,
            False,
            True,
            "Untagged",
            "Link up",
            "192.168.1.117",
            "from DHCP",
            "fe80::1\nfdaa:1::1",
            False,
            True,
        ]

    def test_the_trial_configuration_is_what_runs_and_what_is_edited(self):
        # On a split trial: Split pressed, both cards live, the static gateway shown, link down on thread_bb, banner up.
        r = scenario(
            page("ethernet", routes(NET_SPLIT_TRIAL))
            + START
            + """
            return [t.el('mode-split').getAttribute('aria-pressed'), t.el('if-mgmt').hidden(), t.el('if-thread_bb').hidden(),
                    t.el('if-thread_bb-linkchip').textContent, t.el('if-thread_bb-src').textContent,
                    t.el('if-thread_bb-confrow').hidden(), t.el('if-thread_bb-conf').textContent,
                    t.el('trialBanner').hidden(), t.el('actionBar').hidden()];
        """
        )
        assert r == [
            "true",
            False,
            False,
            "Link down",
            "static",
            False,
            "gateway 192.168.20.1",
            False,
            True,
        ]

    def test_later_polls_do_not_overwrite_the_draft(self):
        # On an edit followed by another poll: the edit stays.
        r = scenario(
            page("ethernet", routes())
            + START
            + """
            t.el('if-single-edit').click(); t.el('if-single-staticbtn').click(); t.el('if-single-cidr').type('10.0.0.2/24');
            await t.page.refreshNetwork();
            return [t.el('if-single-cidr').value, t.el('if-single-staticbtn').getAttribute('aria-pressed')];
        """
        )
        assert r == ["10.0.0.2/24", "true"]

    def test_picking_split_opens_both_new_interfaces_in_edit(self):
        # On Split: the single card hides, mgmt and thread_bb show "Not in use yet" with their editors open.
        r = scenario(
            page("ethernet", routes())
            + START
            + """
            t.el('mode-split').click();
            return [t.el('mode-split').getAttribute('aria-pressed'), t.el('mode-single').getAttribute('aria-pressed'),
                    t.el('if-single').hidden(), t.el('if-mgmt-linkchip').textContent, t.el('if-mgmt-editor').hidden(),
                    t.el('if-thread_bb-editor').hidden(), t.el('if-mgmt-edit').textContent];
        """
        )
        assert r == ["true", "false", True, "Not in use yet", False, False, "Done"]

    def test_edits_validate_inline_and_drive_the_action_bar(self):
        # On Split with empty VLAN IDs: errors on both cards, the bar says fix 2 problems, Review disabled;
        # after entering 10 and 20: no errors, "5 changes not applied yet", Review enabled.
        r = scenario(
            page("ethernet", routes())
            + START
            + """
            t.el('mode-split').click();
            const before = [t.el('if-mgmt-errors').allText(), t.el('barText').textContent, t.el('reviewBtn').disabled];
            t.el('if-mgmt-vlan').type('10'); t.el('if-thread_bb-vlan').type('20');
            return [before, [t.el('if-mgmt-errors').children.length, t.el('barText').textContent, t.el('reviewBtn').disabled]];
        """
        )
        assert "1 to 4094" in r[0][0]
        assert r[0][1] == "Fix 2 problems before you review the changes."
        assert r[0][2] is True
        assert r[1] == [0, "5 changes not applied yet.", False]

    def test_equal_vlan_ids_show_a_global_error(self):
        # On mgmt and thread_bb both 20: the error under the cards.
        r = scenario(
            page("ethernet", routes())
            + START
            + "t.el('mode-split').click(); t.el('if-mgmt-vlan').type('20'); t.el('if-thread_bb-vlan').type('20'); return t.el('global-errors').allText();"
        )
        assert "different VLAN" in r

    def test_discard_goes_back_to_the_running_settings(self):
        # On Discard after picking Split: Single pressed again, bar hidden.
        r = scenario(
            page("ethernet", routes())
            + START
            + """
            t.el('mode-split').click(); t.el('discardBtn').click();
            return [t.el('mode-single').getAttribute('aria-pressed'), t.el('actionBar').hidden(), t.el('if-single-live').hidden()];
        """
        )
        assert r == ["true", True, False]

    def test_review_lists_the_changes_and_where_to_go(self):
        # On Review after a static single address: one diff row, the http address in the steps, no VLAN checkbox.
        r = scenario(
            page("ethernet", routes())
            + START
            + """
            t.el('if-single-edit').click(); t.el('if-single-staticbtn').click(); t.el('if-single-cidr').type('10.0.0.2/24');
            t.el('reviewBtn').click();
            return [t.el('reviewSection').hidden(), t.el('actionBar').hidden(), t.el('diffBody').children.map((tr) => tr.allText()),
                    t.el('reachText').textContent, t.el('ackRow').hidden(), t.el('applyBtn').disabled];
        """
        )
        assert r[0:2] == [False, True]
        assert r[2] == ["Ethernet port IPv4 Automatic (DHCP) Static 10.0.0.2/24"]
        assert r[3:] == ["http://10.0.0.2", True, False]

    def test_tagged_vlans_need_the_switch_checkbox_before_apply(self):
        # On Review of a split draft: the checkbox names both VLANs; Apply is enabled only once it is ticked.
        r = scenario(
            page("ethernet", routes())
            + START
            + """
            t.el('mode-split').click(); t.el('if-mgmt-vlan').type('10'); t.el('if-thread_bb-vlan').type('20');
            t.el('reviewBtn').click();
            const before = [t.el('ackRow').hidden(), t.el('vlanList').textContent, t.el('applyBtn').disabled];
            t.el('ackBox').tick(true);
            return [before, t.el('applyBtn').disabled];
        """
        )
        assert r == [[False, "VLAN 10 and VLAN 20", True], False]

    def test_apply_puts_the_body_and_waits_for_the_board(self):
        # On Apply: PUT /device/network with the draft's API body, a rebooting message, busy with one waiter.
        r = scenario(
            page("ethernet", routes())
            + START
            + """
            t.el('if-single-edit').click(); t.el('if-single-staticbtn').click(); t.el('if-single-cidr').type('10.0.0.2/24');
            t.el('reviewBtn').click();
            await t.page.apply();
            const put = t.calls.find((c) => c.method === 'PUT');
            return [put.url, JSON.parse(put.body), t.el('netMsg').textContent, t.page.kit.isBusy(), t.page.kit.isWaiting(),
                    t.confirms().length];
        """
        )
        assert r[0] == "/device/network"
        assert r[1] == {
            "mode": "single",
            "single": {
                "vlan": 0,
                "ipv4": {"dhcp": False, "addr": "10.0.0.2", "mask": "255.255.255.0"},
            },
            "trel": True,
        }
        assert "http://10.0.0.2" in r[2]
        assert r[3:] == [True, True, 0]

    def test_apply_refused_by_the_board_shows_why_and_releases_the_buttons(self):
        # On a 400 invalid_config: the detail in netMsg, not busy, Apply enabled again.
        r = scenario(
            page(
                "ethernet",
                routes(
                    put="{status: 400, body: {error: 'invalid_config', detail: 'thread_bb.vlan'}}"
                ),
            )
            + START
            + """
            t.el('if-single-edit').click(); t.el('if-single-staticbtn').click(); t.el('if-single-cidr').type('10.0.0.2/24');
            t.el('reviewBtn').click();
            await t.page.apply();
            return [t.el('netMsg').textContent, t.page.kit.isBusy(), t.el('applyBtn').disabled];
        """
        )
        assert "thread_bb.vlan" in r[0]
        assert r[1:] == [False, False]

    def test_apply_without_review_or_with_errors_sends_nothing(self):
        # On apply() with no changes: no PUT.
        r = scenario(
            page("ethernet", routes())
            + START
            + "await t.page.apply(); return t.calls.filter((c) => c.method === 'PUT').length;"
        )
        assert r == 0

    def test_poll_failure_shows_the_stale_line_and_keeps_the_action_message(self):
        # On a failing poll after an action message: netMsg unchanged, n-stale shown.
        r = scenario(
            page("ethernet", "() => { throw new Error('down'); }")
            + """
            t.page.init(); await t.advance(100);
            t.el('netMsg').textContent = 'Stored.';
            await t.page.refreshNetwork();
            return [t.el('netMsg').textContent, !t.el('n-stale').hidden()];
        """
        )
        assert r == ["Stored.", True]

    def test_fallback_is_announced_with_its_error(self):
        # On fallback with an error: the banner names it.
        r = scenario(
            page(
                "ethernet",
                routes(
                    "Object.assign(H.fixture(H.NETWORK), {fallback: true, error: 'ESP_ERR_INVALID_ARG'})"
                ),
            )
            + START
            + "return [t.el('n-fallback').hidden(), t.el('n-fallback').textContent];"
        )
        assert r[0] is False
        assert "ESP_ERR_INVALID_ARG" in r[1]

    def test_dropped_vlan_frames_are_counted(self):
        # On vlan_rx_dropped 7: the counter shows 7.
        r = scenario(
            page(
                "ethernet",
                routes("Object.assign(H.fixture(H.NETWORK), {vlan_rx_dropped: 7})"),
            )
            + START
            + "return t.el('n-dropped').textContent;"
        )
        assert r == "7"

    def test_server_text_is_rendered_as_text(self):
        # On an IPv4 string with markup: it lands in textContent unchanged.
        r = scenario(
            page(
                "ethernet",
                routes(
                    "(() => { const n = H.fixture(H.NETWORK); n.interfaces[0].ipv4 = '<b>x</b>'; return n; })()"
                ),
            )
            + START
            + "return t.el('if-single-ipv4').textContent;"
        )
        assert r == "<b>x</b>"

    def test_init_polls_the_network_and_the_status_once_each(self):
        # On init: GET /device/network once (the banner uses the same answer) and /device/status once (TREL card).
        r = scenario(
            page("ethernet", routes())
            + "t.page.init(); await t.advance(100); return t.calls.map((c) => c.url).sort();"
        )
        assert r == ["/device/network", "/device/status"]


@needs_node
class TestBoardRules:
    """validate mirrors netcfg_validate (config.c), so Review is refused for what the board would refuse"""

    @staticmethod
    def static_errors(**f):
        d = draft(single={"dhcp": False, "cidr": "192.168.1.10/24"} | f)
        return eth(f"L.validate({json.dumps(d)}).ifaces.single")

    @pytest.mark.parametrize(
        "cidr",
        [
            "192.168.1.10/31",
            "192.168.1.10/32",
            "192.168.1.0/24",
            "192.168.1.255/24",
            "127.0.0.5/8",
            "224.0.0.5/24",
            "0.0.0.0/8",
            "192.168.01.5/24",
        ],
    )
    def test_address_the_board_refuses_is_an_error(self, cidr):
        # On /31, /32, network, broadcast, loopback, multicast, 0.0.0.0 or a leading zero: an error on the address.
        assert self.static_errors(cidr=cidr)

    @pytest.mark.parametrize(
        "gw",
        ["192.168.2.1", "192.168.1.10", "192.168.1.0", "192.168.1.255", "127.0.0.1"],
    )
    def test_gateway_the_board_refuses_is_an_error(self, gw):
        # On a gateway outside the subnet, equal to the address, network, broadcast or loopback: an error.
        assert any("gateway" in e for e in self.static_errors(gw=gw))

    def test_gateway_in_the_subnet_is_accepted(self):
        # On 192.168.1.1 for 192.168.1.10/24: no error.
        assert self.static_errors(gw="192.168.1.1") == []

    def test_multicast_dns_is_an_error(self):
        # On DNS 224.0.0.1: an error.
        assert any("DNS" in e for e in self.static_errors(dns="224.0.0.1"))

    def test_both_static_in_one_subnet_is_a_global_error(self):
        # On mgmt 10.0.0.2/24 and thread_bb 10.0.0.3/24: a global error.
        d = draft(
            "split",
            mgmt={"dhcp": False, "cidr": "10.0.0.2/24"},
            thread_bb={"dhcp": False, "cidr": "10.0.0.3/24"},
        )
        assert "subnet" in eth(f"L.validate({json.dumps(d)}).global[0]")

    def test_vlan_ids_are_compared_as_numbers(self):
        # On mgmt "010" and thread_bb "10": the different-VLAN error.
        d = draft("split", mgmt={"vlan": "010"}, thread_bb={"vlan": "10"})
        assert "different VLAN" in eth(f"L.validate({json.dumps(d)}).global[0]")

    def test_unset_static_gateway_and_dns_from_the_board_are_empty(self):
        # On the board's "0.0.0.0" for an unset gateway and DNS: empty fields, and no change against itself.
        cfg = {
            "mode": "single",
            "single": {
                "vlan": 0,
                "ipv4": {
                    "dhcp": False,
                    "addr": "192.168.1.10",
                    "mask": "255.255.255.0",
                    "gw": "0.0.0.0",
                    "dns": "0.0.0.0",
                },
            },
        }
        d = eth(f"L.draftFromConfig({json.dumps(cfg)}).single")
        assert (d["gw"], d["dns"]) == ("", "")


STATIC_NET = (
    "Object.assign(H.fixture(H.NETWORK), {active: {mode: 'single', single: {vlan: 0, ipv4: {dhcp: false, addr: '192.168.1.10',"
    " mask: '255.255.255.0', gw: '0.0.0.0', dns: '0.0.0.0'}}}})"
)


@needs_node
class TestEthernetPageReview:
    """findings of the whole-branch review"""

    def test_unset_gateway_reads_as_none(self):
        # On a static config with the board's 0.0.0.0 gateway and DNS: "no gateway or DNS server", no pending change.
        r = scenario(
            page("ethernet", routes(STATIC_NET))
            + START
            + "return [t.el('if-single-conf').textContent, t.el('actionBar').hidden()];"
        )
        assert r == ["no gateway or DNS server", True]

    def test_changing_a_vlan_after_ticking_needs_a_new_tick(self):
        # On tick, back to editing, VLAN 20 -> 30, review again: the box is clear and Apply disabled.
        r = scenario(
            page("ethernet", routes())
            + START
            + """
            t.el('mode-split').click(); t.el('if-mgmt-vlan').type('10'); t.el('if-thread_bb-vlan').type('20');
            t.el('reviewBtn').click(); t.el('ackBox').tick(true);
            t.el('backBtn').click(); t.el('if-thread_bb-edit').click(); t.el('if-thread_bb-vlan').type('30');
            t.el('reviewBtn').click();
            return [t.el('vlanList').textContent, t.el('ackBox').checked, t.el('applyBtn').disabled];
        """
        )
        assert r == ["VLAN 10 and VLAN 30", False, True]

    def test_editing_is_ignored_while_the_board_reboots(self):
        # On Apply, then clicks on Edit and a mode card during the wait: the review and its message stay.
        r = scenario(
            page("ethernet", routes())
            + START
            + """
            t.el('if-single-edit').click(); t.el('if-single-staticbtn').click(); t.el('if-single-cidr').type('10.0.0.2/24');
            t.el('reviewBtn').click(); await t.page.apply();
            t.el('if-single-edit').click(); t.el('mode-split').click();
            return [t.el('reviewSection').hidden(), t.el('netMsg').textContent.includes('Keep new settings'),
                    t.el('mode-single').getAttribute('aria-pressed')];
        """
        )
        assert r == [False, True, "true"]

    def test_apply_answered_409_shows_busy_and_releases_the_buttons(self):
        # On a 409 from PUT: the busy message, not busy afterwards.
        r = scenario(
            page(
                "ethernet",
                routes(
                    put="{status: 409, body: {error: 'busy', detail: 'update running'}}"
                ),
            )
            + START
            + """
            t.el('if-single-edit').click(); t.el('if-single-staticbtn').click(); t.el('if-single-cidr').type('10.0.0.2/24');
            t.el('reviewBtn').click(); await t.page.apply();
            return [t.el('netMsg').textContent, t.page.kit.isBusy()];
        """
        )
        assert "busy" in r[0].lower() and "update running" in r[0]
        assert r[1] is False

    def test_errors_stay_visible_when_the_editor_is_closed(self):
        # On a bad address and Done: the error is still shown on the card.
        r = scenario(
            page("ethernet", routes())
            + START
            + """
            t.el('if-single-edit').click(); t.el('if-single-staticbtn').click(); t.el('if-single-cidr').type('10.0.0.2');
            t.el('if-single-edit').click();
            return [t.el('if-single-editor').hidden(), t.el('if-single-errors').allText()];
        """
        )
        assert r[0] is True
        assert "192.168.1.10/24" in r[1]

    def test_error_messages_are_not_rebuilt_while_unchanged(self):
        # On a poll that changes nothing: the error element is the same object (screen readers do not re-announce).
        r = scenario(
            page("ethernet", routes())
            + START
            + """
            t.el('mode-split').click(); const first = t.el('if-mgmt-errors').children[0];
            await t.page.refreshNetwork();
            return t.el('if-mgmt-errors').children[0] === first;
        """
        )
        assert r is True


@needs_node
class TestEthernetPrReview:
    """PR review findings"""

    @pytest.mark.parametrize("field", ["gw", "dns"])
    def test_zero_address_means_unset(self, field):
        # On 0.0.0.0 as gateway or DNS: no error (the board treats it as not set).
        d = draft(single={"dhcp": False, "cidr": "192.168.1.10/24", field: "0.0.0.0"})
        assert eth(f"L.validate({json.dumps(d)}).count") == 0

    def test_vlan_with_a_leading_zero_is_no_change(self):
        # On a running VLAN 10 and the draft "010": no change rows.
        cur = draft("split")
        new = draft("split", mgmt={"vlan": "010"})
        assert eth(f"L.changes({json.dumps(cur)}, {json.dumps(new)})") == []

    def test_review_closes_when_the_running_settings_change(self):
        # On an open review and a poll with different running settings: the review closes and the tick clears.
        r = scenario(
            page("ethernet", "(u, o) => ({status: 200, body: t.n || H.NETWORK})")
            + START
            + """
            t.el('mode-split').click(); t.el('if-mgmt-vlan').type('10'); t.el('if-thread_bb-vlan').type('20');
            t.el('reviewBtn').click(); t.el('ackBox').tick(true);
            t.n = H.fixture(H.NETWORK, {active: {mode: 'single', single: {vlan: 7, ipv4: {dhcp: true}}}});
            await t.page.refreshNetwork();
            return [t.el('reviewSection').hidden(), t.el('ackBox').checked];
        """
        )
        assert r == [True, False]

    def test_give_up_keeps_the_new_address(self):
        # On Apply of a static address and a board that never comes back here: the message still names it.
        r = scenario(
            page("ethernet", routes())
            + START
            + """
            t.el('if-single-edit').click(); t.el('if-single-staticbtn').click(); t.el('if-single-cidr').type('10.0.0.2/24');
            t.el('reviewBtn').click(); await t.page.apply();
            await t.advance(200000);
            return [t.el('netMsg').textContent, t.page.kit.isBusy()];
        """
        )
        assert "http://10.0.0.2" in r[0] and "5 minutes" in r[0]
        assert r[1] is False

    def test_lost_reply_to_apply_names_where_to_look(self):
        # On a dropped connection during the PUT: the error plus where to open the page if it was saved.
        r = scenario(
            page(
                "ethernet",
                "(u, o) => { if (o.method === 'PUT') throw new Error('reset'); return {status: 200, body: H.NETWORK}; }",
            )
            + START
            + """
            t.el('if-single-edit').click(); t.el('if-single-staticbtn').click(); t.el('if-single-cidr').type('10.0.0.2/24');
            t.el('reviewBtn').click(); await t.page.apply();
            return t.el('netMsg').textContent;
        """
        )
        assert "No answer" in r and "http://10.0.0.2" in r


TREL_STATUS = (
    "H.fixture(H.STATUS, {trel: {enabled: true, active: true, peers: 2, peers_other: 0, tx_packets: 18204,"
    " tx_failures: 0, rx_packets: 17951, peer_list: ["
    "{ext_address: '1a2b3c4d5e6f7081', ext_pan_id: 'adf0d025c4b51f00', same_network: true,"
    " preference: {ieee802154: 255, trel: 255}, address: '[fe80::1]:12390'},"
    " {ext_address: '9f8e7d6c5b4a3921', ext_pan_id: 'adf0d025c4b51f00', same_network: true,"
    " preference: {ieee802154: 230, trel: 55}, address: '[fe80::2]:12390'}]}})"
)
# One partner of this network, one advertiser of another network (not a neighbour: no preference).
TREL_TWO_NETWORKS = (
    "H.fixture(H.STATUS, {trel: {enabled: true, active: true, peers: 1, peers_other: 1, tx_packets: 1,"
    " tx_failures: 0, rx_packets: 1, peer_list: ["
    "{ext_address: '1a2b3c4d5e6f7081', ext_pan_id: 'adf0d025c4b51f00', same_network: true,"
    " preference: {ieee802154: null, trel: 255}, address: '[fe80::1]:12390'},"
    " {ext_address: '7e287c8b8b140ee6', ext_pan_id: '5965b57e30c0def4', same_network: false, preference: null,"
    " address: '[fe80::2]:48949'}]}})"
)
NET_TREL_OFF = "H.fixture(H.NETWORK, {active: {mode: 'single', single: {vlan: 0, ipv4: {dhcp: true}}, trel: false}})"


@needs_node
class TestTrelLogic:
    """draftFromConfig, changes, requestBody, trelOnly, trelView"""

    def test_config_without_trel_runs_trel(self):
        # On a configuration of older firmware (no "trel"): the draft has TREL on, the board's default.
        assert (
            eth(
                "L.draftFromConfig({mode: 'single', single: {vlan: 0, ipv4: {dhcp: true}}}).trel"
            )
            is True
        )

    def test_config_with_trel_false_gives_a_draft_with_trel_off(self):
        assert (
            eth(
                "L.draftFromConfig({mode: 'single', single: {vlan: 0, ipv4: {dhcp: true}}, trel: false}).trel"
            )
            is False
        )

    def test_trel_change_is_a_review_row(self):
        d = draft() | {"trel": True}
        r = eth(
            f"L.changes({json.dumps(d)}, Object.assign({json.dumps(d)}, {{trel: false}}))"
        )
        assert r == [{"what": "TREL (Thread over Ethernet)", "from": "On", "to": "Off"}]

    def test_off_while_still_running_says_so(self):
        # On a saved Off that OpenThread has not applied yet (lock busy): Off, with a hint that TREL still runs.
        r = eth(
            "L.trelView({enabled: false, active: true, peers: 1, tx_packets: 0, tx_failures: 0, rx_packets: 0,"
            " peer_list: []}, 'single')"
        )
        assert r["chip"] == "Off"
        assert "still runs" in r["hint"]

    def test_request_body_carries_trel(self):
        d = draft() | {"trel": False}
        assert eth(f"L.requestBody({json.dumps(d)}).trel") is False

    def test_trel_only_when_nothing_else_differs(self):
        d = draft() | {"trel": True}
        off = d | {"trel": False}
        both = off | {"single": d["single"] | {"tagged": True, "vlan": "5"}}
        assert eth(f"L.trelOnly({json.dumps(d)}, {json.dumps(off)})") is True
        assert eth(f"L.trelOnly({json.dumps(d)}, {json.dumps(both)})") is False
        assert eth(f"L.trelOnly({json.dumps(d)}, {json.dumps(d)})") is False

    def test_no_trel_object_means_no_card(self):
        assert eth("L.trelView(null, 'single').available") is False
        assert eth("L.trelView(undefined, 'single').available") is False

    @pytest.mark.parametrize(
        ("trel", "chip"),
        [
            ("{enabled: false, active: false, peers: 0}", "Off"),
            ("{enabled: true, active: null, peers: null}", "Unknown"),
            ("{enabled: true, active: false, peers: 0}", "Waiting for Thread"),
            ("{enabled: true, active: true, peers: 1}", "Active"),
        ],
    )
    def test_chip_names_the_state(self, trel, chip):
        assert eth(f"L.trelView({trel}, 'single').chip") == chip

    def test_active_without_peers_explains_how_to_check(self):
        hint = eth("L.trelView({enabled: true, active: true, peers: 0}, 'single').hint")
        assert "avahi-browse -rt _trel._udp" in hint

    def test_where_names_the_backbone_interface(self):
        assert eth("L.trelView({enabled: true}, 'split').where") == "On Thread backbone"
        assert eth("L.trelView({enabled: true}, 'single').where") == "On Ethernet port"

    def test_unread_counters_are_dashes_and_large_ones_grouped(self):
        assert (
            eth(
                "L.trelView({enabled: true, active: null, peers: null}, 'single').peers"
            )
            == "-"
        )
        assert (
            eth(
                "L.trelView({enabled: true, active: true, tx_packets: 1234567}, 'single').tx"
            )
            == "1\u2009234\u2009567"
        )

    def test_peer_of_this_network_gets_the_this_chip(self):
        r = eth(
            "L.trelPeerView({ext_address: 'a', ext_pan_id: 'adf0d025c4b51f00', same_network: true,"
            " preference: null, address: 'x'})"
        )
        assert [r["panId"], r["network"], r["networkClass"]] == [
            "adf0d025c4b51f00",
            "this",
            "chip chip-ok",
        ]

    def test_peer_of_another_network_gets_the_other_chip(self):
        r = eth(
            "L.trelPeerView({ext_address: 'a', ext_pan_id: '5965b57e30c0def4', same_network: false,"
            " preference: null, address: 'x'})"
        )
        assert [r["network"], r["networkClass"]] == ["other", "chip chip-bad"]

    def test_peer_without_a_network_flag_is_unknown(self):
        r = eth("L.trelPeerView({ext_address: 'a', preference: null, address: 'x'})")
        assert [r["panId"], r["network"], r["networkClass"]] == ["-", "-", "chip"]

    def test_peers_of_other_networks_are_counted_beside_the_peers(self):
        v = eth(
            "L.trelView({enabled: true, active: true, peers: 1, peers_other: 2}, 'single')"
        )
        assert [v["peers"], v["peersOther"]] == ["1", "+ 2 peers of other networks"]

    def test_no_other_network_count_without_foreign_peers(self):
        assert (
            eth(
                "L.trelView({enabled: true, active: true, peers: 1, peers_other: 0}, 'single').peersOther"
            )
            == ""
        )
        assert (
            eth(
                "L.trelView({enabled: true, active: true, peers: 1}, 'single').peersOther"
            )
            == ""
        )

    def test_peer_preference_parts_mark_a_low_trel(self):
        r = eth(
            "L.trelPeerView({same_network: true, preference: {ieee802154: 230, trel: 55}}).prefParts"
        )
        assert r == [
            {"text": "15.4 90%", "low": False},
            {"text": " \u00b7 ", "low": False},
            {"text": "TREL ", "low": False},
            {"text": "22%", "low": True},
        ]

    def test_non_neighbour_has_no_preference(self):
        assert (
            eth("L.trelPeerView({same_network: false, preference: null}).prefParts")
            == []
        )


@needs_node
class TestTrelCard:
    """The TREL card of ethernet.js against a fake DOM and fetch"""

    def test_card_shows_state_counters_and_peers(self):
        r = scenario(
            page("ethernet", routes(status_js=TREL_STATUS))
            + START
            + """
            return [t.el('trelSection').hidden(), t.el('trel-chip').textContent, t.el('trel-where').textContent,
                    t.el('trel-on').getAttribute('aria-pressed'), t.el('trel-peers').textContent,
                    t.el('trel-peerwrap').hidden(), t.el('trel-peerlist').children.length];
        """
        )
        assert r == [False, "Active", "On Ethernet port", "true", "2", False, 2]

    def test_network_column_shows_the_pan_id_and_a_chip(self):
        r = scenario(
            page("ethernet", routes(status_js=TREL_TWO_NETWORKS))
            + START
            + r"""
            const rows = t.el('trel-peerlist').children;
            return rows.map((tr) => [tr.children[1].allText().replace(/\s+/g, ' '), tr.children[1].children[0].className]);
        """
        )
        assert r == [
            ["adf0d025c4b51f00 this", "chip chip-ok"],
            ["5965b57e30c0def4 other", "chip chip-bad"],
        ]

    def test_peers_figure_counts_this_network_and_names_the_others(self):
        r = scenario(
            page("ethernet", routes(status_js=TREL_TWO_NETWORKS))
            + START
            + """
            return [t.el('trel-peers').textContent, t.el('trel-peers-other').hidden(),
                    t.el('trel-peers-other').textContent];
        """
        )
        assert r == ["1", False, "+ 1 peer of another network"]

    def test_no_other_network_note_without_foreign_peers(self):
        r = scenario(
            page("ethernet", routes(status_js=TREL_STATUS))
            + START
            + "return t.el('trel-peers-other').hidden();"
        )
        assert r is True

    def test_preference_column_reads_both_radios_with_a_low_trel_in_red(self):
        r = scenario(
            page("ethernet", routes(status_js=TREL_STATUS))
            + START
            + r"""
            const cells = t.el('trel-peerlist').children.map((tr) => tr.children[2]);
            return cells.map((td) => [td.allText().replace(/\s+/g, ' '), td.findAll((e) => e.className === 'pref-low').length]);
        """
        )
        assert r == [["15.4 100% \u00b7 TREL 100%", 0], ["15.4 90% \u00b7 TREL 22%", 1]]

    def test_preference_of_a_non_neighbour_is_a_dash(self):
        r = scenario(
            page("ethernet", routes(status_js=TREL_TWO_NETWORKS))
            + START
            + "return t.el('trel-peerlist').children.map((tr) => tr.children[2].allText().replace(/\\s+/g, ' '));"
        )
        assert r == ["TREL 100%", "-"]

    def test_build_without_trel_hides_the_card(self):
        r = scenario(
            page("ethernet", routes(status_js="H.fixture(H.STATUS, {trel: null})"))
            + START
            + "return t.el('trelSection').hidden();"
        )
        assert r is True

    def test_switch_alone_is_sent_at_once_without_review(self):
        # On Off with nothing else changed: PUT of the running config with trel false; no action bar, no review.
        r = scenario(
            page(
                "ethernet",
                routes(
                    status_js=TREL_STATUS,
                    put="{status: 200, body: {status: 'applied'}}",
                ),
            )
            + START
            + """
            await t.page.setTrel(false);
            const put = t.calls.find((c) => c.method === 'PUT');
            return [put.url, JSON.parse(put.body), t.el('actionBar').hidden(), t.el('reviewSection').hidden(),
                    t.el('trelMsg').textContent, t.page.kit.isWaiting()];
        """
        )
        assert r[0] == "/device/network"
        assert r[1] == {
            "mode": "single",
            "single": {"vlan": 0, "ipv4": {"dhcp": True}},
            "trel": False,
        }
        assert r[2:] == [True, True, "TREL off.", False]

    def test_switch_with_other_changes_joins_the_review(self):
        # On Off while an interface edit is pending: no PUT; the review lists TREL with the other change.
        r = scenario(
            page("ethernet", routes(status_js=TREL_STATUS))
            + START
            + """
            t.el('if-single-edit').click(); t.el('if-single-tagged').click(); t.el('if-single-vlan').type('7');
            await t.page.setTrel(false);
            t.el('reviewBtn').click();
            const rows = t.el('diffBody').children.map((tr) => tr.children[0].textContent);
            return [t.calls.filter((c) => c.method === 'PUT').length, rows];
        """
        )
        assert r[0] == 0
        assert "TREL (Thread over Ethernet)" in r[1]
        assert "Ethernet port VLAN" in r[1]

    def test_switch_during_a_trial_asks_to_settle_it_first(self):
        r = scenario(
            page("ethernet", routes(NET_SPLIT_TRIAL, status_js=TREL_STATUS))
            + START
            + """
            await t.page.setTrel(false);
            return [t.calls.filter((c) => c.method === 'PUT').length, t.el('trelMsg').textContent];
        """
        )
        assert r[0] == 0
        assert "trial" in r[1]

    def test_switch_refused_by_the_board_shows_why_and_keeps_the_state(self):
        r = scenario(
            page(
                "ethernet",
                routes(
                    status_js=TREL_STATUS,
                    put="{status: 409, body: {error: 'busy', detail: 'confirm or revert the network change first'}}",
                ),
            )
            + START
            + """
            await t.page.setTrel(false);
            return [t.el('trelMsg').textContent, t.el('trel-on').getAttribute('aria-pressed'), t.el('trel-on').disabled];
        """
        )
        assert "confirm or revert" in r[0]
        assert r[1:] == ["true", False]

    def test_switch_sends_the_settings_running_now(self):
        # On Off after another client made the port static: the PUT carries the static settings read just before it.
        r = scenario(
            page(
                "ethernet",
                "(() => { let n = 0; return (u, o) => o.method === 'PUT' ? ({status: 200, body: {status: 'applied'}})"
                " : u === '/device/diag' ? ({status: 200, body: H.DIAG})"
                f" : u === '/device/status' ? ({{status: 200, body: {TREL_STATUS}}})"
                f" : ({{status: 200, body: n++ === 0 ? H.NETWORK : {STATIC_NET}}}); }})()",
            )
            + START
            + """
            await t.page.setTrel(false);
            const put = t.calls.find((c) => c.method === 'PUT');
            return [JSON.parse(put.body).single.ipv4.dhcp, t.el('trelMsg').textContent, t.el('actionBar').hidden()];
        """
        )
        assert r == [False, "TREL off.", True]

    def test_switch_answered_with_a_trial_waits_for_the_board(self):
        # On a trial answer (the board still saw another change): the reboot note, and the page waits for the board.
        r = scenario(
            page("ethernet", routes(status_js=TREL_STATUS))
            + START
            + """
            await t.page.setTrel(false);
            return [t.el('trelMsg').textContent, t.page.kit.isWaiting()];
        """
        )
        assert "trial" in r[0]
        assert r[1] is True

    def test_review_left_with_only_trel_is_applied_without_a_reboot(self):
        # An interface edit undone after TREL joined the draft: Apply sends a trel-only PUT, the board applies it live.
        r = scenario(
            page(
                "ethernet",
                routes(
                    status_js=TREL_STATUS,
                    put="{status: 200, body: {status: 'applied'}}",
                ),
            )
            + START
            + """
            t.el('if-single-edit').click(); t.el('if-single-tagged').click(); t.el('if-single-vlan').type('7');
            await t.page.setTrel(false);
            t.el('if-single-untagged').click();
            t.el('reviewBtn').click();
            await t.page.apply();
            const put = t.calls.find((c) => c.method === 'PUT');
            return [JSON.parse(put.body).trel, t.el('netMsg').textContent, t.page.kit.isWaiting(),
                    t.el('actionBar').hidden(), t.el('trel-off').getAttribute('aria-pressed')];
        """
        )
        assert r == [False, "TREL off.", False, True, "true"]

    def test_failed_status_refresh_marks_the_card_stale(self):
        # On a status poll that fails after a good one: the stale line names the failure; the next good poll hides it.
        r = scenario(
            page(
                "ethernet",
                "(() => { let n = 0; return (u, o) => o.method === 'PUT' ? ({status: 500, body: {}})"
                " : u === '/device/status' ? (n++ === 1 ? ({status: 503, body: {error: 'busy', detail: 'try again'}})"
                f" : ({{status: 200, body: {TREL_STATUS}}})) : u === '/device/diag' ? ({{status: 200, body: H.DIAG}})"
                " : ({status: 200, body: H.NETWORK}); })()",
            )
            + START
            + """
            const before = t.el('trel-stale').hidden();
            await t.page.refreshTrel();
            const failed = [t.el('trel-stale').hidden(), t.el('trel-stale').textContent];
            await t.page.refreshTrel();
            return [before, failed, t.el('trel-stale').hidden()];
        """
        )
        assert r[0] is True
        assert r[1][0] is False
        assert "could not be read" in r[1][1]
        assert r[2] is True

    def test_apply_waits_while_the_switch_is_being_sent(self):
        # On Apply while the TREL PUT is still in flight: Apply sends nothing, so the trial never carries a stale trel.
        r = scenario(
            page(
                "ethernet",
                routes(
                    status_js=TREL_STATUS,
                    put="{status: 200, body: {status: 'applied'}}",
                ),
            )
            + START
            + """
            const sending = t.page.setTrel(false);
            t.el('if-single-edit').click(); t.el('if-single-staticbtn').click(); t.el('if-single-cidr').type('10.0.0.2/24');
            t.el('reviewBtn').click();
            const disabled = t.el('applyBtn').disabled;
            await t.page.apply();
            await sending;
            return [disabled, t.calls.filter((c) => c.method === 'PUT').length];
        """
        )
        assert r == [True, 1]

    def test_edits_wait_while_the_switch_is_being_sent(self):
        # On an interface edit while the TREL PUT is in flight: the edit is not taken (the draft is about to be replaced
        # by the running settings, which would drop it unseen); after the switch the page shows no pending change.
        r = scenario(
            page(
                "ethernet",
                routes(
                    status_js=TREL_STATUS,
                    put="{status: 200, body: {status: 'applied'}}",
                ),
            )
            + START
            + """
            const sending = t.page.setTrel(false);
            t.el('if-single-edit').click(); t.el('if-single-tagged').click();
            const during = t.el('actionBar').hidden();
            await sending;
            const put = t.calls.find((c) => c.method === 'PUT');
            return [during, t.el('actionBar').hidden(), JSON.parse(put.body).single.vlan];
        """
        )
        assert r == [True, True, 0]

    def test_running_trel_off_is_shown_pressed_off(self):
        r = scenario(
            page(
                "ethernet",
                routes(
                    NET_TREL_OFF,
                    status_js="H.fixture(H.STATUS, {trel: {enabled: false, active: false, peers: 0, tx_packets: 0,"
                    " tx_failures: 0, rx_packets: 0, peer_list: []}})",
                ),
            )
            + START
            + "return [t.el('trel-off').getAttribute('aria-pressed'), t.el('trel-chip').textContent, t.el('actionBar').hidden()];"
        )
        assert r == ["true", "Off", True]
