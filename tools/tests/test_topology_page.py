"""Topology page (BDD: TopologyLogic). Runs static/topology.js under node."""

import json

from device_js import logic, needs_node

# The sketch mesh as a JS literal. R: this board 0x8800 is a router. B: the same mesh with this board as a child.
FIXTURE = r"""
const mode = (rx) => ({rx_on_when_idle: rx, full_thread_device: false, full_network_data: false});
const detail = {timeout_s: 240, age_s: 12, connected_s: 86400, rssi_avg: -61, rssi_last: -63, link_margin_db: 39,
                frame_error_pct: 0.4, message_error_pct: 0.0, version: 4, csl: false};
const NET = {name: 'MyThread', channel: 15, partition_id: 1243556871, leader_router_id: 9,
             mesh_local_prefix: 'fdde:ad00:beef::/64', this_rloc16: 0x8800,
             omr_prefixes: ['2001:db8:4a2:1::/64', 'fd7a:91c3:22e0:1::/64']};
const SRP = [{name: 'kitchen-plug', addresses: ['2001:db8:4a2:1:2be4:6c19:a0f7:3381'],
              services: [{instance: 'kitchen-plug', type: '_matter._tcp'}]},
             {name: '<b>"x"</b>', addresses: ['fd7a:91c3:22e0:1::77'], services: []}];
const routers = () => [
  {rloc16: 0x2400, router_id: 9, ext_address: 'd61a7c03f4e2b918', version: 5, leader: true, this_device: false,
   border_router: false, links: [{router_id: 34, lq_in: 3}, {router_id: 23, lq_in: 2}, {router_id: 40, lq_in: 1}],
   addresses: ['fdde:ad00:beef::ff:fe00:2400', 'fdde:ad00:beef::ff:fe00:fc00', 'fe80::d41a:7c03:f4e2:b918'],
   responded: true, children_detail: true,
   children: [{rloc16: 0x2401, ext_address: '1c88f02ab4d6e5a3', mode: mode(false), lq_in: 3, ...detail,
               addresses: ['fdde:ad00:beef::1c88:f02a:b4d6']}]},
  {rloc16: 0x5c00, router_id: 23, ext_address: 'aa00000000005c00', version: 5, leader: false, this_device: false,
   border_router: false, links: [{router_id: 9, lq_in: 2}, {router_id: 34, lq_in: 3}, {router_id: 99, lq_in: 1}],
   addresses: ['fdde:ad00:beef::ff:fe00:5c00'], responded: true, children_detail: false,
   children: [{rloc16: 0x5c02, mode: mode(false), lq_in: 2, addresses: []},
              {rloc16: 0x5c01, mode: mode(true), lq_in: 3, addresses: ['2001:db8:4a2:1::5c01']}]},
  {rloc16: 0x8800, router_id: 34, ext_address: 'bb00000000008800', version: 5, leader: false, this_device: true,
   border_router: true, links: [{router_id: 9, lq_in: 3}, {router_id: 40, lq_in: 2}],
   addresses: ['fdde:ad00:beef::ff:fe00:8800', '2001:db8:4a2:1::1'], responded: true, children_detail: true,
   children: [{rloc16: 0x8801, ext_address: 'c100000000008801', mode: mode(true), lq_in: 3, ...detail,
               addresses: ['2001:db8:4a2:1::8801']},
              {rloc16: 0x8802, ext_address: 'c100000000008802', mode: mode(false), lq_in: 2, ...detail,
               addresses: ['fd7a:91c3:22e0:1::77']},
              {rloc16: 0x8803, ext_address: 'c100000000008803', mode: mode(false), lq_in: 1, ...detail,
               addresses: ['fdde:ad00:beef::8803:1', 'fe80::1']}]},
  {rloc16: 0xa000, router_id: 40, ext_address: 'dd0000000000a000', version: 5, leader: false, this_device: false,
   border_router: false, links: [{router_id: 9, lq_in: 3}, {router_id: 34, lq_in: 2}],
   addresses: ['fdde:ad00:beef::ff:fe00:a000'], responded: true, children_detail: true,
   children: [{rloc16: 0xa001, ext_address: 'e1000000000a0001', mode: mode(false), lq_in: 3, ...detail,
               addresses: ['2001:db8:4a2:1:2be4:6c19:a0f7:3381']}]},
  {rloc16: 0xd400, router_id: 53, ext_address: 'ee0000000000d400', leader: false, this_device: false,
   border_router: false, links: [], addresses: ['fdde:ad00:beef::ff:fe00:d400'], responded: true,
   children_detail: true, children: []},
  {rloc16: 0xdc00, router_id: 55, responded: false}
];
const R = {partial: true, network: NET, routers: routers(), srp_hosts: SRP};
const B = (() => {
  const r = routers();
  r[2].this_device = false; r[2].border_router = false;
  r[3].children.push({rloc16: 0xa002, ext_address: 'e1000000000a0002', mode: mode(true), lq_in: 3, ...detail,
                      addresses: ['2001:db8:4a2:1::a002']});
  return {partial: false, network: {...NET, this_rloc16: 0xa002}, routers: r, srp_hosts: SRP};
})();
const D = L.devices(R), DB = L.devices(B), LK = L.routerLinks(R);
const byId = (ds, id) => ds.find((d) => d.id === id);
const names = (ds, list) => list.map((d) => d.id);
"""


