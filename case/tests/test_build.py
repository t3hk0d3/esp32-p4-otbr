"""p4_case.build (BDD: p4_case.build (CLI)). Builds the real parts once per module (slow)."""

import pytest
from conftest import BOARD_STEP, REPO
from p4_case import build
from p4_case.results import CheckResult

OUTPUTS = (
    "p4_case_base.stl",
    "p4_case_base.step",
    "p4_case_lid.stl",
    "p4_case_lid.step",
)


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    out = tmp_path_factory.mktemp("case")
    import contextlib
    import io

    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        code = build.main(["--board", str(BOARD_STEP), "--out", str(out)])
    return out, code, buf.getvalue()


class TestMainValid:
    """build.main: With a valid board path and an out dir"""

    def test_writes_the_stl_and_step_files(self, built):
        out, _, _ = built
        for name in OUTPUTS:
            assert (out / name).stat().st_size > 1000, name

    def test_writes_the_preview_pngs(self, built):
        out, _, _ = built
        pngs = sorted(p.name for p in out.glob("preview_*.png"))
        assert len(pngs) >= 4
        assert all((out / p).read_bytes()[:4] == b"\x89PNG" for p in pngs)

    def test_prints_one_line_per_check(self, built):
        _, _, text = built
        lines = [ln for ln in text.splitlines() if ln.startswith(("PASS ", "FAIL "))]
        assert len(lines) >= 20

    def test_exits_0_when_all_checks_pass(self, built):
        _, code, _ = built
        assert code == 0


class TestMainCheckFailing:
    """build.main: With --check and a failing check"""

    def test_exits_1(self, tmp_path, monkeypatch, capsys):
        monkeypatch.setattr(
            build, "run_all", lambda *a: [CheckResult("x", False, "broken")]
        )
        monkeypatch.setattr(build, "write_outputs", lambda *a: [])
        code = build.main(
            ["--board", str(BOARD_STEP), "--out", str(tmp_path), "--check"]
        )
        assert code == 1
        assert "FAIL x" in capsys.readouterr().out


class TestMainMissingBoard:
    """build.main: With a missing board file"""

    def test_prints_the_make_case_board_hint_and_exits_2(self, tmp_path, capsys):
        code = build.main(
            ["--board", str(tmp_path / "nope.stp"), "--out", str(tmp_path)]
        )
        assert code == 2
        assert "make case-board" in capsys.readouterr().err


class TestCommittedModels:
    """build.main: The print STLs committed under docs/models"""

    @pytest.mark.parametrize("name", ["p4_case_base.stl", "p4_case_lid.stl"])
    def test_match_a_fresh_build(self, built, name):
        out, _, _ = built
        committed = REPO / "docs" / "models" / name
        assert committed.is_file(), f"{committed} missing: run make case"
        assert committed.read_bytes() == (out / name).read_bytes(), (
            f"{committed} is stale: run make case and commit docs/models/"
        )
