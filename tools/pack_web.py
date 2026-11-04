#!/usr/bin/env python3
"""Stage the web UI for the SPIFFS image: every .html, .css and .js file is stored gzipped as <name>.gz.

The web server sends <path>.gz with Content-Encoding: gzip when it exists, so the image holds about a third of the
source bytes. Other files are copied as they are. The output is deterministic (no timestamps or names in the gzip
headers), so an unchanged frontend gives the same image.

Usage: pack_web.py <frontend dir> <output dir>
"""

import gzip
import shutil
import sys
from pathlib import Path

COMPRESSED = {".html", ".css", ".js"}
# CONFIG_SPIFFS_OBJ_NAME_LEN (ESP-IDF default 32) counts the leading "/" and the terminating NUL.
SPIFFS_OBJ_NAME_LEN = 32


def packed_name(rel):
    return rel.with_name(rel.name + ".gz") if rel.suffix in COMPRESSED else rel


def pack(src, out):
    """Write the staged tree of `src` to `out` (replaced). Returns {packed relative path: size}."""
    src, out = Path(src), Path(out)
    if out.exists():
        shutil.rmtree(out)
    sizes = {}
    for path in sorted(p for p in src.rglob("*") if p.is_file()):
        rel = packed_name(path.relative_to(src))
        name = "/" + rel.as_posix()
        if len(name) >= SPIFFS_OBJ_NAME_LEN:
            raise SystemExit(f"pack_web: {name} is longer than SPIFFS allows ({SPIFFS_OBJ_NAME_LEN - 1} bytes)")
        data = path.read_bytes()
        if rel != path.relative_to(src):
            data = gzip.compress(data, compresslevel=9, mtime=0)
        target = out / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        sizes[rel.as_posix()] = len(data)
    return sizes


def main(argv):
    if len(argv) != 3:
        raise SystemExit(__doc__.strip().splitlines()[-1])
    pack(argv[1], argv[2])


if __name__ == "__main__":
    main(sys.argv)
