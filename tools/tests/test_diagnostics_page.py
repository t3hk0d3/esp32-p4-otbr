"""Diagnostics page (BDD: DiagnosticsLogic, DiagnosticsPage). Runs static/diagnostics.js under node."""

from device_js import logic, needs_node, page, scenario


def diag(expression):
    return logic("diagnostics", expression)


ROUTES = "(u) => ({status: 200, body: u === '/device/diag' ? H.DIAG : H.NETWORK})"


@needs_node
class TestDiagnosticsLogic:
    """DiagnosticsLogic pure functions"""

    def test_tasks_are_sorted_by_ten_second_load_descending(self):
        # On three tasks: the busiest by 10 s load first; equal loads by name.
        tasks = "[{name:'b',cpu_pct:{'10s':1}},{name:'a',cpu_pct:{'10s':1}},{name:'c',cpu_pct:{'10s':5}}]"
        assert diag(f"L.sortedTasks({tasks}).map(t => t.name)") == ["c", "a", "b"]

    def test_sorting_leaves_the_input_unchanged(self):
        # On sortedTasks: the given array keeps its order.
        assert (
            diag(
                "(() => { const a = [{name:'x',cpu_pct:{'10s':0}},{name:'y',cpu_pct:{'10s':9}}]; L.sortedTasks(a); return a[0].name; })()"
            )
            == "x"
        )

    def test_stack_below_one_kilobyte_is_low(self):
        # On 1023 bytes: low; on 1024: not.
        assert diag("[L.stackLow(1023), L.stackLow(1024)]") == [True, False]

    def test_microseconds_read_as_the_largest_fitting_unit(self):
        # On 500 / 1758 / 263196 / 2500000 us: µs / ms with one decimal / s with one decimal.
        assert diag("[500, 1758, 263196, 2500000].map(L.usText)") == [
            "500 µs",
            "1.8 ms",
            "263.2 ms",
            "2.5 s",
        ]


@needs_node
class TestDiagnosticsPage:
    """diagnostics.js run against a fake DOM and fetch"""

    def test_figures_are_filled_from_diag(self):
        # On a diag answer: uptime in words, reset reason, a row per core and per memory region.
        r = scenario(
            page("diagnostics", ROUTES)
            + """
            await t.page.refresh();
            return [t.el('d-uptime').textContent, t.el('d-reset').textContent,
                    t.el('d-cpu').allText(), t.el('d-mem').allText(), t.el('d-note').textContent];
        """
        )
        assert r[0] == "7 h 35 min"
        assert r[1] == "software"
        assert "Core 0" in r[2] and "0.4% / 0.5% / 0.4%" in r[2] and "Core 1" in r[2]
        assert (
            "Internal RAM" in r[3]
            and "408.9 KB" in r[3]
            and "PSRAM" in r[3]
            and "30.2 MB" in r[3]
        )
        assert r[4] == ""

    def test_cpu_bar_is_the_ten_second_load(self):
        # On core 0 at 0.5 % over 10 s: its bar is 0.5 % wide.
        r = scenario(
            page("diagnostics", ROUTES)
            + "await t.page.refresh(); const core0 = t.el('d-cpu').children[0];"
            + "const find = (e) => e.classes.has('bar-fill') ? e : e.children.map(find).find(Boolean);"
            + "return find(core0).style.width;"
        )
        assert r == "0.5%"

    def test_lock_table_has_boot_and_last_minute_rows(self):
        # On the ot_lock figures: two rows with acquisitions, hold, holder and wait.
        r = scenario(
            page("diagnostics", ROUTES)
            + "await t.page.refresh(); return t.el('d-lock').children.map((tr) => tr.allText());"
        )
        assert len(r) == 2
        assert (
            "Since boot" in r[0]
            and "416546" in r[0]
            and "263.2 ms" in r[0]
            and "ot_main" in r[0]
        )
        assert "Last 60 s" in r[1] and "910" in r[1] and "1.8 ms" in r[1]

    def test_tasks_are_listed_busiest_first_with_low_stack_marked(self):
        # On three tasks: IDLE0 first; ipc0 (508 B) has its stack cell marked, httpd not.
        r = scenario(
            page("diagnostics", ROUTES)
            + """
            await t.page.refresh();
            const rows = t.el('d-tasks').children;
            const stack = (tr) => tr.children[tr.children.length - 1];
            return [rows.map((tr) => tr.children[0].textContent),
                    rows.map((tr) => stack(tr).classes.has('stack-low')), stack(rows[2]).textContent];
        """
        )
        assert r == [["IDLE0", "httpd", "ipc0"], [False, False, True], "508 B"]

    def test_untracked_tasks_are_mentioned(self):
        # On tasks_untracked 3: the note says 3 more tasks are not listed.
        r = scenario(
            page(
                "diagnostics",
                "(u) => ({status: 200, body: Object.assign(H.fixture(H.DIAG), {tasks_untracked: 3})})",
            )
            + "await t.page.refresh(); return t.el('d-note').textContent;"
        )
        assert "3 more tasks" in r

    def test_failure_shows_a_note_and_keeps_the_last_figures(self):
        # On a good answer then a 503: the note names the problem, the uptime stays.
        r = scenario(
            page(
                "diagnostics",
                "(u) => t.fail ? ({status: 503, body: {error: 'busy', detail: 'x'}}) : ({status: 200, body: H.DIAG})",
            )
            + """
            await t.page.refresh(); t.fail = true; const ok = await t.page.refresh();
            return [ok, t.el('d-note').textContent, t.el('d-uptime').textContent];
        """
        )
        assert r[0] is False
        assert "Not available now" in r[1]
        assert r[2] == "7 h 35 min"

    def test_server_text_is_rendered_as_text(self):
        # On a task name with markup: it lands in textContent unchanged.
        r = scenario(
            page(
                "diagnostics",
                "(u) => { const d = H.fixture(H.DIAG); d.tasks[0].name = '<b>x</b>'; return {status: 200, body: d}; }",
            )
            + "await t.page.refresh(); return t.el('d-tasks').children[0].children[0].textContent;"
        )
        assert r == "<b>x</b>"

    def test_init_polls_diag_and_the_trial_banner(self):
        # On init: GET /device/diag and GET /device/network are sent.
        r = scenario(
            page("diagnostics", ROUTES)
            + "t.page.init(); await t.advance(100); return [...new Set(t.calls.map((c) => c.url))].sort();"
        )
        assert r == ["/device/diag", "/device/network"]