def js(expression):
    """Evaluate `expression` with the fixture (R, B, D, DB, LK, NET, SRP) in scope."""
    return logic("topology", f"(() => {{ {FIXTURE} return ({expression}); }})()")


def plain(expression):
    return logic("topology", expression)


NETWORK = (
    "{mesh_local_prefix: 'fdde:ad00:beef:0::/64', omr_prefixes: ['2001:db8:4a2:1::/64', 'fd7a:91c3:22e0:1::/64']}"
)
STORE = """(() => { const m = {}; return {m, getItem: (k) => (k in m ? m[k] : null),
  setItem: (k, v) => { m[k] = String(v); }, removeItem: (k) => { delete m[k]; }}; })()"""
BROKEN = "{getItem() { throw new Error('denied'); }, setItem() { throw new Error('denied'); }, removeItem() { throw new Error('denied'); }}"


@needs_node
class TestTopologyLogic:
    """TopologyLogic pure functions"""

    # addressClass

    def test_address_under_a_global_omr_prefix_is_global(self):
        # On an address under a 2000::/3 OMR prefix: "global".
        assert plain(f"L.addressClass('2001:db8:4a2:1:2be4:6c19:a0f7:3381', {NETWORK})") == "global"

    def test_address_under_a_ula_omr_prefix_is_ula(self):
        # On an address under an fc00::/7 OMR prefix: "ula".
        assert plain(f"L.addressClass('fd7a:91c3:22e0:1::77', {NETWORK})") == "ula"

    def test_mesh_local_rloc_address_is_rloc(self):
        # On mesh-local with IID 0:ff:fe00:xxxx, xxxx < 0xfc00: "rloc".
        assert plain(f"L.addressClass('fdde:ad00:beef::ff:fe00:5c00', {NETWORK})") == "rloc"

    def test_mesh_local_fc_address_is_aloc(self):
        # On mesh-local with IID 0:ff:fe00:fcxx: "aloc".
        assert plain(f"L.addressClass('fdde:ad00:beef:0:0:ff:fe00:fc00', {NETWORK})") == "aloc"

    def test_other_mesh_local_address_is_mleid(self):
        # On other mesh-local: "mleid".
        assert plain(f"L.addressClass('fdde:ad00:beef::1c88:f02a:b4d6', {NETWORK})") == "mleid"

    def test_fe80_address_is_linklocal(self):
        # On fe80::/10: "linklocal".
        assert plain(f"L.addressClass('fe80::d41a:7c03:f4e2:b918', {NETWORK})") == "linklocal"

    def test_ff00_address_is_multicast(self):
        # On ff00::/8: "multicast".
        assert plain(f"L.addressClass('ff03::1', {NETWORK})") == "multicast"

    def test_unknown_address_is_other(self):
        # Otherwise: "other" (also for text that is not an address).
        assert plain(f"[L.addressClass('2a00:1::1', {NETWORK}), L.addressClass('nonsense', {NETWORK})]") == [
            "other",
            "other",
        ]

    # devices

    def test_devices_lists_every_router_stub_and_child_with_kind_and_parent(self):
        # On a result: one device per router, stub and child, each with kind, parent id and display fields.
        got = js("D.map((d) => [d.id, d.kind, d.parentId, d.rloc16])")
        assert got == [
            ["r9", "leader", None, 0x2400],
            ["c9217", "sed", "r9", 0x2401],
            ["r23", "router", None, 0x5C00],
            ["c23553", "med", "r23", 0x5C01],
            ["c23554", "sed", "r23", 0x5C02],
            ["r34", "br", None, 0x8800],
            ["c34817", "med", "r34", 0x8801],
            ["c34818", "sed", "r34", 0x8802],
            ["c34819", "sed", "r34", 0x8803],
            ["r40", "router", None, 0xA000],
            ["c40961", "sed", "r40", 0xA001],
            ["r53", "router", None, 0xD400],
            ["r55", "stub", None, 0xDC00],
        ]

    def test_devices_flags_this_board_and_keeps_stub_and_missing_fields_unknown(self):
        # On a result: only the board router is flagged; a stub has no ext/addresses/version; detail follows the data.
        got = js(
            "[D.filter((d) => d.thisBoard).map((d) => d.id), byId(D, 'r55').ext, byId(D, 'r55').responded,"
            " byId(D, 'r55').addrs, byId(D, 'r53').version, byId(D, 'r9').version,"
            " byId(D, 'r23').detail, byId(D, 'r34').detail, byId(D, 'c23554').ext, byId(D, 'c23554').detail,"
            " byId(D, 'c34818').detail, byId(D, 'c34818').child.rssi_avg, byId(D, 'c34818').sleepy]"
        )
        assert got == [["r34"], None, False, [], None, 5, False, True, None, False, True, -61, True]

    def test_devices_marks_the_child_matching_this_rloc16_when_no_router_is_this_device(self):
        # On this board being a child (no this_device router): marks the child with rloc16 == this_rloc16 as this board.
        got = js("[DB.filter((d) => d.thisBoard).map((d) => [d.id, d.kind, d.parentId]), DB.filter((d) => d.kind === 'br').length]")
        assert got == [[["c40962", "br", "r40"]], 1]

    # routerLinks

    def test_routerlinks_pairs_two_sides_with_the_lower_id_first(self):
        # On two routers that list each other: one link, a = lower id, lqAB = b's lq_in for a, lqBA = a's lq_in for b.
        got = js("LK.filter((l) => l.a === 'r9' && l.b === 'r34')")
        # r34 lists r9 with 3 (lqAB); r9 lists r34 with 3 (lqBA)
        assert got == [{"a": "r9", "b": "r34", "lqAB": 3, "lqBA": 3}]
        got = js("LK.filter((l) => l.a === 'r9' && l.b === 'r40')")
        assert got == [{"a": "r9", "b": "r40", "lqAB": 3, "lqBA": 1}]

    def test_routerlinks_fills_a_missing_side_with_zero(self):
        # On a link listed by one side only: one link with the missing side 0.
        got = js("LK.filter((l) => l.a === 'r23' && l.b === 'r34')")
        assert got == [{"a": "r23", "b": "r34", "lqAB": 0, "lqBA": 3}]

    def test_routerlinks_drops_a_link_to_an_unknown_router(self):
        # On a link to a router id that is in no router entry: no link.
        got = js("[LK.some((l) => l.a === 'r99' || l.b === 'r99'), LK.length]")
        assert got == [False, 5]

    # linkText

    def test_linktext_is_one_number_on_equal_sides(self):
        # On equal sides: "3".
        assert plain("L.linkText({a: 'r1', b: 'r2', lqAB: 3, lqBA: 3})") == "3"

    def test_linktext_shows_a_first_on_different_sides(self):
        # On different sides: "3/2" (a's view first).
        assert plain("L.linkText({a: 'r1', b: 'r2', lqAB: 3, lqBA: 2})") == "3/2"

    # displayName

    def test_displayname_prefers_the_nickname_for_the_ext_address(self):
        # On a nickname for the ext address: the nickname.
        got = js("L.displayName(byId(D, 'c40961'), {e1000000000a0001: 'Hall light'}, SRP)")
        assert got == "Hall light"

    def test_displayname_uses_the_srp_host_sharing_an_address(self):
        # On an SRP host sharing an address: the host name.
        assert js("L.displayName(byId(D, 'c40961'), {}, SRP)") == "kitchen-plug"

    def test_displayname_falls_back_to_unnamed_rloc16(self):
        # Otherwise: "Unnamed 0xNNNN".
        assert js("L.displayName(byId(D, 'c23553'), {}, SRP)") == "Unnamed 0x5c01"

    def test_displayname_of_this_board_is_its_srp_or_nickname_not_a_map_label(self):
        # On this board: the usual name chain applies (the map label is the page's concern).
        assert js("L.displayName(byId(D, 'r34'), {}, SRP)") == "Unnamed 0x8800"

    # rows

    def test_rows_all_lists_the_board_its_children_then_routers_with_children(self):
        # On "all": this board, its children, then each router by router id followed by its children.
        got = js("L.rows(D, 'all', '', {}, SRP).map((d) => d.id)")
        assert got == [
            "r34", "c34817", "c34818", "c34819",
            "r9", "c9217",
            "r23", "c23553", "c23554",
            "r40", "c40961",
            "r53", "r55",
        ]  # fmt: skip

    def test_rows_routers_keeps_only_routers_in_the_same_order(self):
        # On "routers": only routers, stubs, the leader and the board, same order.
        assert js("L.rows(D, 'routers', '', {}, SRP).map((d) => d.id)") == ["r34", "r9", "r23", "r40", "r53", "r55"]

    def test_rows_end_keeps_only_children_in_the_same_order(self):
        # On "end": only children, same order.
        assert js("L.rows(D, 'end', '', {}, SRP).map((d) => d.id)") == [
            "c34817", "c34818", "c34819", "c9217", "c23553", "c23554", "c40961",
        ]  # fmt: skip

    def test_rows_sleepy_keeps_only_sleepy_children(self):
        # On "sleepy": only sleepy devices, same order.
        assert js("L.rows(D, 'sleepy', '', {}, SRP).map((d) => d.id)") == [
            "c34818", "c34819", "c9217", "c23554", "c40961",
        ]  # fmt: skip

    def test_rows_puts_a_board_that_is_a_child_under_its_parent(self):
        # On this board being a child: it sits under its parent, in RLOC16 order, with no leading board row.
        got = js("L.rows(DB, 'all', '', {}, SRP).map((d) => d.id)")
        assert got[0] == "r9"
        assert got[got.index("r40") : got.index("r40") + 4] == ["r40", "c40961", "c40962", "r53"]

    def test_rows_query_matches_name_rloc_ext_srp_and_addresses_ignoring_case(self):
        # On a query: keeps rows whose name, RLOC16, ext address, SRP host name or any address contains it.
        got = js(
            "['KITCHEN', '0x5C01', '5c01', 'E1000000000A0001', '2001:db8:4a2:1::8801', 'hall', 'zzz']"
            ".map((q) => L.rows(D, 'all', q, {c100000000008801: 'Hall light'}, SRP).map((d) => d.id))"
        )
        assert got == [["c40961"], ["c23553"], ["c23553"], ["c40961"], ["c34817"], ["c34817"], []]

    def test_rows_query_matches_a_name_that_is_markup_literally(self):
        # On a query: the SRP host name is compared as plain text.
        assert js("L.rows(D, 'all', '<b>', {}, SRP).map((d) => d.id)") == ["c34818"]

    # layout

    def test_layout_gives_identical_output_twice(self):
        # On the same input twice: the same coordinates.
        got = js("JSON.stringify(L.layout(D, LK)) === JSON.stringify(L.layout(D, LK))")
        assert got is True

    def test_layout_puts_this_board_at_the_centre(self):
        # On any input: this board is the centre the other routers surround.
        others = js(
            "(() => { const m = L.layout(D, LK); const c = m.pos.r34;"
            " return ['r9', 'r23', 'r40', 'r53', 'r55'].map((id) => Math.hypot(m.pos[id].x - c.x, m.pos[id].y - c.y)); })()"
        )
        assert all(d > 60 for d in others)

    def test_layout_centres_the_leader_when_this_board_is_a_child(self):
        # On this board being a child: the leader is the centre (nearest to the middle of the routers).
        got = js(
            "(() => { const m = L.layout(DB, L.routerLinks(B)); const ids = ['r23', 'r34', 'r40', 'r53', 'r55'];"
            " const c = m.pos.r9; const xs = Object.keys(m.pos).map((k) => m.pos[k].x);"
            " const cx = (Math.min(...xs) + Math.max(...xs)) / 2;"
            " return [ids.every((id) => Math.hypot(m.pos[id].x - c.x, m.pos[id].y - c.y) > 60), m.pos.c40962 !== undefined]; })()"
        )
        assert got == [True, True]

    def test_layout_keeps_every_child_within_its_parents_fan(self):
        # On any input: every child is within the fan radius of its parent.
        got = js(
            "(() => { const m = L.layout(D, LK); return D.filter((d) => d.parentId)"
            ".map((d) => Math.hypot(m.pos[d.id].x - m.pos[d.parentId].x, m.pos[d.id].y - m.pos[d.parentId].y)); })()"
        )
        assert len(got) == 7
        assert all(40 <= d <= 100 for d in got)

    def test_layout_never_goes_below_the_minimum_size_and_grows_with_the_node_count(self):
        # On any input: at least 640x420, positions inside the map, and a bigger mesh gives a bigger map.
        got = js(
            "(() => { const small = L.layout(D.slice(0, 1), []); const m = L.layout(D, LK);"
            " const many = []; for (let i = 0; i < 16; i++) { many.push({id: 'r' + i, kind: 'router', rloc16: i << 10,"
            " routerId: i, parentId: null, thisBoard: i === 0, leader: false, addrs: []}); }"
            " const big = L.layout(many, []);"
            " const inside = Object.keys(m.pos).every((k) => m.pos[k].x >= 0 && m.pos[k].x <= m.width && m.pos[k].y >= 0 && m.pos[k].y <= m.height);"
            " return [small.width, small.height, m.width >= 640, m.height >= 420, inside, big.width > m.width && big.height > m.height]; })()"
        )
        assert got == [640, 420, True, True, True, True]

    def test_layout_of_no_devices_is_the_minimum_empty_map(self):
        # On no devices: 640x420 and no positions.
        assert plain("L.layout([], [])") == {"width": 640, "height": 420, "pos": {}}

    def test_source_has_no_random_numbers(self):
        # On any input: the layout does not use Math.random.
        from device_js import STATIC

        assert "Math.random" not in (STATIC / "topology.js").read_text()

    # savedResult / saveResult

    def test_savedresult_returns_the_stored_result_and_time(self):
        # On a valid topology:last: {savedAt, result}.
        got = js(
            f"(() => {{ const s = {STORE}; L.saveResult(s, R, 1700000000000); const v = L.savedResult(s);"
            " return [v.savedAt, v.result.routers.length, JSON.parse(s.m['topology:last']).saved_at]; })()"
        )
        assert got == [1700000000000, 6, 1700000000000]

    def test_savedresult_is_null_for_missing_garbage_old_shape_or_throwing_storage(self):
        # On missing, unparsable, wrong-shaped data or a throwing storage: null.
        got = js(
            f"[{STORE}, ...['not json', '{{}}', '[]', 'null', '42', '{{\"saved_at\": 1, \"result\": 5}}',"
            " '{\"saved_at\": \"x\", \"result\": {\"routers\": [], \"network\": {}}}',"
            " '{\"savedAt\": 1, \"result\": {\"routers\": [], \"network\": {}}}',"
            " JSON.stringify({routers: [], network: {}}), 'null'].map((raw) => ({getItem: () => raw}))]"
            ".map((s) => L.savedResult(s))"
        )
        assert got == [None] * 11
        assert js(f"L.savedResult({BROKEN})") is None

    def test_saveresult_returns_false_when_storage_throws(self):
        # On a throwing storage: false, no exception.
        assert js(f"[L.saveResult({BROKEN}, R, 1), L.saveResult({STORE}, R, 1)]") == [False, True]

    # names

    def test_savename_stores_and_loadnames_reads_the_nickname(self):
        # On a nickname: stored under the ext address, returns true.
        got = js(f"(() => {{ const s = {STORE}; return [L.saveName(s, 'aa11', ' Hall light '), L.loadNames(s)]; }})()")
        assert got == [True, {"aa11": "Hall light"}]

    def test_savename_with_an_empty_name_removes_the_nickname(self):
        # On an empty name: the nickname is removed; the others stay.
        got = js(
            f"(() => {{ const s = {STORE}; L.saveName(s, 'aa11', 'A'); L.saveName(s, 'bb22', 'B');"
            " const r = L.saveName(s, 'aa11', '  '); return [r, L.loadNames(s)]; })()"
        )
        assert got == [True, {"bb22": "B"}]

    def test_loadnames_is_empty_for_missing_garbage_or_throwing_storage(self):
        # On missing, unparsable or throwing storage: {}.
        got = js(
            f"[L.loadNames({STORE}), L.loadNames({{getItem: () => 'oops'}}), L.loadNames({{getItem: () => '[1]'}}),"
            f" L.loadNames({BROKEN})]"
        )
        assert got == [{}, {}, {}, {}]

    def test_savename_returns_false_when_storage_throws(self):
        # On a throwing storage: false, no exception.
        assert js(f"L.saveName({BROKEN}, 'aa11', 'x')") is False

    # pickLast

    def test_picklast_prefers_a_newer_board_result(self):
        # On a board result newer than the saved one: the board result, saved at nowMs - age_s*1000.
        got = js(
            "(() => { const v = L.pickLast({savedAt: 1000000, result: B}, {state: 'done', age_s: 60, result: R}, 2000000);"
            " return [v.savedAt, v.result === R]; })()"
        )
        assert got == [1940000, True]

    def test_picklast_keeps_the_saved_result_otherwise(self):
        # Otherwise: the saved one (or null).
        got = js(
            "[L.pickLast({savedAt: 1990000, result: B}, {state: 'done', age_s: 60, result: R}, 2000000).result === B,"
            " L.pickLast({savedAt: 5, result: B}, {state: 'running'}, 2000000).savedAt,"
            " L.pickLast({savedAt: 5, result: B}, {state: 'failed', age_s: 1}, 2000000).savedAt,"
            " L.pickLast(null, {state: 'idle'}, 2000000),"
            " L.pickLast(null, {state: 'done', age_s: 0, result: R}, 2000000).savedAt]"
        )
        assert got == [True, 5, 5, None, 2000000]

    # conflictText

    def test_conflicttext_names_the_mode_when_the_network_is_not_running(self):
        # On 409 with detail "mode X": "The Thread network isn't running (mode X)".
        assert plain("L.conflictText(409, {error: 'busy', detail: 'mode DISABLED'})") == (
            "The Thread network isn't running (mode DISABLED)"
        )

    def test_conflicttext_explains_diagnostics_in_use(self):
        # On 409 with detail "mesh diagnostics are in use": ask to try again.
        assert plain("L.conflictText(409, {error: 'busy', detail: 'mesh diagnostics are in use'})") == (
            "Another mesh scan is running, try again in a few seconds"
        )

    def test_conflicttext_passes_another_detail_through(self):
        # On 409 with another detail: that detail.
        assert plain("L.conflictText(409, {error: 'busy', detail: 'firmware update in progress'})") == (
            "firmware update in progress"
        )

    # pingSummary

    def test_pingsummary_reports_replies_loss_and_round_trip_times(self):
        # On replies: "R of N replies", loss %, min/avg/max ms.
        got = plain(
            "L.pingSummary({replies: [{rtt: 5}, {rtt: 7}, {rtt: 9}], statistics: {sent: 4, received: 3,"
            " loss_percent: 25, rtt_min: 5, rtt_avg: 7, rtt_max: 9}}, false)"
        )
        assert got == ["3 of 4 replies", "Loss 25%", "min/avg/max 5/7/9 ms"]

    def test_pingsummary_adds_the_sleepy_line_for_sleepy_devices(self):
        # On a sleepy device: the sleepy line comes last.
        got = plain("L.pingSummary({replies: [], statistics: {sent: 4, received: 0, loss_percent: 100}}, true)")
        assert got == ["0 of 4 replies", "Loss 100%", "Sleepy devices answer when they next wake up."]

    # clockText / agoText

    def test_clocktext_is_local_time_zero_padded(self):
        # On a time: local HH:MM, zero-padded.
        assert plain("[L.clockText(new Date(2026, 10, 3, 9, 5).getTime()), L.clockText(new Date(2026, 10, 3, 23, 41).getTime())]") == [
            "09:05",
            "23:41",
        ]

    def test_agotext_picks_the_unit_by_age(self):
        # On ages: "just now" under 1 min, "N min ago" under 60 min, "N h ago" under 24 h, else "N d ago".
        got = plain(
            "[0, 59000, 60000, 38 * 60000, 59 * 60000, 60 * 60000, 5 * 3600000, 24 * 3600000, 50 * 3600000, -5000]"
            ".map((age) => L.agoText(1000000000000 - age, 1000000000000))"
        )
        assert got == ["just now", "just now", "1 min ago", "38 min ago", "59 min ago", "1 h ago", "5 h ago", "1 d ago", "2 d ago", "just now"]

    # lqColor

    def test_lqcolor_maps_three_two_one_to_green_amber_red(self):
        # On LQI 3 / 2 / 1 / 0: green, amber, red, grey (four distinct colours).
        got = plain("[3, 2, 1, 0].map(L.lqColor)")
        assert len(set(got)) == 4
        assert json.dumps(got).count("#") == 4
