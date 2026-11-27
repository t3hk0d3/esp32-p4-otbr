"""tools/cache_arbitration_patch.cmake: the generated hal component with round-robin L1 D-cache arbitration.

Uses a throwaway CMake project and a fake IDF tree (IDF_PATH) with a small cache_hal.c that holds the v6.1 line the
script patches.
"""

import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
SCRIPT = ROOT / "tools" / "cache_arbitration_patch.cmake"

pytestmark = pytest.mark.skipif(not shutil.which("cmake"), reason="needs cmake")

IDF_LINE = "    cache_ll_preload_set_strategy(CACHE_LL_LEVEL_ALL, CACHE_TYPE_ALL, CACHE_LL_ID_ALL, CACHE_LL_PRELOAD_ARBITRARY);\n"
NEW_LINE = "    cache_ll_preload_set_strategy(1, CACHE_TYPE_DATA, CACHE_LL_ID_ALL, CACHE_LL_PRELOAD_UNTIL_FETCH_DONE);\n"

CACHE_HAL = (
    """#if SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
    cache_hal_init_l2_cache(config);
#endif
    //CACHE_LL_PRELOAD_ARBITRARY will have better performance for preload
"""
    + IDF_LINE
    + """}

#if CACHE_LL_ENABLE_DISABLE_STATE_SW
"""
)


@pytest.fixture
def idf(tmp_path):
    hal = tmp_path / "idf" / "components" / "hal"
    (hal / "esp32p4").mkdir(parents=True)
    (hal / "esp32p4" / "cache_ll.h").write_text("/* ll */\n")
    (hal / "CMakeLists.txt").write_text("# hal\n")
    (hal / "linker.lf").write_text("# linker\n")
    (hal / "cache_hal.c").write_text(CACHE_HAL)
    return tmp_path / "idf"


@pytest.fixture
def project(tmp_path):
    proj = tmp_path / "proj"
    proj.mkdir()
    (proj / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.16)\n"
        "project(patch NONE)\n"
        "set(EXTRA_COMPONENT_DIRS /earlier)\n"
        f"include({SCRIPT.as_posix()})\n"
        'message(STATUS "DIRS ${EXTRA_COMPONENT_DIRS}")\n'
    )
    return proj


def configure(project, idf):
    return subprocess.run(
        ["cmake", "-S", ".", "-B", "build"],
        cwd=project,
        env={**os.environ, "IDF_PATH": str(idf)},
        check=False,
        capture_output=True,
        text=True,
    )


def configured(project, idf):
    result = configure(project, idf)
    assert result.returncode == 0, result.stderr
    return result


def generated(project):
    return project / "build" / "idf_patch_components" / "hal"


def patched_text(project):
    return (generated(project) / "cache_hal.c").read_text()


def test_v61_line_writes_cache_hal_with_the_round_robin_line_right_after_it(
    project, idf
):
    configured(project, idf)
    text = patched_text(project)
    after = text.split(IDF_LINE, 1)[1]
    lines = after.splitlines(keepends=True)
    assert lines[0].lstrip().startswith("//")
    assert "19369" in lines[0]
    assert lines[1] == NEW_LINE


def test_v61_line_keeps_the_idf_line(project, idf):
    configured(project, idf)
    assert patched_text(project).count(IDF_LINE) == 1


def test_v61_line_written_cache_hal_is_otherwise_identical(project, idf):
    configured(project, idf)
    path = generated(project) / "cache_hal.c"
    assert not path.is_symlink()
    lines = path.read_text().splitlines(keepends=True)
    index = lines.index(IDF_LINE)
    assert "".join(lines[: index + 1] + lines[index + 3 :]) == CACHE_HAL


def test_v61_line_every_other_entry_is_a_symlink(project, idf):
    configured(project, idf)
    for name in ("CMakeLists.txt", "linker.lf", "esp32p4"):
        link = generated(project) / name
        assert link.is_symlink(), name
        assert link.resolve() == (idf / "components" / "hal" / name).resolve()


def test_v61_line_appends_the_components_dir(project, idf):
    result = configured(project, idf)
    expected = f"DIRS /earlier;{project / 'build' / 'idf_patch_components'}"
    assert expected in result.stdout


def test_v61_line_prints_a_status_line(project, idf):
    result = configured(project, idf)
    assert f"cache_arbitration_patch: hal from {generated(project)}" in result.stdout


def test_missing_line_fails_the_configure_naming_the_file(project, idf):
    cache_hal = idf / "components" / "hal" / "cache_hal.c"
    cache_hal.write_text(
        CACHE_HAL.replace(
            "CACHE_LL_PRELOAD_ARBITRARY);", "CACHE_LL_PRELOAD_ROUND_ROBIN);"
        )
    )
    result = configure(project, idf)
    assert result.returncode != 0
    assert "cache_arbitration_patch: a text to replace is not in" in result.stderr
    assert str(cache_hal) in result.stderr.replace("\n", "")


def test_reconfigure_unchanged_keeps_the_mtime(project, idf):
    configured(project, idf)
    path = generated(project) / "cache_hal.c"
    os.utime(path, (1_000_000, 1_000_000))
    configured(project, idf)
    assert path.stat().st_mtime == 1_000_000


def test_removed_idf_file_drops_the_dangling_symlink(project, idf):
    configured(project, idf)
    (idf / "components" / "hal" / "linker.lf").unlink()
    configured(project, idf)
    assert not os.path.lexists(generated(project) / "linker.lf")


def test_stale_generated_component_is_removed(project, idf):
    stale = project / "build" / "idf_patch_components" / "freertos"
    stale.mkdir(parents=True)
    (stale / "app_startup.c").write_text("/* old patch */\n")
    configured(project, idf)
    assert not os.path.lexists(stale)
    assert generated(project).is_dir()


def test_added_idf_file_is_linked(project, idf):
    configured(project, idf)
    (idf / "components" / "hal" / "new_hal.c").write_text("/* new */\n")
    configured(project, idf)
    assert (generated(project) / "new_hal.c").is_symlink()
