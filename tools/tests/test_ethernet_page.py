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


def routes(net_js="H.NETWORK", put='{status: 200, body: {status: "trial"}}'):
    return f"(u, o) => o.method === 'PUT' ? ({put}) : u === '/device/diag' ? ({{status: 200, body: H.DIAG}}) : ({{status: 200, body: {net_js}}})"


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

    def test_init_polls_only_the_network(self):
        # On init: GET /device/network once (the banner uses the same answer), nothing else.
        r = scenario(
            page("ethernet", routes())
            + "t.page.init(); await t.advance(100); return t.calls.map((c) => c.url);"
        )
        assert r == ["/device/network"]
