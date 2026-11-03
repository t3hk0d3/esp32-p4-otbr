"""Routing page (BDD: RoutingLogic, RoutingPage). Runs static/routing.js under node."""

import json

from device_js import logic, needs_node, page, scenario

# GET /device/routing (spec section 6) with method bgp, a custom OMR prefix and one established peer.
STATE = {
    "method": "bgp",
    "omr": {"mode": "custom", "prefix": "2001:db8:42:100::/64"},
    "announce_ula": True,
    "bgp": {
        "local_asn": 4200000001,
        "router_id": None,
        "hold_time": 90,
        "peers": [{"address": "2001:db8:42::1", "asn": 4200000001}],
    },
    "status": {
        "omr_prefix": "2001:db8:42:100::/64",
        "ula_prefix": "fd98:2656:4740:1::/64",
        "announced": ["2001:db8:42:100::/64", "fd98:2656:4740:1::/64"],
        "bgp": {
            "router_id": "192.168.20.15",
            "blocked": None,
            "peers": [
                {
                    "address": "2001:db8:42::1",
                    "state": "established",
                    "uptime_s": 8123,
                    "last_error": None,
                    "received_dropped": 3,
                }
            ],
        },
        "ripng": None,
    },
}

PD_STATE = STATE | {
    "method": "pd",
    "omr": {"mode": "auto", "prefix": None},
    "announce_ula": False,
    "status": STATE["status"] | {"announced": [], "bgp": None, "ripng": None},
}


def rl(expression):
    return logic("routing", expression)


def draft_of(state):
    return f"L.draftFromState({json.dumps(state)})"


def routes(state=STATE, put=None):
    """Fake device: GET /device/routing answers `state`, PUT answers `put` (default: 200 with the body echoed)."""
    put_js = (
        put
        or f"{{status: 200, body: Object.assign(JSON.parse(o.body), {{status: {json.dumps(state['status'])}}})}}"
    )
    return (
        f"(u, o) => o.method === 'PUT' ? ({put_js}) : u === '/device/routing' ? ({{status: 200, body: {json.dumps(state)}}})"
        " : u === '/device/status' ? ({status: 200, body: H.STATUS}) : ({status: 200, body: H.NETWORK})"
    )


START = "t.page.init(); await t.advance(100);"


@needs_node
class TestVisibleSections:
    """RoutingLogic.visibleSections"""

    def test_method_bgp_bgp_section_visible_ripng_hidden(self):
        # method bgp: bgp section visible, ripng hidden
        r = rl(f"L.visibleSections({draft_of(STATE)})")
        assert r["bgp"] is True
        assert r["ripng"] is False

    def test_method_pd_neither_section(self):
        # method pd: neither section
        r = rl(f"L.visibleSections({draft_of(PD_STATE)})")
        assert [r["bgp"], r["ripng"]] == [False, False]

    def test_auto_omr_no_custom_prefix_field(self):
        # auto omr: no custom prefix field
        assert rl(f"L.visibleSections({draft_of(PD_STATE)})")["customPrefix"] is False

    def test_custom_omr_shows_the_custom_prefix_field(self):
        # custom omr: the custom prefix field is shown
        assert rl(f"L.visibleSections({draft_of(STATE)})")["customPrefix"] is True


@needs_node
class TestBody:
    """RoutingLogic.bodyFromDraft"""

    def test_body_carries_method_omr_announce_ula_bgp(self):
        # body: carries method, omr, announce_ula, bgp
        r = rl(f"L.bodyFromDraft({draft_of(STATE)})")
        assert r == {
            "method": "bgp",
            "omr": {"mode": "custom", "prefix": "2001:db8:42:100::/64"},
            "announce_ula": True,
            "bgp": {
                "local_asn": 4200000001,
                "router_id": None,
                "hold_time": 90,
                "peers": [{"address": "2001:db8:42::1", "asn": 4200000001}],
            },
        }

    def test_body_auto_omr_has_a_null_prefix_and_keeps_bgp(self):
        # body, auto OMR and method pd: prefix null, the stored bgp object still sent
        r = rl(f"L.bodyFromDraft({draft_of(PD_STATE)})")
        assert r["omr"] == {"mode": "auto", "prefix": None}
        assert r["bgp"]["local_asn"] == 4200000001
        assert "status" not in r


