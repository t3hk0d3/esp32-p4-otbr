"""Leader card of the Network page (BDD: LeaderLogic, LeaderPage). Runs static/leader.js under node."""

import json
import re

from device_js import logic, needs_node, page, scenario


def view(thread, node, pending=None):
    return logic(
        "leader",
        f"L.view({json.dumps(thread)}, {json.dumps(node)}, {json.dumps(pending)})",
    )


def thread(adj=0, weight=None, base=65, **extra):
    w = base + adj if weight is None else weight
    return {
        "leader_weight_adjustment": adj,
        "leader_weight": w,
        "leader_weight_base": base,
    } | extra


def node(state="router", weighting=65, router_id=11):
    return {
        "state": state,
        "leaderData": {
            "weighting": weighting,
            "leaderRouterId": router_id,
            "partitionId": 1,
        },
    }


LOWER_HINT = (
    "Needs a weight above 65 (the leader’s). Raise the adjustment to +1 or more."
)


@needs_node
class TestLeaderLogic:
    """view, leaderName"""

    def test_lower_weight_keeps_the_button_disabled(self):
        # On a router, own 61, leader 65: verdict lower, button disabled, raise-the-adjustment hint.
        v = view(thread(adj=-4), node())
        assert v["verdictKind"] == "bad"
        assert v["verdict"] == "Lower than the leader: the leader keeps the role."
        assert v["buttonEnabled"] is False
        assert v["hint"] == LOWER_HINT

    def test_equal_weight_keeps_the_button_disabled(self):
        # On own 65, leader 65: verdict equal, button disabled, same hint.
        v = view(thread(adj=0), node())
        assert v["verdictKind"] == "warn"
        assert (
            v["verdict"]
            == "Equal: the next merge is decided by the random partition ID."
        )
        assert v["buttonEnabled"] is False
        assert v["hint"] == LOWER_HINT

    def test_higher_weight_enables_the_button(self):
        # On own 73, leader 65: verdict higher, button enabled, takes-over hint.
        v = view(thread(adj=8), node())
        assert v["verdictKind"] == "good"
        assert v["verdict"] == "Higher than the leader: this board wins the next merge."
        assert v["buttonEnabled"] is True
        assert v["hint"] == "Takes over now. The mesh reattaches for a few seconds."
        assert (v["ownWeight"], v["leaderWeight"], v["base"], v["adjustment"]) == (
            73,
            65,
            65,
            8,
        )

    def test_the_leader_board_has_nothing_to_take_over(self):
        # On state leader: verdict, button disabled, "Already the leader.", leader shown as this board.
        v = view(thread(adj=8), node("leader", 73))
        assert v["verdict"] == "This board is the leader."
        assert v["buttonEnabled"] is False
        assert v["hint"] == "Already the leader."
        assert v["leaderName"] == "this board"
        assert v["role"] == "leader"

    def test_the_leader_board_shows_the_partitions_weight(self):
        # On state leader with the adjustment changed (own 73, partition still 65): leader weight 65 from /node.
        v = view(thread(adj=8), node("leader", 65))
        assert v["leaderWeight"] == 65
        assert v["ownWeight"] == 73

    def test_the_leader_board_falls_back_to_its_own_weight(self):
        # On state leader without leaderData.weighting: the leader weight is the board's own.
        n = {"state": "leader", "leaderData": {}}
        assert view(thread(adj=8), n)["leaderWeight"] == 73

    def test_an_unsaved_slider_value_asks_to_save_first(self):
        # On a pending value other than the saved one: dirty, Save enabled, button disabled, save-first hint.
        v = view(thread(adj=8), node(), pending=3)
        assert v["dirty"] is True
        assert v["saveEnabled"] is True
        assert v["buttonEnabled"] is False
        assert v["hint"] == "Save the adjustment first."
        same = view(thread(adj=8), node(), pending=8)
        assert same["dirty"] is False and same["saveEnabled"] is False

    def test_a_leader_above_the_boards_maximum_says_so(self):
        # On leader 82 and base 65 (maximum 81): no suggestion of an adjustment that does not exist.
        v = view(thread(adj=16), node(weighting=82))
        assert v["buttonEnabled"] is False
        assert (
            v["hint"]
            == "The leader’s weight 82 is above the most this board can reach (81)."
        )

    def test_a_detached_board_hides_the_comparison(self):
        # On node state detached or disabled: comparison hidden, button disabled, not-attached hint.
        for state in ["detached", "disabled"]:
            v = view(thread(adj=8), node(state))
            assert v["comparisonShown"] is False, state
            assert v["buttonEnabled"] is False, state
            assert v["hint"] == "This board is not attached to a Thread network.", state

    def test_a_failed_node_read_keeps_save_working(self):
        # On GET /node failed (null): comparison hidden, button disabled, "Thread is not running.", Save when dirty.
        v = view(thread(adj=8), None, pending=2)
        assert v["comparisonShown"] is False
        assert v["buttonEnabled"] is False
        assert v["hint"] == "Thread is not running."
        assert v["saveEnabled"] is True
        assert v["adjustment"] == 8

    def test_a_partial_reply_hides_the_comparison(self):
        # On a thread reply without weights (partial): comparison hidden, button disabled.
        v = view({"leader_weight_adjustment": 8, "partial": True}, node())
        assert v["comparisonShown"] is False
        assert v["buttonEnabled"] is False

    def test_the_scale_runs_from_48_to_96_and_is_clamped(self):
        # On 48, 96, 40, 100: 0 %, 100 %, 0 %, 100 %.
        got = [
            view(thread(weight=w, adj=0), node(weighting=w))["scaleOwn"]
            for w in [48, 96, 40, 100]
        ]
        assert got == [0, 100, 0, 100]
        v = view(thread(adj=8), node(weighting=65))
        assert v["scaleLeader"] == (65 - 48) / 48 * 100

    def test_leader_name(self):
        # On state leader: "this board"; on leaderRouterId 11: "router 0x2c00".
        assert (
            logic("leader", f"L.leaderName({json.dumps(node('leader'))})")
            == "this board"
        )
        assert logic("leader", f"L.leaderName({json.dumps(node())})") == "router 0x2c00"
        assert (
            logic("leader", f"L.leaderName({json.dumps(node(router_id=0))})")
            == "router 0x0000"
        )


