"""tools/package_release.sh: what a release directory holds, and when packaging refuses.

Runs the script in a temporary tree with a build directory for every target in the registry, and a fake IDF_PATH.
"""

import hashlib
import json
import os
import shutil
import subprocess
import zipfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
BOARD = "ESP32-P4-WIFI6-POE-ETH"
DEFAULT = f"{BOARD}/ESP32-P4-rev1.3"
REV3 = f"{BOARD}/ESP32-P4-rev3.0"
VERSION = "v0.4.0-12-gabc1234"
LICENSE_FILES = (
    "LICENSE",
    "components/openthread/openthread/LICENSE",
    "components/lwip/lwip/COPYING",
    "components/mbedtls/mbedtls/LICENSE",
    "components/esp_phy/lib/LICENSE",
    "components/esp_coex/lib/LICENSE",
    "components/esp_wifi/lib/LICENSE",
)


def write(path, text=""):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def target_files(tree, pair):
    """The build dir and sdkconfig of a target, as tools/targets.py resolves them."""
    board, chip = pair.split("/")
    out = subprocess.run(
        ["python3", str(tree / "tools" / "targets.py"), "resolve", board, chip],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    values = dict(line.split("=", 1) for line in out.splitlines())
    return tree / values["build_dir"], tree / values["sdkconfig"]


def fake_p4_build(tree, pair, min_rev, max_rev):
    build, sdkconfig = target_files(tree, pair)
    write(build / "project_description.json", json.dumps({"project_version": VERSION}))
    write(build / "CMakeCache.txt", "OTBR_TEST_BUILD:BOOL=0\n")
    write(
        build / "flash_args",
        "--flash-mode dio --flash-freq 40m --flash-size 32MB\n"
        "0x2000 bootloader/bootloader.bin\n"
        "0x100000 p4.bin\n"
        "0x1100000 web_0.bin\n",
    )
    write(build / "bootloader" / "bootloader.bin", f"bootloader of {pair}")
    write(build / "p4.bin", f"app of {pair}")
    write(build / "web_0.bin", f"web of {pair}")
    write(
        sdkconfig,
        f"CONFIG_ESP32P4_REV_MIN_FULL={min_rev}\nCONFIG_ESP32P4_REV_MAX_FULL={max_rev}\n",
    )


@pytest.fixture
def tree(tmp_path):
    tools = tmp_path / "tools"
    tools.mkdir()
    for name in ("package_release.sh", "targets.py", "make_bundle.py"):
        shutil.copy(ROOT / "tools" / name, tools / name)
    shutil.copytree(ROOT / "p4" / "targets", tmp_path / "p4" / "targets")
    registry = json.loads((tmp_path / "p4" / "targets" / "targets.json").read_text())
    for target in registry["targets"]:
        chip = registry["chips"][target["chip"]]
        fake_p4_build(
            tmp_path,
            f"{target['board']}/{target['chip']}",
            chip["min_rev"],
            chip["max_rev"],
        )
    for project, name in (
        ("c6_rcp", "c6_rcp.bin"),
        ("c6_installer", "c6_installer.bin"),
        ("c6_updater", "c6_updater.bin"),
    ):
        build = tmp_path / project / "build"
        write(build / "project_description.json", json.dumps({"project_version": "1"}))
        write(build / "CMakeCache.txt", "OTBR_TEST_BUILD:BOOL=0\n")
        write(build / name, project)
    updater = tmp_path / "c6_updater" / "build"
    write(updater / "flash_args", "--flash-mode dio\n")
    write(updater / "bootloader" / "bootloader.bin")
    write(updater / "partition_table" / "partition-table.bin")
    write(
        tmp_path / "common" / "c6_board" / "partitions_c6.csv",
        "test, app, test, 0x110000, 1M\n",
    )
    for name in ("LICENSE", "NOTICE"):
        write(tmp_path / name, name)
    write(tmp_path / "p4" / "components" / "esp_ot_br_server" / "LICENSE")
    write(tmp_path / "p4" / "components" / "zigbee" / "ziggurat" / "LICENSE.md")
    write(tmp_path / "p4" / "managed_components" / "espressif__fake" / "LICENSE")
    idf = tmp_path / "idf"
    for name in LICENSE_FILES:
        write(idf / name)
    return tmp_path


def run(tree, out=None, **env):
    out = out or tree / "dist"
    result = subprocess.run(
        [str(tree / "tools" / "package_release.sh"), str(out)],
        check=False,
        capture_output=True,
        text=True,
        env={**os.environ, "IDF_PATH": str(tree / "idf"), **env},
    )
    return result, out


def build_dir(tree, pair):
    return target_files(tree, pair)[0]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def zip_names(path):
    with zipfile.ZipFile(path) as archive:
        return set(archive.namelist())


def test_with_every_target_built_it_writes_the_bundle_and_zip_of_each_target(tree):
    result, out = run(tree)
    assert result.returncode == 0, result.stderr
    for pair in (DEFAULT, REV3):
        name = pair.replace("/", "_")
        assert (out / f"{name}.ota").is_file()
        assert {"flash_args", "p4.bin", "web_0.bin"} <= zip_names(
            out / f"full_{name}.zip"
        )


def test_each_target_zip_and_bundle_come_from_its_own_build(tree):
    result, out = run(tree)
    assert result.returncode == 0, result.stderr
    name = REV3.replace("/", "_")
    with zipfile.ZipFile(out / f"full_{name}.zip") as archive:
        assert archive.read("p4.bin") == f"app of {REV3}".encode()
    assert f"app of {REV3}".encode() in (out / f"{name}.ota").read_bytes()


def test_it_writes_the_plain_names_as_copies_of_the_default_target(tree):
    result, out = run(tree)
    assert result.returncode == 0, result.stderr
    name = DEFAULT.replace("/", "_")
    assert (out / "otbr.bundle").read_bytes() == (out / f"{name}.ota").read_bytes()
    assert (out / "p4-flash.zip").read_bytes() == (
        out / f"full_{name}.zip"
    ).read_bytes()


def test_it_lists_every_file_in_sha256sums(tree):
    result, out = run(tree)
    assert result.returncode == 0, result.stderr
    listed = {
        line.split()[1]: line.split()[0]
        for line in (out / "SHA256SUMS").read_text().splitlines()
    }
    for pair in (DEFAULT, REV3):
        name = pair.replace("/", "_")
        for file in (f"{name}.ota", f"full_{name}.zip"):
            assert listed[file] == sha256(out / file)
    assert {"otbr.bundle", "p4-flash.zip", "versions.txt"} <= set(listed)


def test_it_writes_one_p4_app_line_to_versions_txt(tree):
    result, out = run(tree)
    assert result.returncode == 0, result.stderr
    lines = (out / "versions.txt").read_text().splitlines()
    assert [line for line in lines if line.startswith("p4 app:")] == [
        f"p4 app:      {VERSION}"
    ]


def test_a_missing_build_dir_exits_1_saying_to_run_make_build_release(tree):
    shutil.rmtree(build_dir(tree, REV3))
    result, out = run(tree)
    assert result.returncode == 1
    assert "make build-release" in result.stderr
    assert REV3 in result.stderr
    assert not out.exists()


def test_a_test_build_exits_1_and_writes_nothing(tree):
    cache = build_dir(tree, REV3) / "CMakeCache.txt"
    cache.write_text("OTBR_TEST_BUILD:BOOL=1\n")
    result, out = run(tree)
    assert result.returncode == 1
    assert "--test build" in result.stderr
    assert not out.exists()


def test_different_versions_between_targets_exit_1_naming_them(tree):
    write(
        build_dir(tree, REV3) / "project_description.json",
        json.dumps({"project_version": "v0.4.0-9-gdef5678"}),
    )
    result, out = run(tree)
    assert result.returncode == 1
    assert VERSION in result.stderr
    assert "v0.4.0-9-gdef5678" in result.stderr
    assert REV3 in result.stderr
    assert not out.exists()


@pytest.mark.parametrize("key", ["MIN", "MAX"])
def test_a_sdkconfig_revision_range_that_differs_from_the_registry_exits_1_naming_the_target(
    tree, key
):
    _, sdkconfig = target_files(tree, REV3)
    text = sdkconfig.read_text()
    sdkconfig.write_text(
        "\n".join(
            f"CONFIG_ESP32P4_REV_{key}_FULL=0" if f"REV_{key}_FULL" in line else line
            for line in text.splitlines()
        )
        + "\n"
    )
    result, out = run(tree)
    assert result.returncode == 1
    assert REV3 in result.stderr
    assert not out.exists()
