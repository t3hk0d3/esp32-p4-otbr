"""tools/fetch_board_model.sh (BDD: tools/fetch_board_model.sh). Uses a local file:// URL, never the network."""

import hashlib
import os
import subprocess
import zipfile
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "fetch_board_model.sh"
STEP_TEXT = b"ISO-10303-21;\nEND-ISO-10303-21;\n"


def make_zip(path: Path) -> str:
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("model/ESP32-P4-WIFI6-POE-ETH_3d-20251213.stp", STEP_TEXT)
        z.writestr("model/drawing.pdf", b"%PDF-1.4")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(src: Path, sha: str, out: Path) -> subprocess.CompletedProcess:
    env = dict(
        os.environ,
        BOARD_MODEL_URL=src.as_uri(),
        BOARD_MODEL_SHA256=sha,
        BOARD_MODEL_DIR=str(out),
    )
    return subprocess.run(
        ["bash", str(SCRIPT)], check=False, env=env, capture_output=True, text=True
    )


@pytest.fixture
def source(tmp_path):
    src = tmp_path / "vendor.zip"
    return src, make_zip(src)


class TestNoCachedZip:
    """tools/fetch_board_model.sh: With no cached zip"""

    def test_downloads_the_zip_into_the_board_dir(self, source, tmp_path):
        src, sha = source
        out = tmp_path / "board"
        run(src, sha, out)
        assert (out / "board.zip").read_bytes() == src.read_bytes()

    def test_extracts_the_step_as_board_stp(self, source, tmp_path):
        src, sha = source
        out = tmp_path / "board"
        run(src, sha, out)
        assert (out / "board.stp").read_bytes() == STEP_TEXT

    def test_exits_0(self, source, tmp_path):
        src, sha = source
        assert run(src, sha, tmp_path / "board").returncode == 0


class TestCachedZipMatches:
    """tools/fetch_board_model.sh: With a cached zip whose sha256 matches"""

    def test_does_not_download_again(self, source, tmp_path):
        src, sha = source
        out = tmp_path / "board"
        run(src, sha, out)
        src.unlink()  # a second download would now fail
        result = run(src, sha, out)
        assert "cached" in result.stdout

    def test_exits_0(self, source, tmp_path):
        src, sha = source
        out = tmp_path / "board"
        run(src, sha, out)
        assert run(src, sha, out).returncode == 0


class TestShaMismatch:
    """tools/fetch_board_model.sh: With a zip whose sha256 does not match"""

    WRONG = "0" * 64

    def test_deletes_the_bad_zip(self, source, tmp_path):
        src, _ = source
        out = tmp_path / "board"
        run(src, self.WRONG, out)
        assert not (out / "board.zip").exists()

    def test_prints_the_expected_and_actual_hashes(self, source, tmp_path):
        src, sha = source
        result = run(src, self.WRONG, tmp_path / "board")
        assert self.WRONG in result.stderr
        assert sha in result.stderr

    def test_exits_non_zero(self, source, tmp_path):
        src, _ = source
        assert run(src, self.WRONG, tmp_path / "board").returncode != 0