def routes(
    put="{status: 200, body: H.fixture(H.THREAD)}",
    thread="H.fixture(H.THREAD)",
    nd="H.fixture(H.NODE)",
):
    return (
        "(u, o) => u === '/device/thread' ? (o.method === 'PUT' ? ("
        + put
        + ") : ({status: 200, body: "
        + thread
        + "}))"
        " : u === '/node' ? ({status: 200, body: " + nd + "})"
        " : u === '/device/status' ? ({status: 200, body: H.STATUS}) : ({status: 404, body: {}})"
    )


START = "t.page.init(); await t.advance(100);"
READS = "t.calls.filter(c => c.method !== 'PUT' && (c.url === '/device/thread' || c.url === '/node')).length"


@needs_node
class TestLeaderPage:
    """leader.js run against the Network page's fake DOM and fetch"""

    def test_load_renders_both_weights_and_the_breakdown(self):
        # On both reads answering: the weights, "65 base · +8 adjustment", the leader name, the slider position.
        r = scenario(
            page(
                "leader",
                routes(
                    thread="H.fixture(H.THREAD, {leader_weight_adjustment: 8, leader_weight: 73})"
                ),
            )
            + START
            + """
            return [t.el('ld-own-weight').textContent, t.el('ld-own-breakdown').textContent,
                    t.el('ld-leader-weight').textContent, t.el('ld-leader-who').textContent,
                    t.el('ld-adj').value, t.el('ld-comparison').hidden(), t.el('ld-become').disabled,
                    t.el('ld-save').disabled, t.el('ld-role').textContent];
        """
        )
        assert r == [
            "73",
            "65 base · +8 adjustment",
            "65",
            "router 0x2c00",
            "8",
            False,
            False,
            True,
            "router",
        ]

    def test_moving_the_slider_enables_save_and_disables_the_button(self):
        # On a slider moved to +8: Save enabled, label shows the pending weight, Become leader disabled.
        r = scenario(
            page("leader", routes())
            + START
            + """
            t.el('ld-adj').type('8');
            return [t.el('ld-save').disabled, t.el('ld-adj-label').textContent, t.el('ld-become').disabled,
                    t.el('ld-become-hint').textContent];
        """
        )
        assert r[0] is False
        assert "+8" in r[1] and "73" in r[1]
        assert r[2] is True
        assert r[3] == "Save the adjustment first."

    def test_save_sends_the_adjustment_and_shows_the_reply(self):
        # On the slider at +8 and Save: PUT {"leader_weight_adjustment":8}; the card shows the reply.
        r = scenario(
            page(
                "leader",
                routes(
                    put="{status: 200, body: H.fixture(H.THREAD, {leader_weight_adjustment: 8, leader_weight: 73, applied: true})}"
                ),
            )
            + START
            + """
            t.el('ld-adj').type('8'); t.el('ld-save').click(); await t.advance(100);
            const put = t.calls.filter(c => c.method === 'PUT');
            return [put.map(c => [c.url, c.body]), t.el('ld-own-weight').textContent,
                    t.el('ld-own-breakdown').textContent, t.el('ld-save').disabled, t.el('ld-save-msg').textContent];
        """
        )
        assert r[0] == [["/device/thread", '{"leader_weight_adjustment":8}']]
        assert r[1:4] == ["73", "65 base · +8 adjustment", True]
        assert r[4] == "Saved."

    def test_a_refused_save_shows_the_detail_and_keeps_the_slider(self):
        # On a 400 reply: the detail under the slider; the slider keeps the value, Save stays enabled.
        r = scenario(
            page(
                "leader",
                routes(
                    put="{status: 400, body: {error: 'invalid_config', field: 'leader_weight_adjustment', detail: 'must be between -16 and 16'}}"
                ),
            )
            + START
            + """
            t.el('ld-adj').type('8'); t.el('ld-save').click(); await t.advance(100);
            return [t.el('ld-save-msg').textContent, t.el('ld-adj').value, t.el('ld-save').disabled];
        """
        )
        assert "must be between -16 and 16" in r[0]
        assert r[1:] == ["8", False]

    def test_the_button_only_opens_the_confirm_dialog(self):
        # On Become leader: the dialog opens with both weights and names; no request yet.
        r = scenario(
            page(
                "leader",
                routes(
                    thread="H.fixture(H.THREAD, {leader_weight_adjustment: 8, leader_weight: 73})"
                ),
            )
            + START
            + """
            t.el('ld-become').click(); await t.advance(10);
            return [t.el('ld-modal').classes.has('active'), t.calls.filter(c => c.method === 'PUT').length,
                    t.el('ld-modal-weight').textContent, t.el('ld-modal-leader-weight').textContent,
                    t.el('ld-modal-name').textContent, t.el('ld-modal-name-2').textContent];
        """
        )
        assert r == [True, 0, "73", "65", "router 0x2c00", "router 0x2c00"]

    def test_cancel_closes_the_dialog_without_a_request(self):
        # On Cancel: the dialog closes; no PUT.
        r = scenario(
            page(
                "leader",
                routes(
                    thread="H.fixture(H.THREAD, {leader_weight_adjustment: 8, leader_weight: 73})"
                ),
            )
            + START
            + """
            t.el('ld-become').click(); t.el('ld-cancel').click(); await t.advance(10);
            return [t.el('ld-modal').classes.has('active'), t.calls.filter(c => c.method === 'PUT').length];
        """
        )
        assert r == [False, 0]

    def test_confirm_starts_the_take_over_and_polls_every_second_until_leader(self):
        # On Confirm and a 200 started: PUT {"become_leader":true}; both reads every 1 s until node state is leader.
        r = scenario(
            "let state = 'router';"
            + page(
                "leader",
                routes(
                    thread="H.fixture(H.THREAD, {leader_weight_adjustment: 8, leader_weight: 73})",
                    put="{status: 200, body: H.fixture(H.THREAD, {leader_weight_adjustment: 8, leader_weight: 73, applied: true, become_leader: 'started'})}",
                    nd="H.fixture(H.NODE, {state})",
                ),
            )
            + START
            + """
            t.el('ld-become').click(); t.el('ld-confirm').click(); await t.advance(100);
            const put = t.calls.filter(c => c.method === 'PUT').map(c => c.body);
            const before = @READS@;
            await t.advance(3000);
            const during = @READS@ - before;
            state = 'leader'; await t.advance(1100);
            const atLeader = @READS@;
            await t.advance(3000);
            return [put, during >= 6 && during <= 8, @READS@ - atLeader <= 2, t.el('ld-verdict').textContent,
                    t.el('ld-become-msg').textContent, t.el('ld-role').textContent];
        """.replace("@READS@", READS)
        )
        assert r[0] == ['{"become_leader":true}']
        assert r[1] is True
        assert (
            r[2] is True
        )  # the 1 s polling stopped once the node was leader: only the 5 s poll is left
        assert r[3] == "This board is the leader."
        assert r[5] == "leader"

    def test_the_take_over_polling_gives_up_after_30_seconds(self):
        # On a board that never becomes leader: the 1 s polling stops after 30 s with a message.
        r = scenario(
            page(
                "leader",
                routes(
                    thread="H.fixture(H.THREAD, {leader_weight_adjustment: 8, leader_weight: 73})",
                    put="{status: 200, body: H.fixture(H.THREAD, {leader_weight_adjustment: 8, leader_weight: 73, become_leader: 'started'})}",
                ),
            )
            + START
            + """
            t.el('ld-become').click(); t.el('ld-confirm').click(); await t.advance(31000);
            const n = @READS@; await t.advance(3000);
            return [t.el('ld-become-msg').textContent, @READS@ - n < 4];
        """.replace("@READS@", READS)
        )
        assert "30" in r[0]
        assert r[1] is True

    def test_a_stale_page_shows_the_detail_and_refreshes_both_reads(self):
        # On Confirm answered 409 weight not higher: the detail next to the button; both reads again at once.
        r = scenario(
            page(
                "leader",
                routes(
                    thread="H.fixture(H.THREAD, {leader_weight_adjustment: 8, leader_weight: 73})",
                    put="{status: 409, body: {error: 'cannot_take_over', detail: 'weight not higher'}}",
                ),
            )
            + START
            + """
            const n = @READS@;
            t.el('ld-become').click(); t.el('ld-confirm').click(); await t.advance(100);
            return [t.el('ld-become-msg').textContent, @READS@ - n, t.el('ld-modal').classes.has('active'),
                    t.calls.filter(c => c.url === '/node').length];
        """.replace("@READS@", READS)
        )
        assert "weight not higher" in r[0]
        assert r[1] == 2
        assert r[2] is False

    def test_a_failed_node_read_still_shows_the_adjustment_and_saves(self):
        # On GET /node 503 while /device/thread answers: adjustment shown, comparison hidden, button off, Save works.
        r = scenario(
            "const base = "
            + routes(
                put="{status: 200, body: H.fixture(H.THREAD, {leader_weight_adjustment: 3, leader_weight: 68, applied: true})}",
                thread="H.fixture(H.THREAD, {leader_weight_adjustment: 8, leader_weight: 73})",
            )
            + ";"
            + page(
                "leader",
                "(u, o) => u === '/node' ? {status: 503, body: {}} : base(u, o)",
            )
            + START
            + """
            const shown = [t.el('ld-adj').value, t.el('ld-comparison').hidden(), t.el('ld-become').disabled,
                           t.el('ld-become-hint').textContent];
            t.el('ld-adj').type('3'); t.el('ld-save').click(); await t.advance(100);
            return [shown, t.calls.filter(c => c.method === 'PUT').map(c => c.body), t.el('ld-adj').value];
        """
        )
        assert r[0] == ["8", True, True, "Thread is not running."]
        assert r[1] == ['{"leader_weight_adjustment":3}']
        assert r[2] == "3"

    def test_a_leader_above_the_boards_maximum_shows_the_hint(self):
        # On leader 82 against a maximum of 81: the card says so next to the button.
        r = scenario(
            page(
                "leader",
                routes(
                    thread="H.fixture(H.THREAD, {leader_weight_adjustment: 16, leader_weight: 81})",
                    nd="H.fixture(H.NODE, {leaderData: {weighting: 82, leaderRouterId: 11}})",
                ),
            )
            + START
            + """
            return [t.el('ld-become-hint').textContent, t.el('ld-become').disabled];
        """
        )
        assert r == [
            "The leader’s weight 82 is above the most this board can reach (81).",
            True,
        ]

    def test_visible_page_reads_both_every_5_seconds(self):
        # On a visible page: both reads about every 5 s.
        r = scenario(
            page("leader", routes())
            + START
            + """
            const n = @READS@; await t.advance(10500);
            return @READS@ - n;
        """.replace("@READS@", READS)
        )
        assert r == 4

    def test_hidden_page_does_not_read_until_visible_again(self):
        # On a hidden page: no reads; once visible again, a read follows.
        r = scenario(
            page("leader", routes())
            + START
            + """
            t.doc.hidden = true; await t.advance(1000);
            const n = @READS@; await t.advance(20000);
            const hiddenReads = @READS@ - n;
            t.doc.hidden = false; t.doc.fire('visibilitychange'); await t.advance(100);
            return [hiddenReads, @READS@ - n > 0];
        """.replace("@READS@", READS)
        )
        assert r == [0, True]


