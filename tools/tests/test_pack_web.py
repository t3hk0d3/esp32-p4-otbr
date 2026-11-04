"""tools/pack_web.py: the web UI staged gzipped for the SPIFFS image."""

import gzip
import importlib.util
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parent.parent
FRONTEND = TOOLS.parent / "p4" / "components" / "esp_ot_br_server" / "frontend"
# Of the 2 MB web partition: gzipped bytes of the staged frontend.
PACKED_BUDGET_BYTES = 192 * 1024


@pytest.fixture(scope="module")
def pack_web():
    spec = importlib.util.spec_from_file_location("pack_web", TOOLS / "pack_web.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture
def frontend(pack_web, tmp_path):
    out = tmp_path / "out"
    return out, pack_web.pack(FRONTEND, out)


def test_pages_styles_and_scripts_are_stored_gzipped_and_decode_to_the_source(frontend):
    out, _ = frontend
    for src in (p for p in FRONTEND.rglob("*") if p.suffix in {".html", ".css", ".js"}):
        rel = src.relative_to(FRONTEND)
        assert not (out / rel).exists()
        assert gzip.decompress((out / rel.parent / (rel.name + ".gz")).read_bytes()) == src.read_bytes()


def test_other_files_are_copied_as_they_are(pack_web, tmp_path):
    (tmp_path / "src").mkdir()
    (tmp_path / "src" / "slot.txt").write_bytes(b"web_0")
    pack_web.pack(tmp_path / "src", tmp_path / "out")
    assert (tmp_path / "out" / "slot.txt").read_bytes() == b"web_0"


def test_output_is_the_same_on_every_run(pack_web, tmp_path):
    first = pack_web.pack(FRONTEND, tmp_path / "a")
    pack_web.pack(FRONTEND, tmp_path / "b")
    assert all((tmp_path / "a" / n).read_bytes() == (tmp_path / "b" / n).read_bytes() for n in first)


def test_stale_files_of_an_earlier_run_are_removed(pack_web, tmp_path):
    (tmp_path / "out").mkdir()
    (tmp_path / "out" / "gone.js.gz").write_bytes(b"old")
    pack_web.pack(FRONTEND, tmp_path / "out")
    assert not (tmp_path / "out" / "gone.js.gz").exists()


def test_a_name_too_long_for_spiffs_is_refused(pack_web, tmp_path):
    (tmp_path / "src").mkdir()
    (tmp_path / "src" / ("x" * 30 + ".js")).write_text("1")
    with pytest.raises(SystemExit):
        pack_web.pack(tmp_path / "src", tmp_path / "out")


def test_packed_frontend_stays_within_the_web_image_budget(frontend):
    _, sizes = frontend
    assert sum(sizes.values()) < PACKED_BUDGET_BYTES
