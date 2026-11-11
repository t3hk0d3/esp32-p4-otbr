"""tools/targets.py: the registry of firmware targets (board_id x chip_id) and the files each one builds from."""

import json
import subprocess
import sys
from pathlib import Path

import pytest

TOOL = Path(__file__).resolve().parent.parent / "targets.py"
REGISTRY = {
    "default": {"board": "B1", "chip": "C1"},
    "boards": {"B1": {"name": "Board one"}, "B2": {"name": "Board two"}},
    "chips": {
        "C1": {"name": "Chip one", "min_rev": 0, "max_rev": 199},
        "C2": {"name": "Chip two", "min_rev": 301, "max_rev": 399},
    },
    "targets": [
        {"board": "B1", "chip": "C1"},
        {"board": "B1", "chip": "C2"},
        {"board": "B2", "chip": "C2"},
    ],
}


@pytest.fixture
def registry(tmp_path):
    path = tmp_path / "targets.json"
    path.write_text(json.dumps(REGISTRY))
    return path


def run(registry, *args):
    return subprocess.run(
        [sys.executable, str(TOOL), "--registry", str(registry), *args],
        check=False,
        capture_output=True,
        text=True,
    )


def resolve(registry, *ids):
    result = run(registry, "resolve", *ids)
    assert result.returncode == 0, result.stderr
    return dict(line.split("=", 1) for line in result.stdout.splitlines())


def test_default_target_builds_into_p4_build(registry):
    info = resolve(registry, "B1", "C1")
    assert info["build_dir"] == "p4/build"
    assert info["sdkconfig"] == "p4/sdkconfig"


def test_resolve_without_ids_is_the_default_target(registry):
    assert resolve(registry) == resolve(registry, "B1", "C1")


def test_another_target_builds_into_its_own_dir_and_sdkconfig(registry):
    info = resolve(registry, "B2", "C2")
    assert info["build_dir"] == "p4/build_B2_C2"
    assert info["sdkconfig"] == "p4/sdkconfig_B2_C2"


@pytest.mark.parametrize("ids", [("B1", "C1"), ("B2", "C2")])
def test_defaults_are_common_then_board_then_chip(registry, ids):
    board, chip = ids
    info = resolve(registry, board, chip)
    assert info["defaults"].split(";") == [
        "p4/sdkconfig.defaults",
        f"p4/targets/boards/{board}.defaults",
        f"p4/targets/chips/{chip}.defaults",
    ]
    assert info["board_id"] == board
    assert info["chip_id"] == chip


@pytest.mark.parametrize("ids", [("NOPE", "C1"), ("B1", "NOPE")])
def test_unknown_id_exits_2_listing_the_known_ids(registry, ids):
    result = run(registry, "resolve", *ids)
    assert result.returncode == 2
    for known in ("B1", "B2", "C1", "C2"):
        assert known in result.stderr


def test_known_ids_that_are_not_a_listed_pair_exit_2_listing_the_targets(
    registry,
):
    result = run(registry, "resolve", "B2", "C1")
    assert result.returncode == 2
    for pair in ("B1/C1", "B1/C2", "B2/C2"):
        assert pair in result.stderr


def test_list_prints_every_target(registry):
    result = run(registry, "list")
    assert result.returncode == 0
    assert result.stdout.split() == ["B1/C1", "B1/C2", "B2/C2"]


def test_load_rejects_an_id_with_an_underscore(tmp_path):
    sys.path.insert(0, str(TOOL.parent))
    try:
        import targets
    finally:
        sys.path.pop(0)
    bad = json.loads(json.dumps(REGISTRY))
    bad["boards"]["B_3"] = {"name": "Bad"}
    path = tmp_path / "targets.json"
    path.write_text(json.dumps(bad))
    with pytest.raises(ValueError, match="B_3"):
        targets.load(path)


def test_the_real_registry_is_consistent():
    result = subprocess.run(
        [sys.executable, str(TOOL), "resolve"],
        check=False,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    assert "build_dir=p4/build\n" in result.stdout
