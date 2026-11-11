"""tools/build_all.sh: which idf.py calls it makes, with what build dir, sdkconfig, defaults and ids.

Runs the script in a temporary tree with a fake idf.py on PATH that records its arguments.
"""

import os
import re
import shutil
import stat
import subprocess
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
FAKE_IDF = """#!/usr/bin/env bash
printf '%s\\n' "$*" >> "$IDF_LOG"
"""
DEFAULT_BUILD = "p4/build"
REV3_BUILD = "p4/build_ESP32-P4-WIFI6-POE-ETH_ESP32-P4-rev3.0"
STAMP = re.compile(r"-DOTBR_BUILD_STAMP=(\S*)")


@pytest.fixture
def tree(tmp_path):
    tools = tmp_path / "tools"
    tools.mkdir()
    for name in ("build_all.sh", "targets.py"):
        shutil.copy(ROOT / "tools" / name, tools / name)
    shutil.copytree(ROOT / "p4" / "targets", tmp_path / "p4" / "targets")
    (tmp_path / "p4" / "CMakeLists.txt").write_text("")
    (tmp_path / "p4" / "sdkconfig.defaults").write_text("")
    (tmp_path / "p4" / "sdkconfig.defaults.test").write_text("")
    for project in ("c6_updater", "c6_rcp", "c6_installer"):
        (tmp_path / project).mkdir()
        (tmp_path / project / "CMakeLists.txt").write_text("")
        (tmp_path / project / "sdkconfig.defaults").write_text("")
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    idf = bin_dir / "idf.py"
    idf.write_text(FAKE_IDF)
    idf.chmod(idf.stat().st_mode | stat.S_IXUSR)
    return tmp_path


def run(tree, *args):
    log = tree / "idf.log"
    log.write_text("")
    env = {
        **os.environ,
        "PATH": f"{tree / 'bin'}:{os.environ['PATH']}",
        "IDF_LOG": str(log),
    }
    result = subprocess.run(
        [str(tree / "tools" / "build_all.sh"), *args],
        check=False,
        capture_output=True,
        text=True,
        env=env,
    )
    return result, log.read_text().splitlines()


def calls_for(tree, lines, project):
    prefix = f"-C {tree / project} "
    return [line for line in lines if line.startswith(prefix)]


def test_no_option_builds_the_default_target(tree):
    result, lines = run(tree)
    assert result.returncode == 0, result.stderr
    (call,) = calls_for(tree, lines, "p4")
    assert f"-B {tree / DEFAULT_BUILD} " in call
    assert f"-DSDKCONFIG={tree / 'p4' / 'sdkconfig'} " in call
    assert "-DOTBR_BOARD_ID=ESP32-P4-WIFI6-POE-ETH " in call
    assert "-DOTBR_CHIP_ID=ESP32-P4-rev1.3 " in call


def test_target_builds_p4_with_its_build_dir_sdkconfig_defaults_and_ids(tree):
    result, lines = run(tree, "--target", "ESP32-P4-WIFI6-POE-ETH/ESP32-P4-rev3.0")
    assert result.returncode == 0, result.stderr
    (call,) = calls_for(tree, lines, "p4")
    assert f"-B {tree / REV3_BUILD} " in call
    sdkconfig = tree / "p4" / "sdkconfig_ESP32-P4-WIFI6-POE-ETH_ESP32-P4-rev3.0"
    assert f"-DSDKCONFIG={sdkconfig} " in call
    defaults = ";".join(
        str(tree / name)
        for name in (
            "p4/sdkconfig.defaults",
            "p4/targets/boards/ESP32-P4-WIFI6-POE-ETH.defaults",
            "p4/targets/chips/ESP32-P4-rev3.0.defaults",
        )
    )
    assert f"-DSDKCONFIG_DEFAULTS={defaults} " in call
    assert "-DOTBR_BOARD_ID=ESP32-P4-WIFI6-POE-ETH " in call
    assert "-DOTBR_CHIP_ID=ESP32-P4-rev3.0 " in call


def test_target_builds_the_c6_projects_once(tree):
    _, lines = run(tree, "--target", "ESP32-P4-WIFI6-POE-ETH/ESP32-P4-rev3.0")
    for project in ("c6_updater", "c6_rcp", "c6_installer"):
        assert len(calls_for(tree, lines, project)) == 1


def test_test_build_appends_the_test_defaults_and_uses_the_test_sdkconfig(tree):
    result, lines = run(
        tree, "--test", "--target", "ESP32-P4-WIFI6-POE-ETH/ESP32-P4-rev3.0"
    )
    assert result.returncode == 0, result.stderr
    (call,) = calls_for(tree, lines, "p4")
    sdkconfig = tree / "p4" / "sdkconfig_ESP32-P4-WIFI6-POE-ETH_ESP32-P4-rev3.0.test"
    assert f"-DSDKCONFIG={sdkconfig} " in call
    assert (
        f"ESP32-P4-rev3.0.defaults;{tree / 'p4' / 'sdkconfig.defaults.test'} " in call
    )


def test_test_build_of_the_default_target_keeps_sdkconfig_test(tree):
    _, lines = run(tree, "--test")
    (call,) = calls_for(tree, lines, "p4")
    assert f"-DSDKCONFIG={tree / 'p4' / 'sdkconfig.test'} " in call


def test_defaults_newer_than_the_sdkconfig_delete_it_first(tree):
    sdkconfig = tree / "p4" / "sdkconfig_ESP32-P4-WIFI6-POE-ETH_ESP32-P4-rev3.0"
    sdkconfig.write_text("")
    past = time.time() - 100
    os.utime(sdkconfig, (past, past))
    result, _ = run(tree, "--target", "ESP32-P4-WIFI6-POE-ETH/ESP32-P4-rev3.0")
    assert result.returncode == 0, result.stderr
    assert not sdkconfig.exists()


def test_defaults_older_than_the_sdkconfig_keep_it(tree):
    sdkconfig = tree / "p4" / "sdkconfig_ESP32-P4-WIFI6-POE-ETH_ESP32-P4-rev3.0"
    sdkconfig.write_text("")
    past = time.time() - 100
    for path in (tree / "p4").rglob("*.defaults*"):
        os.utime(path, (past, past))
    run(tree, "--target", "ESP32-P4-WIFI6-POE-ETH/ESP32-P4-rev3.0")
    assert sdkconfig.exists()


def test_all_targets_builds_every_target_with_one_stamp(tree):
    result, lines = run(tree, "--all-targets")
    assert result.returncode == 0, result.stderr
    calls = calls_for(tree, lines, "p4")
    assert len(calls) == 2
    assert f"-B {tree / DEFAULT_BUILD} " in calls[0]
    assert f"-B {tree / REV3_BUILD} " in calls[1]
    stamps = {STAMP.search(call).group(1) for call in calls}
    assert len(stamps) == 1
    assert re.fullmatch(r"\d{8}T\d{6}Z", stamps.pop())
    for project in ("c6_updater", "c6_rcp", "c6_installer"):
        assert len(calls_for(tree, lines, project)) == 1


def test_a_single_target_passes_an_empty_stamp(tree):
    _, lines = run(tree)
    (call,) = calls_for(tree, lines, "p4")
    assert STAMP.search(call).group(1) == ""


def test_all_targets_with_target_exits_2_with_the_usage_line(tree):
    result, lines = run(
        tree,
        "--all-targets",
        "--target",
        "ESP32-P4-WIFI6-POE-ETH/ESP32-P4-rev3.0",
    )
    assert result.returncode == 2
    assert "usage:" in result.stderr
    assert lines == []
