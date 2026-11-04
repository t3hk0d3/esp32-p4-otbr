#!/usr/bin/env python3
"""Write the ESP Web Tools manifest for an unpacked p4-flash.zip (the browser flasher on the documentation site).

Usage: tools/flash_manifest.py <unpacked p4-flash.zip dir> <version>
Writes <dir>/manifest.json with one part per image line of <dir>/flash_args, paths relative to the manifest.
"""

import json
import sys
from pathlib import Path


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
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": "ESP32-P4", "parts": parts}],
    }


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    directory = Path(sys.argv[1])
    data = manifest((directory / "flash_args").read_text(), sys.argv[2])
    for part in data["builds"][0]["parts"]:
        if not (directory / part["path"]).is_file():
            print(f"missing {part['path']}", file=sys.stderr)
            return 1
    (directory / "manifest.json").write_text(json.dumps(data, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
