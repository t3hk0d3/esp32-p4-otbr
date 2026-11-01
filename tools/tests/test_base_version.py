"""tools/base_version.cmake: the frozen updater's version is content-addressed.

Uses a throwaway git repository and CMake project in a temp directory; the real repository is never touched.
"""

import re
import shutil
import subprocess
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
BASE_VERSION_CMAKE = ROOT / "tools" / "base_version.cmake"

pytestmark = pytest.mark.skipif(
    not (shutil.which("cmake") and shutil.which("git")),
    reason="needs cmake and git",
)

VERSION = re.compile(r"BASE=(\S+)")

INPUTS = {
    "IDF_VERSION": "v6.1\n",
    "c6_updater/main/updater_main.c": "int main_a;\n",
    "c6_updater/main/updater_core.c": "int core_a;\n",
    "c6_updater/sdkconfig.defaults": "CONFIG_A=y\n",
    "c6_updater/CMakeLists.txt": "project(x)\n",
    "common/rcp_sync_proto/rcp_sync_proto.c": "int proto_a;\n",
    "common/rcp_sync_proto/include/rcp_sync_proto.h": "int proto_h;\n",
    "common/c6_board/c6_board.h": "int board_a;\n",
    "common/c6_board/partitions_c6.csv": "nvs,data,nvs,0x9000,0x6000\n",
}
UNRELATED = {
    "p4/main/app_main.c": "int p4;\n",
    "README.md": "readme\n",
    "common/rcp_sync_proto/test_host/main/test_proto.c": "int host_test;\n",
    "common/rcp_sync_proto/test_host/build/junk.o": "obj\n",
    "c6_updater/build/c6_updater.bin": "bin\n",
}


def run(args, cwd):
    return subprocess.run(
        args, cwd=cwd, check=True, capture_output=True, text=True
    ).stdout


def write(repo, name, text):
    path = repo / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def commit(repo, message):
    run(["git", "add", "-A", "-f", "."], repo)
    run(["git", "commit", "-q", "-m", message], repo)


@pytest.fixture
def repo(tmp_path):
    repo = tmp_path / "repo"
    write(repo, "tools/placeholder", "")
    shutil.copy(BASE_VERSION_CMAKE, repo / "tools" / "base_version.cmake")
    for name, text in {**INPUTS, **UNRELATED}.items():
        write(repo, name, text)
    write(
        repo,
        "CMakeLists.txt",
        "cmake_minimum_required(VERSION 3.16)\n"
        "include(${CMAKE_CURRENT_LIST_DIR}/tools/base_version.cmake)\n"
        'message(STATUS "BASE=${PROJECT_VER}")\n',
    )
    run(["git", "init", "-q", "-b", "main"], repo)
    run(["git", "config", "user.email", "t@example.com"], repo)
    run(["git", "config", "user.name", "t"], repo)
    commit(repo, "first")
    return repo


def configure(repo):
    shutil.rmtree(repo / "build", ignore_errors=True)
    out = run(["cmake", "-S", ".", "-B", "build"], repo)
    return VERSION.search(out).group(1)


def test_version_has_the_base_prefix_and_fits_the_app_descriptor(repo):
    version = configure(repo)
    assert re.fullmatch(r"base-[0-9a-f]{8}", version)
    assert len(version) <= 30


def test_new_commit_of_an_unrelated_file_keeps_the_version(repo):
    first = configure(repo)
    write(repo, "p4/main/app_main.c", "int p4_changed;\n")
    commit(repo, "second")
    assert configure(repo) == first


def test_later_build_time_keeps_the_version(repo):
    first = configure(repo)
    time.sleep(1.1)
    assert configure(repo) == first


def test_touching_an_input_without_changing_it_keeps_the_version(repo):
    first = configure(repo)
    time.sleep(1.1)
    (repo / "c6_updater/main/updater_core.c").touch()
    assert configure(repo) == first


def test_dirty_work_tree_of_an_unrelated_file_keeps_the_version(repo):
    first = configure(repo)
    write(repo, "README.md", "edited\n")
    assert configure(repo) == first


def test_changes_below_test_host_and_build_directories_keep_the_version(repo):
    first = configure(repo)
    write(repo, "common/rcp_sync_proto/test_host/main/test_proto.c", "int other;\n")
    write(repo, "common/rcp_sync_proto/test_host/build/junk.o", "other\n")
    write(repo, "c6_updater/build/c6_updater.bin", "other\n")
    assert configure(repo) == first


@pytest.mark.parametrize(
    "name",
    [
        "c6_updater/main/updater_main.c",
        "common/rcp_sync_proto/rcp_sync_proto.c",
        "common/rcp_sync_proto/include/rcp_sync_proto.h",
        "common/c6_board/c6_board.h",
        "common/c6_board/partitions_c6.csv",
        "c6_updater/sdkconfig.defaults",
        "c6_updater/CMakeLists.txt",
        "IDF_VERSION",
    ],
)
def test_changing_an_input_changes_the_version(repo, name):
    first = configure(repo)
    write(repo, name, "changed\n")
    assert configure(repo) != first


def test_adding_a_source_file_to_an_input_directory_changes_the_version(repo):
    first = configure(repo)
    write(repo, "c6_updater/main/extra.c", "int extra;\n")
    assert configure(repo) != first


def test_moving_the_checkout_keeps_the_version(repo, tmp_path):
    first = configure(repo)
    moved = tmp_path / "elsewhere"
    shutil.copytree(repo, moved, ignore=shutil.ignore_patterns("build"))
    assert configure(moved) == first


@pytest.mark.parametrize(
    "name",
    [
        "c6_updater/main/.updater_main.c.swp",
        "c6_updater/main/updater_main.c~",
        "common/c6_board/.DS_Store",
        "common/rcp_sync_proto/#rcp_sync_proto.c#",
    ],
)
def test_editor_and_os_junk_files_keep_the_version(repo, name):
    first = configure(repo)
    write(repo, name, "junk\n")
    assert configure(repo) == first
