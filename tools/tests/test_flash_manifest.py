"""tools/flash_manifest.py: the ESP Web Tools manifest of the browser flasher."""

import importlib.util
import json
import subprocess
import sys
from pathlib import Path

import pytest

TOOL = Path(__file__).resolve().parent.parent / "flash_manifest.py"
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


@pytest.fixture(scope="module")
def flash_manifest():
    spec = importlib.util.spec_from_file_location("flash_manifest", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_parts_follow_flash_args(flash_manifest):
    data = flash_manifest.manifest(FLASH_ARGS, "v1.2.3")
    assert data["version"] == "v1.2.3"
    assert data["new_install_prompt_erase"] is True
    (build,) = data["builds"]
    assert build["chipFamily"] == "ESP32-P4"
    assert build["parts"] == [
        {"path": "bootloader.bin", "offset": 0x2000},
        {"path": "partition-table.bin", "offset": 0x8000},
        {"path": "ota_data_initial.bin", "offset": 0x19000},
        {"path": "p4.bin", "offset": 0x100000},
        {"path": "web_0.bin", "offset": 0x1100000},
    ]


def test_no_images_is_an_error(flash_manifest):
    with pytest.raises(ValueError):
        flash_manifest.manifest("--flash-mode dio\n", "v1")


def test_cli_writes_manifest(tmp_path):
    (tmp_path / "flash_args").write_text(FLASH_ARGS)
    for image in IMAGES:
        (tmp_path / image).write_bytes(b"\xff")
    subprocess.run([sys.executable, str(TOOL), str(tmp_path), "v1"], check=True)
    assert [
        p["path"]
        for p in json.loads((tmp_path / "manifest.json").read_text())["builds"][0][
            "parts"
        ]
    ] == IMAGES


def test_cli_refuses_missing_image(tmp_path):
    (tmp_path / "flash_args").write_text(FLASH_ARGS)
    result = subprocess.run(
        [sys.executable, str(TOOL), str(tmp_path), "v1"], capture_output=True, text=True
    )
    assert result.returncode == 1
    assert "missing bootloader.bin" in result.stderr
    assert not (tmp_path / "manifest.json").exists()
