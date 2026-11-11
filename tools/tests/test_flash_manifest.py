"""tools/flash_manifest.py and `make docs-firmware`: the firmware files of the browser flasher."""

import json
import os
import subprocess
import sys
import zipfile
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parent.parent
ROOT = TOOLS.parent
TOOL = TOOLS / "flash_manifest.py"
BOARD = "ESP32-P4-WIFI6-POE-ETH"
V1 = "ESP32-P4-rev1.3"
V3 = "ESP32-P4-rev3.0"
FLASH_ARGS = """--flash-mode dio --flash-freq 40m --flash-size 32MB
0x2000 bootloader.bin
0x8000 partition-table.bin
0x19000 ota_data_initial.bin
0x100000 p4.bin
0x1100000 web_0.bin
"""
IMAGES = [
    "bootloader.bin",
    "partition-table.bin",
    "ota_data_initial.bin",
    "p4.bin",
    "web_0.bin",
]
V1_ENTRY = {
    "board": BOARD,
    "board_name": f"Waveshare {BOARD}",
    "chip": V1,
    "chip_name": "ESP32-P4 revision v1.x",
    "min_rev": 0,
    "max_rev": 199,
    "manifest": f"{BOARD}/{V1}/manifest.json",
    "version": "v1",
}
V3_ENTRY = {
    "board": BOARD,
    "board_name": f"Waveshare {BOARD}",
    "chip": V3,
    "chip_name": "ESP32-P4 revision v3.x",
    "min_rev": 301,
    "max_rev": 399,
    "manifest": f"{BOARD}/{V3}/manifest.json",
    "version": "v1",
}


@pytest.fixture(scope="module")
def flash_manifest():
    sys.path.insert(0, str(TOOLS))
    import flash_manifest as module

    return module


def write_zip(path, flash_args=FLASH_ARGS, images=IMAGES):
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("flash_args", flash_args)
        for image in images:
            archive.writestr(image, b"\xff")


def publish(zips, out):
    return subprocess.run(
        [sys.executable, str(TOOL), str(zips), str(out), "v1"],
        check=False,
        capture_output=True,
        text=True,
    )


def read_json(path):
    return json.loads(path.read_text())


def test_parts_follow_flash_args(flash_manifest):
    data = flash_manifest.manifest(FLASH_ARGS, "v1.2.3")
    assert data["version"] == "v1.2.3"
    assert data["parts"] == [
        {"path": "bootloader.bin", "offset": 0x2000},
        {"path": "partition-table.bin", "offset": 0x8000},
        {"path": "ota_data_initial.bin", "offset": 0x19000},
        {"path": "p4.bin", "offset": 0x100000},
        {"path": "web_0.bin", "offset": 0x1100000},
    ]


def test_no_images_is_an_error(flash_manifest):
    with pytest.raises(ValueError):
        flash_manifest.manifest("--flash-mode dio\n", "v1")


@pytest.fixture
def release_with_targets(tmp_path):
    zips = tmp_path / "zips"
    write_zip(zips / f"full_{BOARD}_{V1}.zip")
    write_zip(
        zips / f"full_{BOARD}_{V3}.zip",
        FLASH_ARGS.replace("40m", "80m").replace("0x2000", "0x3000"),
    )
    write_zip(zips / "p4-flash.zip")
    return zips


@pytest.fixture
def release_without_targets(tmp_path):
    zips = tmp_path / "zips"
    write_zip(zips / "p4-flash.zip")
    return zips


