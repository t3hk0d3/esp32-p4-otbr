"""tools/trel_peer/flash_c6.sh: the example is built from a clean copy, even after a build inside the IDF tree.

A fake IDF tree and a stub idf.py in a temp directory; nothing is built or flashed.
"""

import os
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
FLASH_C6 = ROOT / "tools" / "trel_peer" / "flash_c6.sh"
LEFTOVERS = ["build", "sdkconfig", "dependencies.lock", "managed_components"]

# Records the files the build sees in its working directory, then succeeds.
STUB_IDF = '#!/bin/sh\nls -A > "$IDF_SEEN"\n'


@pytest.fixture
def idf(tmp_path):
    example = tmp_path / "idf" / "examples" / "openthread" / "ot_rcp"
    (example / "main").mkdir(parents=True)
    (example / "CMakeLists.txt").write_text("")
    (example / "sdkconfig.defaults").write_text("")
    (example / "build").mkdir()
    (example / "managed_components").mkdir()
    (example / "sdkconfig").write_text("")
    (example / "dependencies.lock").write_text(
        "dependencies:\n  cmd_ieee802154_debug:\n    source:\n"
        "      path: ../../ieee802154/components/cmd_ieee802154_debug\n"
    )
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    stub = bin_dir / "idf.py"
    stub.write_text(STUB_IDF)
    stub.chmod(0o755)
    return tmp_path


def flash(idf, example):
    env = dict(
        os.environ,
        IDF_PATH=str(idf / "idf"),
        IDF_SEEN=str(idf / "seen"),
        TMPDIR=str(idf / "tmp"),
        PATH=f"{idf / 'bin'}:{os.environ['PATH']}",
    )
    (idf / "tmp").mkdir(exist_ok=True)
    return subprocess.run(
        [str(FLASH_C6), example, "/dev/null"],
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )


def test_build_leftovers_of_the_idf_tree_are_not_copied(idf):
    reply = flash(idf, "rcp")
    assert reply.returncode == 0, reply.stderr
    seen = (idf / "seen").read_text().split()
    assert "CMakeLists.txt" in seen
    for leftover in LEFTOVERS:
        assert leftover not in seen


def test_the_idf_tree_is_left_alone(idf):
    flash(idf, "rcp")
    example = idf / "idf" / "examples" / "openthread" / "ot_rcp"
    for leftover in LEFTOVERS:
        assert (example / leftover).exists()


def test_an_unknown_example_is_refused(idf):
    reply = flash(idf, "ncp")
    assert reply.returncode == 2
    assert "usage" in reply.stderr
