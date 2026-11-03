#!/usr/bin/env python3
"""Sizes of the built firmware images, their flash headroom, and the change against another build.

  image_sizes.py collect [--root DIR] --out sizes.json   read the build directories (after make build)
  image_sizes.py report HEAD.json [--base BASE.json]      Markdown table, with deltas when a base is given

Needs no ESP-IDF environment: the limits come from each build's partition-table.bin and flasher_args.json.
"""

import argparse
import json
import struct
import sys
from pathlib import Path

ENTRY = struct.Struct(
    "<HBBII16sI"
)  # magic, type, subtype, offset, size, label, flags (32 bytes)
ENTRY_MAGIC = 0x50AA

# (name, project, image in the project's build directory, where it goes on the flash)
# where: a partition label, "bootloader" (the space before the partition table) or None (no fixed slot here:
# c6_installer is booted from the stock ESP-Hosted firmware's own partition, whose size this repository does not set).
IMAGES = [
    ("P4 app", "p4", "p4.bin", "ota_0"),
    ("P4 web UI", "p4", "web_0.bin", "web_0"),
    ("P4 bootloader", "p4", "bootloader/bootloader.bin", "bootloader"),
    ("C6 radio (c6_rcp)", "c6_rcp", "c6_rcp.bin", "ota_0"),
    ("C6 updater (base)", "c6_updater", "c6_updater.bin", "test"),
    ("C6 bootloader", "c6_updater", "bootloader/bootloader.bin", "bootloader"),
    ("C6 installer", "c6_installer", "c6_installer.bin", None),
]
# SPIFFS images fill the whole partition and spread their pages over it: the size that counts is the pages in use
# (not erased, all 0xFF). The page size is ESP-IDF's default CONFIG_SPIFFS_PAGE_SIZE.
SPIFFS = {"web_0.bin"}
SPIFFS_PAGE = 256

MARKER = "<!-- image-sizes -->"


def partitions(table_bin):
    """{label: size} from a partition-table.bin."""
    data = Path(table_bin).read_bytes()
    out = {}
    for pos in range(0, len(data) - ENTRY.size + 1, ENTRY.size):
        magic, _type, _subtype, _offset, size, label, _flags = ENTRY.unpack_from(
            data, pos
        )
        if magic != ENTRY_MAGIC:
            break
        out[label.rstrip(b"\0").decode()] = size
    return out


def bootloader_limit(build):
    args = json.loads((build / "flasher_args.json").read_text())
    return int(args["partition-table"]["offset"], 16) - int(
        args["bootloader"]["offset"], 16
    )


def image_size(path):
    data = path.read_bytes()
    if path.name not in SPIFFS:
        return len(data)
    erased = b"\xff" * SPIFFS_PAGE
    return SPIFFS_PAGE * sum(
        data[i : i + SPIFFS_PAGE] != erased for i in range(0, len(data), SPIFFS_PAGE)
    )


def collect(root):
    images = []
    for name, project, image, where in IMAGES:
        build = root / project / "build"
        path = build / image
        if not path.is_file():
            print(f"image_sizes: {path} missing, skipped", file=sys.stderr)
            continue
        limit = None
        if where == "bootloader":
            limit = bootloader_limit(build)
        elif where:
            limit = partitions(build / "partition_table" / "partition-table.bin").get(
                where
            )
            if limit is None:
                print(
                    f"image_sizes: no partition {where} in {project}", file=sys.stderr
                )
        images.append(
            {
                "name": name,
                "file": f"{project}/{image}",
                "size": image_size(path),
                "limit": limit,
                "region": where,
            }
        )
    return {"images": images}


def kib(n):
    return f"{n / 1024:,.1f} KiB"


def signed(n):
    if n == 0:
        return "0"
    sign = "+" if n > 0 else "−"
    if abs(n) < 1024:
        return f"{sign}{abs(n):,} B"
    return f"{sign}{abs(n) / 1024:,.1f} KiB"


def report(head, base=None, base_label=None):
    base_sizes = {i["name"]: i["size"] for i in (base or {}).get("images", [])}
    rows = [
        "| Image | Size | Δ | Region | Free | Used |",
        "| --- | ---: | ---: | --- | ---: | ---: |",
    ]
    for img in head["images"]:
        size, limit = img["size"], img["limit"]
        if base is None:
            delta = ""
        elif img["name"] in base_sizes:
            d = size - base_sizes[img["name"]]
            delta = f"**{signed(d)}**" if d else "0"
        else:
            delta = "new"
        if limit:
            free = kib(limit - size)
            used = f"{100 * size / limit:.1f}%"
            if size > limit:
                used = f"⚠️ {used}"
        else:
            free = used = "–"
        rows.append(
            f"| {img['name']} | {kib(size)} | {delta} | {img['region'] or '–'} | {free} | {used} |"
        )
    lines = [MARKER, "### Firmware image sizes", ""] + rows + [""]
    if base_label:
        lines.append(f"Δ against {base_label}.")
    elif base is None:
        lines.append("No base build to compare against.")
    lines.append("")
    lines.append(
        "Web UI: the SPIFFS pages in use. C6 installer: booted from the ESP-Hosted"
        " firmware's own slot, no limit here."
    )
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("collect")
    c.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    c.add_argument("--out", type=Path, required=True)
    r = sub.add_parser("report")
    r.add_argument("head", type=Path)
    r.add_argument("--base", type=Path)
    r.add_argument(
        "--base-label", help="what the base build is, e.g. 'master @ abc1234'"
    )
    args = parser.parse_args()

    if args.cmd == "collect":
        sizes = collect(args.root)
        if not sizes["images"]:
            print("image_sizes: no images found; run make build first", file=sys.stderr)
            return 1
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(sizes, indent=2) + "\n")
        return 0
    head = json.loads(args.head.read_text())
    base = (
        json.loads(args.base.read_text()) if args.base and args.base.is_file() else None
    )
    sys.stdout.write(report(head, base, args.base_label if base else None))
    return 0


if __name__ == "__main__":
    sys.exit(main())