@needs_node
class TestDirtyCount:
    """RoutingLogic.dirtyCount"""

    def test_unchanged_draft_dirty_count_0(self):
        # unchanged draft: dirty count 0
        assert rl(f"L.dirtyCount({draft_of(STATE)}, {draft_of(STATE)})") == 0

    def test_method_and_hold_time_changed_dirty_count_2(self):
        # method and hold time changed: dirty count 2
        r = rl(
            f"(() => {{ const s = {draft_of(STATE)}, d = {draft_of(STATE)}; d.method = 'ripng'; d.hold_time = '30';"
            " return L.dirtyCount(s, d); })()"
        )
        assert r == 2


@needs_node
class TestMethodHint:
    """RoutingLogic.methodHint"""

    def test_method_hint_pd_soliciting(self):
        # method hint pd soliciting: 'DHCPv6 PD: soliciting, nothing delegated yet.'
        r = rl(
            f"L.methodHint({json.dumps(PD_STATE)}, {{state: 'running', prefix: null}})"
        )
        assert r == "DHCPv6 PD: soliciting, nothing delegated yet."

    def test_method_hint_bgp_counts_established_sessions(self):
        # method hint bgp: 'BGP: 1 of 1 sessions established. DHCPv6 PD is off.'
        assert (
            rl(f"L.methodHint({json.dumps(STATE)})")
            == "BGP: 1 of 1 sessions established. DHCPv6 PD is off."
        )


@needs_node
class TestRoutingPage:
    """routing.js run against a fake DOM and fetch"""

    def test_load_pick_ripng_save_put_carries_method_ripng(self):
        # load, pick RIPng, save: the PUT carries "method":"ripng"
        r = scenario(
            page("routing", routes())
            + START
            + """
            t.el('method-ripng').click();
            const bar = t.el('actionBar').hidden();
            await t.page.save();
            const put = t.calls.filter((c) => c.method === 'PUT');
            return [bar, put.length, put[0].url, JSON.parse(put[0].body).method, t.el('actionBar').hidden()];
        """
        )
        assert r == [False, 1, "/device/routing", "ripng", True]

    def test_400_from_put_shows_the_bad_field_next_to_its_input(self):
        # 400 from PUT: shows the bad field next to its input
        r = scenario(
            page(
                "routing",
                routes(
                    put="{status: 400, body: {error: 'invalid_request', detail: 'omr.prefix'}}"
                ),
            )
            + START
            + """
            t.el('customPrefix').type('fe80::/64');
            await t.page.save();
            return [t.el('err-omr').hidden(), t.el('err-omr').textContent, t.el('err-asn').hidden(),
                    t.el('saveMsg').textContent, t.el('actionBar').hidden()];
        """
        )
        assert r[0] is False
        assert "/64" in r[1]
        assert r[2] is True
        assert r[3] == ""
        assert r[4] is False

    def test_400_naming_an_unknown_field_goes_to_the_page_error_line(self):
        # 400 naming a field the page does not have: the page-level error line shows it
        r = scenario(
            page(
                "routing",
                routes(
                    put="{status: 400, body: {error: 'invalid_request', detail: 'omr.mode'}}"
                ),
            )
            + START
            + "t.el('method-off').click(); await t.page.save(); return t.el('saveMsg').textContent;"
        )
        assert "omr.mode" in r

    def test_in_use_chip_marks_the_saved_method_not_the_picked_one(self):
        # pick RIPng without saving: BGP keeps "In use now", RIPng is pressed
        r = scenario(
            page("routing", routes())
            + START
            + """
            t.el('method-ripng').click();
            return [t.el('method-bgp-now').classList.contains('off'), t.el('method-ripng-now').classList.contains('off'),
                    t.el('method-ripng').getAttribute('aria-pressed'), t.el('method-bgp').getAttribute('aria-pressed'),
                    t.el('bgpArea').hidden(), t.el('ripngArea').hidden()];
        """
        )
        assert r == [False, True, "true", "false", True, False]

    def test_status_refresh_keeps_unsaved_edits(self):
        # 5 s later, a status refresh: the typed hold time and the dirty bar stay
        r = scenario(
            page("routing", routes())
            + START
            + """
            t.el('holdTime').type('30');
            await t.advance(5000);
            const gets = t.calls.filter((c) => c.url === '/device/routing').length;
            return [gets, t.el('holdTime').value, t.el('actionBar').hidden(), t.el('barText').textContent];
        """
        )
        assert r == [2, "30", False, "1 unsaved change"]

    def test_sessions_table_shows_the_peer_state(self):
        # BGP active with one established peer: a row with its address, Established and the dropped count
        r = scenario(
            page("routing", routes()) + START + "return t.el('sessionsBody').allText();"
        )
        assert "2001:db8:42::1" in r
        assert "Established" in r
        assert "iBGP" in r

    def test_add_peer_stops_at_two(self):
        # Add peer twice from one peer: two rows, the button disabled
        r = scenario(
            page("routing", routes())
            + START
            + """
            t.el('addPeerBtn').click(); t.el('addPeerBtn').click();
            return [t.el('peerRows').children.length, t.el('addPeerBtn').disabled];
        """
        )
        assert r == [2, True]

    def test_pd_hint_reads_the_pd_state_from_device_status(self):
        # method pd saved: the hint comes from /device/status pd (running, no prefix)
        r = scenario(
            page("routing", routes(PD_STATE))
            + START
            + "return t.el('methodHint').textContent;"
        )
        assert r == "DHCPv6 PD: soliciting, nothing delegated yet."


