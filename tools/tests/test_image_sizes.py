"""tools/image_sizes.py: sizes and limits read from fake build directories, and the report's deltas."""

import json
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SCRIPT = ROOT / "tools" / "image_sizes.py"
ENTRY = struct.Struct("<HBBII16sI")


def table(*parts):
    data = b"".join(
        ENTRY.pack(0x50AA, 0, 0, 0x10000 * i, size, label.encode(), 0)
        for i, (label, size) in enumerate(parts)
    )
    return data + b"\xff" * (0xC00 - len(data))


def spiffs_image():
    def block(*pages):
        return b"".join(p.ljust(256, b"\xff") for p in pages).ljust(4096, b"\xff")

    return block(b"lookup", b"", b"data", b"more") + block(b"lookup magic") + block()


def write_build(root, project, images, parts, boot_offset="0x2000"):
    build = root / project / "build"
    (build / "partition_table").mkdir(parents=True)
    (build / "bootloader").mkdir()
    (build / "partition_table" / "partition-table.bin").write_bytes(table(*parts))
    (build / "flasher_args.json").write_text(
        json.dumps(
            {
                "bootloader": {"offset": boot_offset},
                "partition-table": {"offset": "0x8000"},
            }
        )
    )
    for name, data in images.items():
        (build / name).write_bytes(data)


def run(*args):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *map(str, args)],
        check=True,
        capture_output=True,
        text=True,
    ).stdout


def test_collect_reads_sizes_and_limits(tmp_path):
    write_build(
        tmp_path,
        "p4",
        {
            "p4.bin": b"a" * 3000,
            "web_0.bin": spiffs_image(),
            "bootloader/bootloader.bin": b"b" * 500,
        },
        [("ota_0", 8 << 20), ("web_0", 2 << 20)],
    )
    write_build(
        tmp_path,
        "c6_updater",
        {"c6_updater.bin": b"u" * 10, "bootloader/bootloader.bin": b"b" * 20},
        [("test", 0x80000), ("ota_0", 0x180000)],
        boot_offset="0x0",
    )
    out = tmp_path / "sizes.json"
    run("collect", "--root", tmp_path, "--out", out)
    images = {i["name"]: i for i in json.loads(out.read_text())["images"]}

    assert images["P4 app"]["size"] == 3000
    assert images["P4 app"]["limit"] == 8 << 20
    # block 0: lookup page and two data pages; block 1: only its lookup page (free); block 2: erased
    assert images["P4 web UI"]["size"] == 3 * 256
    assert images["P4 web UI"]["limit"] == 2 << 20
    assert images["P4 bootloader"]["limit"] == 0x6000
    assert images["C6 updater (base)"]["limit"] == 0x80000  # the test slot, not ota_0
    assert images["C6 bootloader"]["limit"] == 0x8000
    assert "C6 radio (c6_rcp)" not in images  # not built: skipped


def test_report_deltas(tmp_path):
    def sizes(**kw):
        return {
            "images": [
                {"name": n, "file": n, "size": s, "limit": 4096, "region": "ota_0"}
                for n, s in kw.items()
            ]
        }

    head = tmp_path / "head.json"
    base = tmp_path / "base.json"
    head.write_text(json.dumps(sizes(a=2048, b=1000, c=5000)))
    base.write_text(json.dumps(sizes(a=1024, b=1000)))
    text = run("report", head, "--base", base, "--base-label", "master @ abc")

    assert text.startswith("<!-- image-sizes -->")
    assert "| a | 2.0 KiB | **+1.0 KiB** | ota_0 | 2.0 KiB | 50.0% |" in text
    assert "| b | 1,000 B" not in text and "| b | 1.0 KiB | 0 |" in text
    assert "| c | 4.9 KiB | new | ota_0 | -0.9 KiB | ⚠️ 122.1% |" in text
    assert "Δ against master @ abc." in text


def test_report_without_base(tmp_path):
    head = tmp_path / "head.json"
    head.write_text(
        json.dumps(
            {
                "images": [
                    {
                        "name": "x",
                        "file": "x",
                        "size": 10,
                        "limit": None,
                        "region": None,
                    }
                ]
            }
        )
    )
    text = run("report", head, "--base", tmp_path / "missing.json")
    assert "| x | 0.0 KiB |  | – | – | – |" in text
    assert "No base build to compare against." in text