class TestWithTargetZips:
    def test_unpacks_each_with_a_manifest(self, release_with_targets, tmp_path):
        out = tmp_path / "firmware"
        assert publish(release_with_targets, out).returncode == 0
        for chip in (V1, V3):
            assert (out / BOARD / chip / "p4.bin").is_file()
            assert (out / BOARD / chip / "manifest.json").is_file()

    def test_each_manifest_comes_from_its_own_zip(self, release_with_targets, tmp_path):
        out = tmp_path / "firmware"
        publish(release_with_targets, out)
        v1, v3 = (read_json(out / BOARD / c / "manifest.json") for c in (V1, V3))
        assert v1["parts"][0]["offset"] == 0x2000
        assert v3["parts"][0]["offset"] == 0x3000

    def test_lists_them_in_targets_json(self, release_with_targets, tmp_path):
        out = tmp_path / "firmware"
        publish(release_with_targets, out)
        assert read_json(out / "targets.json") == [V1_ENTRY, V3_ENTRY]

    def test_lists_only_published_targets(self, tmp_path):
        zips = tmp_path / "zips"
        write_zip(zips / f"full_{BOARD}_{V3}.zip")
        out = tmp_path / "firmware"
        publish(zips, out)
        assert read_json(out / "targets.json") == [V3_ENTRY]
        assert not (out / BOARD / V1).exists()


class TestWithOnlyTheOldZip:
    def test_publishes_it_as_the_default_target(
        self, release_without_targets, tmp_path
    ):
        out = tmp_path / "firmware"
        assert publish(release_without_targets, out).returncode == 0
        assert (out / BOARD / V1 / "manifest.json").is_file()
        assert read_json(out / "targets.json") == [V1_ENTRY]


class TestRefusals:
    def test_a_missing_image_names_it(self, tmp_path):
        zips = tmp_path / "zips"
        write_zip(zips / "p4-flash.zip", images=IMAGES[1:])
        out = tmp_path / "firmware"
        result = publish(zips, out)
        assert result.returncode == 1
        assert "missing bootloader.bin" in result.stderr
        assert not (out / "targets.json").exists()

    def test_a_release_without_firmware_fails(self, tmp_path):
        zips = tmp_path / "zips"
        zips.mkdir()
        result = publish(zips, tmp_path / "firmware")
        assert result.returncode == 1
        assert "no firmware" in result.stderr


FAKE_GH = """#!/usr/bin/env bash
# Fake gh: serves $FAKE_RELEASE, honouring --pattern and --dir as `gh release download` does.
set -euo pipefail
if [ "$2" = view ]; then echo v1; exit 0; fi
patterns=()
while [ $# -gt 0 ]; do
    case "$1" in
    --pattern) patterns+=("$2"); shift 2 ;;
    --dir) dir="$2"; shift 2 ;;
    *) shift ;;
    esac
done
mkdir -p "$dir"
for pattern in "${patterns[@]}"; do
    for file in "$FAKE_RELEASE"/$pattern; do
        [ -e "$file" ] && cp "$file" "$dir/"
    done
done
"""


def make_docs_firmware(tmp_path, release):
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    gh = bin_dir / "gh"
    gh.write_text(FAKE_GH)
    gh.chmod(0o755)
    site = tmp_path / "site"
    env = dict(
        os.environ,
        PATH=f"{bin_dir}:{os.environ['PATH']}",
        FAKE_RELEASE=str(release),
    )
    result = subprocess.run(
        ["make", "-s", "docs-firmware", f"SITE={site}"],
        check=False,
        cwd=ROOT,
        env=env,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    return site


class TestMakeDocsFirmware:
    def test_with_target_zips_publishes_each(self, release_with_targets, tmp_path):
        site = make_docs_firmware(tmp_path, release_with_targets)
        assert read_json(site / "firmware" / "targets.json") == [V1_ENTRY, V3_ENTRY]

    def test_with_only_the_old_zip_publishes_the_default_target(
        self, release_without_targets, tmp_path
    ):
        site = make_docs_firmware(tmp_path, release_without_targets)
        assert read_json(site / "firmware" / "targets.json") == [V1_ENTRY]

    def test_leaves_no_zips_in_the_site(self, release_with_targets, tmp_path):
        site = make_docs_firmware(tmp_path, release_with_targets)
        assert not list(site.rglob("*.zip"))