@needs_node
class TestFixRound1:
    """review fix round 1: no lost input, no silent 400"""

    def test_400_bgp_local_asn_while_ripng_is_picked_shows_a_visible_message(self):
        # 400 bgp.local_asn while RIPng is picked: shows a visible message
        r = scenario(
            page(
                "routing",
                routes(
                    put="{status: 400, body: {error: 'invalid_request', detail: 'bgp.local_asn'}}"
                ),
            )
            + START
            + """
            t.el('method-ripng').click();
            await t.page.save();
            return [t.el('bgpArea').hidden(), t.el('saveMsg').textContent];
        """
        )
        assert r[0] is True
        assert r[1].startswith("BGP settings: ")
        assert "Pick BGP" in r[1]

    def test_inputs_are_disabled_while_a_save_runs_and_enabled_after(self):
        # Save in flight: inputs, cards and peer fields disabled; after the reply: enabled again
        r = scenario(
            page("routing", routes())
            + START
            + """
            t.el('holdTime').type('30');
            const p = t.page.save();
            const during = ['holdTime', 'customPrefix', 'ulaBox', 'method-pd', 'omr-auto', 'peer-0-address',
                            'peer-0-remove', 'addPeerBtn'].map((id) => t.el(id).disabled);
            await p; await t.flush();
            const after = ['holdTime', 'customPrefix', 'method-pd', 'peer-0-address'].map((id) => t.el(id).disabled);
            return [during, after];
        """
        )
        assert r[0] == [True] * 8
        assert r[1] == [False] * 4

    def test_typed_custom_prefix_survives_a_refresh_behind_the_automatic_card(self):
        # type a custom prefix, press Automatic, refresh: the prefix is still there when switching back to Custom
        r = scenario(
            page("routing", routes(PD_STATE))
            + START
            + """
            t.el('omr-custom').click();
            t.el('customPrefix').type('2001:db8:9::/64');
            t.el('omr-auto').click();
            await t.advance(5000);
            t.el('omr-custom').click();
            return [t.el('customPrefix').value, t.el('barText').textContent];
        """
        )
        assert r == ["2001:db8:9::/64", "2 unsaved changes"]
