"""tools/version.cmake: a new commit triggers a reconfigure, so the next build carries a fresh version stamp.

Uses a throwaway git repository and CMake project in a temp directory; the real repository is never touched.
"""

import re
import shutil
import subprocess
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
VERSION_CMAKE = ROOT / "tools" / "version.cmake"

pytestmark = pytest.mark.skipif(
    not (shutil.which("cmake") and shutil.which("ninja") and shutil.which("git")),
    reason="needs cmake, ninja and git",
)

STAMP = re.compile(r"STAMP=(\S+)")


def run(args, cwd):
    return subprocess.run(
        args, cwd=cwd, check=True, capture_output=True, text=True
    ).stdout


def commit(repo, name):
    (repo / name).write_text(name)
    run(["git", "add", name], repo)
    run(["git", "commit", "-q", "-m", name], repo)


@pytest.fixture
def project(tmp_path):
    repo = tmp_path / "repo"
    (repo / "tools").mkdir(parents=True)
    shutil.copy(VERSION_CMAKE, repo / "tools" / "version.cmake")
    (repo / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.16)\n"
        "include(${CMAKE_CURRENT_LIST_DIR}/tools/version.cmake)\n"
        "project(stamp NONE)\n"
        'message(STATUS "STAMP=${PROJECT_VER}")\n'
        "add_custom_target(noop)\n"
    )
    run(["git", "init", "-q", "-b", "main"], repo)
    run(["git", "config", "user.email", "t@example.com"], repo)
    run(["git", "config", "user.name", "t"], repo)
    run(["git", "add", "."], repo)
    run(["git", "commit", "-q", "-m", "first"], repo)
    return repo


def configure(repo):
    return STAMP.search(
        run(["cmake", "-S", ".", "-B", "build", "-G", "Ninja"], repo)
    ).group(1)


def build(repo):
    """ninja re-runs CMake first when a configure dependency changed; returns the stamp it printed, else None."""
    out = run(["ninja", "-C", "build"], repo)
    found = STAMP.search(out)
    return found.group(1) if found else None


def next_second():
    time.sleep(1.1)  # the stamp has second resolution


def test_build_without_a_new_commit_keeps_the_stamp(project):
    configure(project)
    assert build(project) is None


def test_new_commit_on_a_branch_gives_a_fresh_stamp(project):
    first = configure(project)
    next_second()
    commit(project, "second")
    second = build(project)
    assert second is not None and second != first
    assert (
        second.split("-")[1]
        == run(["git", "rev-parse", "--short=7", "HEAD"], project).strip()
    )


def test_new_commit_with_packed_refs_gives_a_fresh_stamp(project):
    run(["git", "pack-refs", "--all"], project)  # the branch ref is no longer a file
    first = configure(project)
    next_second()
    commit(project, "second")
    second = build(project)
    assert second is not None and second != first


def test_new_commit_on_a_detached_head_gives_a_fresh_stamp(project):
    run(["git", "checkout", "-q", "--detach"], project)
    first = configure(project)
    next_second()
    commit(project, "second")
    second = build(project)
    assert second is not None and second != first


def test_new_commit_in_a_linked_worktree_gives_a_fresh_stamp(project, tmp_path):
    linked = tmp_path / "linked"
    run(["git", "worktree", "add", "-q", "-b", "other", str(linked)], project)
    first = configure(linked)
    next_second()
    commit(linked, "second")
    second = build(linked)
    assert second is not None and second != first


def test_new_commit_with_packed_only_ref_and_no_reflog_gives_a_fresh_stamp(project):
    run(["git", "config", "core.logAllRefUpdates", "false"], project)
    run(["git", "pack-refs", "--all"], project)
    shutil.rmtree(project / ".git" / "logs", ignore_errors=True)
    first = configure(project)
    next_second()
    commit(project, "second")  # only the (new) loose branch ref file changes
    second = build(project)
    assert second is not None and second != first


def test_build_stamp_replaces_the_current_time(project):
    out = run(
        [
            "cmake",
            "-S",
            ".",
            "-B",
            "build",
            "-G",
            "Ninja",
            "-DOTBR_BUILD_STAMP=20260102T030405Z",
        ],
        project,
    )
    assert STAMP.search(out).group(1).startswith("20260102T030405Z-")


def test_empty_build_stamp_uses_the_configure_time(project):
    out = run(
        ["cmake", "-S", ".", "-B", "build", "-G", "Ninja", "-DOTBR_BUILD_STAMP="],
        project,
    )
    assert re.match(r"\d{8}T\d{6}Z-", STAMP.search(out).group(1))


def test_without_a_build_stamp_the_version_has_the_configure_time(project):
    assert re.match(r"\d{8}T\d{6}Z-", configure(project))