GUARD_SCENARIO = """
    const mk = (parent, attrs) => {
      const e = {parent, disabled: false, classes: new Set(), attrs: attrs || {},
                 classList: {add: (c) => e.classes.add(c), remove: (c) => e.classes.delete(c)},
                 closest: (sel) => { for (let x = e; x; x = x.parent) if (x.attrs['data-guard-exempt'] !== undefined) return x; return null; }};
      return e;
    };
    const ldCard = mk(null, {'data-guard-exempt': ''});
    const other = mk(null);
    const ldAdj = mk(ldCard), ldSave = mk(ldCard), otherBtn = mk(other);
    const controls = [ldAdj, ldSave, otherBtn];
    globalThis.document = {
      querySelector: () => ({insertBefore() {}, firstChild: null}),
      getElementById: () => null,
      createElement: () => ({appendChild() {}, setAttribute() {}, firstChild: null}),
      createTextNode: () => ({}),
      querySelectorAll: (sel) => sel.includes('button') ? controls : sel === '.card' ? [ldCard, other]
                                 : sel === '.card-disabled' ? [ldCard, other].filter((c) => c.classes.has('card-disabled')) : []
    };
    globalThis.fetch = async (u) => ({status: 200, json: async () => (u === '/device/status' ? {mode: 'RADIO_FAULT'} : {})});
    globalThis.setTimeout = () => 0;
    const Page = require(@MGMT@).Page;
    await Page.guard(false);
    return [ldAdj.disabled, ldSave.disabled, ldCard.classes.has('card-disabled'),
            otherBtn.disabled, other.classes.has('card-disabled')];
"""


@needs_node
class TestGuardWithTheLeaderCard:
    """mgmt.js guard + the Leader card"""

    def test_the_guard_leaves_the_leader_card_usable(self):
        # On RADIO_FAULT: the card's controls stay enabled and the card is not greyed; every other card is disabled.
        from device_js import STATIC

        r = scenario(GUARD_SCENARIO.replace("@MGMT@", json.dumps(str(STATIC / "mgmt.js"))))
        assert r == [False, False, False, True, True]

    def test_the_network_page_marks_the_card_exempt(self):
        # On network.html: #ld-card carries data-guard-exempt; no other card does.
        from device_js import FRONTEND

        html = (FRONTEND / "network.html").read_text()
        assert re.search(r'id="ld-card"[^>]*\bdata-guard-exempt\b', html)
        assert html.count("data-guard-exempt") == 1
