#!/usr/bin/env python3
"""Publish a release's P4 images for the browser flasher on the documentation site.

Usage: tools/flash_manifest.py <dir of downloaded zips> <firmware dir> <version>
Unpacks each full_<board_id>_<chip_id>.zip of a target in the registry into <firmware dir>/<board_id>/<chip_id>/ and
writes a manifest.json there, with one part per image line of its flash_args, paths relative to the manifest. A release
from before targets has only p4-flash.zip, which is published as the default target. Writes <firmware dir>/targets.json,
the list of what was published.
"""

import json
import sys
import zipfile
from pathlib import Path

import targets


def manifest(flash_args: str, version: str) -> dict:
    parts = []
    for line in flash_args.splitlines()[1:]:  # the first line holds the esptool options
        if line.strip():
            offset, image = line.split()
            parts.append({"path": image, "offset": int(offset, 0)})
    if not parts:
        raise ValueError("no images in flash_args")
    return {
        "name": "ESP32-P4 OpenThread Border Router",
        "version": version,
        "parts": parts,
    }


def found_zips(zips: Path, registry: dict) -> list:
    """(target, zip path) of each target the release carries, in registry order."""
    found = []
    for target in registry["targets"]:
        path = zips / f"full_{target['board']}_{target['chip']}.zip"
        if path.is_file():
            found.append((target, path))
    if not found and (zips / "p4-flash.zip").is_file():
        found.append((registry["default"], zips / "p4-flash.zip"))
    return found


def unpack(zip_path: Path, directory: Path, version: str) -> None:
    """Unpack one zip and write its manifest; ValueError names a missing image."""
    with zipfile.ZipFile(zip_path) as archive:
        archive.extractall(directory)
    data = manifest((directory / "flash_args").read_text(), version)
    for part in data["parts"]:
        if not (directory / part["path"]).is_file():
            raise ValueError(f"missing {part['path']} in {zip_path.name}")
    (directory / "manifest.json").write_text(json.dumps(data, indent=2) + "\n")


def publish(zips: Path, firmware: Path, version: str, registry: dict) -> None:
    found = found_zips(zips, registry)
    if not found:
        raise ValueError(f"no firmware in {zips}")
    index = []
    for target, zip_path in found:
        board, chip = target["board"], target["chip"]
        unpack(zip_path, firmware / board / chip, version)
        index.append(
            {
                "board": board,
                "board_name": registry["boards"][board]["name"],
                "chip": chip,
                "chip_name": registry["chips"][chip]["name"],
                "min_rev": registry["chips"][chip]["min_rev"],
                "max_rev": registry["chips"][chip]["max_rev"],
                "manifest": f"{board}/{chip}/manifest.json",
                "version": version,
            }
        )
    (firmware / "targets.json").write_text(json.dumps(index, indent=2) + "\n")


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    try:
        publish(Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], targets.load())
    except ValueError as error:
        print(f"flash_manifest.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
