"""tools/target.cmake: the board and chip ids of the target become C string macros.

Uses a throwaway CMake project; idf_build_set_property is replaced by a function that prints what it is given.
"""

import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
TARGET_CMAKE = ROOT / "tools" / "target.cmake"

pytestmark = pytest.mark.skipif(not shutil.which("cmake"), reason="needs cmake")


@pytest.fixture
def project(tmp_path):
    (tmp_path / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.16)\n"
        "project(target NONE)\n"
        "function(idf_build_set_property name value)\n"
        '    message(STATUS "SET ${name} ${value}")\n'
        "endfunction()\n"
        f"include({TARGET_CMAKE.as_posix()})\n"
    )
    return tmp_path


def configure(project, *defines):
    return subprocess.run(
        ["cmake", "-S", ".", "-B", "build", *defines],
        cwd=project,
        check=False,
        capture_output=True,
        text=True,
    )


def test_both_ids_become_c_string_macros(project):
    result = configure(project, "-DOTBR_BOARD_ID=Board-1", "-DOTBR_CHIP_ID=Chip-2.0")
    assert result.returncode == 0, result.stderr
    assert 'SET COMPILE_DEFINITIONS OTBR_BOARD_ID="Board-1"' in result.stdout
    assert 'SET COMPILE_DEFINITIONS OTBR_CHIP_ID="Chip-2.0"' in result.stdout


@pytest.mark.parametrize(
    "defines, missing",
    [
        (["-DOTBR_CHIP_ID=Chip-2.0"], "OTBR_BOARD_ID"),
        (["-DOTBR_BOARD_ID=Board-1"], "OTBR_CHIP_ID"),
        ([], "OTBR_BOARD_ID"),
    ],
)
def test_a_missing_id_fails_the_configure_step(project, defines, missing):
    result = configure(project, *defines)
    assert result.returncode != 0
    assert missing in result.stderr
