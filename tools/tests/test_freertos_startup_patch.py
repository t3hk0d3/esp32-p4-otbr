"""tools/freertos_startup_patch.cmake: the generated freertos component with a pause in CPU1's start wait.

Uses a throwaway CMake project and a fake IDF tree (IDF_PATH) with a small app_startup.c that holds the v6.1 texts the
script replaces.
"""

import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
SCRIPT = ROOT / "tools" / "freertos_startup_patch.cmake"

pytestmark = pytest.mark.skipif(not shutil.which("cmake"), reason="needs cmake")

STARTUP = """#include "sdkconfig.h"
#include "esp_chip_info.h"
#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif

void esp_startup_start_app_other_cores(void)
{
    // Wait for CPU0 to start FreeRTOS before progressing
    extern volatile unsigned port_xSchedulerRunning[CONFIG_FREERTOS_NUMBER_OF_CORES];
    while (port_xSchedulerRunning[0] == 0) {
        ;
    }
}
"""

PATCHED = """#include "sdkconfig.h"
#include "esp_chip_info.h"
#include "esp_rom_sys.h" // freertos_startup_patch: esp_rom_delay_us
#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif

void esp_startup_start_app_other_cores(void)
{
    // Wait for CPU0 to start FreeRTOS before progressing
    extern volatile unsigned port_xSchedulerRunning[CONFIG_FREERTOS_NUMBER_OF_CORES];
    while (port_xSchedulerRunning[0] == 0) {
        esp_rom_delay_us(10); // freertos_startup_patch: ESP-IDF issue 19369, CPU1's spin starves CPU0 on P4 v3.x
    }
}
"""


@pytest.fixture
def idf(tmp_path):
    freertos = tmp_path / "idf" / "components" / "freertos"
    (freertos / "FreeRTOS-Kernel").mkdir(parents=True)
    (freertos / "FreeRTOS-Kernel" / "tasks.c").write_text("/* tasks */\n")
    (freertos / "CMakeLists.txt").write_text("# freertos\n")
    (freertos / "Kconfig").write_text("# Kconfig\n")
    (freertos / "app_startup.c").write_text(STARTUP)
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


def generated(project):
    return project / "build" / "idf_patch_components" / "freertos"


def test_v61_loop_writes_app_startup_with_the_delay_as_loop_body(project, idf):
    result = configure(project, idf)
    assert result.returncode == 0, result.stderr
    text = (generated(project) / "app_startup.c").read_text()
    assert "while (port_xSchedulerRunning[0] == 0) {\n        esp_rom_delay_us(10);" in text


def test_v61_loop_written_app_startup_includes_esp_rom_sys(project, idf):
    configure(project, idf)
    assert '#include "esp_rom_sys.h"' in (generated(project) / "app_startup.c").read_text()


def test_v61_loop_written_app_startup_is_otherwise_identical(project, idf):
    configure(project, idf)
    path = generated(project) / "app_startup.c"
    assert not path.is_symlink()
    assert path.read_text() == PATCHED


def test_v61_loop_every_other_entry_is_a_symlink(project, idf):
    configure(project, idf)
    for name in ("CMakeLists.txt", "Kconfig", "FreeRTOS-Kernel"):
        link = generated(project) / name
        assert link.is_symlink(), name
        assert link.resolve() == (idf / "components" / "freertos" / name).resolve()


def test_v61_loop_appends_the_components_dir(project, idf):
    result = configure(project, idf)
    expected = f"DIRS /earlier;{project / 'build' / 'idf_patch_components'}"
    assert expected in result.stdout


def test_v61_loop_prints_a_status_line(project, idf):
    result = configure(project, idf)
    assert f"freertos_startup_patch: FreeRTOS from {generated(project)}" in result.stdout


def test_missing_loop_text_fails_the_configure_naming_the_file(project, idf):
    startup = idf / "components" / "freertos" / "app_startup.c"
    startup.write_text(STARTUP.replace("        ;\n", "        portYIELD_CORE(1);\n"))
    result = configure(project, idf)
    assert result.returncode != 0
    assert "freertos_startup_patch: a text to replace is not in" in result.stderr
    assert str(startup) in result.stderr.replace("\n", "")


def test_missing_include_anchor_fails_the_configure(project, idf):
    startup = idf / "components" / "freertos" / "app_startup.c"
    startup.write_text(STARTUP.replace('#include "esp_chip_info.h"\n', ""))
    result = configure(project, idf)
    assert result.returncode != 0
    assert "freertos_startup_patch: a text to replace is not in" in result.stderr


def test_reconfigure_unchanged_keeps_the_mtime(project, idf):
    configure(project, idf)
    path = generated(project) / "app_startup.c"
    os.utime(path, (1_000_000, 1_000_000))
    result = configure(project, idf)
    assert result.returncode == 0, result.stderr
    assert path.stat().st_mtime == 1_000_000


def test_removed_idf_file_drops_the_dangling_symlink(project, idf):
    configure(project, idf)
    (idf / "components" / "freertos" / "Kconfig").unlink()
    configure(project, idf)
    assert not os.path.lexists(generated(project) / "Kconfig")


def test_added_idf_file_is_linked(project, idf):
    configure(project, idf)
    (idf / "components" / "freertos" / "port_new.c").write_text("/* new */\n")
    configure(project, idf)
    assert (generated(project) / "port_new.c").is_symlink()
