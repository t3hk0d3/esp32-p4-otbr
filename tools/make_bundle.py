#!/usr/bin/env python3
"""Build or inspect an OTBR OTA bundle (spec 5.6, R14: web image and P4 app image only).

Bundle = header | web image (trailing 0xFF trimmed) | P4 app image
header = magic u32 | format_version u16 | reserved u16 | web_len u32 | web_sha256[32]   (44 bytes, little-endian)
"""

import argparse
import hashlib
import struct
import sys
from pathlib import Path

MAGIC = 0x4242544F  # "OTBB"
FORMAT_VERSION = 1
HEADER = struct.Struct("<IHHI32s")


def build(web_path, app_path, out_path):
    web = Path(web_path).read_bytes().rstrip(b"\xff")
    app = Path(app_path).read_bytes()
    if not web:
        print("make_bundle: the web image is empty (all 0xFF)", file=sys.stderr)
        return 1
    if not app:
        print("make_bundle: the app image is empty", file=sys.stderr)
        return 1
    header = HEADER.pack(
        MAGIC, FORMAT_VERSION, 0, len(web), hashlib.sha256(web).digest()
    )
    Path(out_path).write_bytes(header + web + app)
    return 0


def inspect(bundle_path):
    data = Path(bundle_path).read_bytes()
    if len(data) < HEADER.size:
        print("make_bundle: shorter than the 44-byte header", file=sys.stderr)
        return 1
    magic, version, reserved, web_len, sha = HEADER.unpack_from(data)
    if magic != MAGIC or version != FORMAT_VERSION:
        print(
            f"make_bundle: not a bundle (magic 0x{magic:08x}, format_version {version})",
            file=sys.stderr,
        )
        return 1
    app_len = len(data) - HEADER.size - web_len
    if app_len <= 0:
        print(
            "make_bundle: truncated bundle (no app image after the web image)",
            file=sys.stderr,
        )
        return 1
    web_ok = hashlib.sha256(data[HEADER.size : HEADER.size + web_len]).digest() == sha
    print(f"magic: 0x{magic:08x}")
    print(f"format_version: {version}")
    print(f"reserved: {reserved}")
    print(f"web_len: {web_len}")
    print(f"web_sha256: {sha.hex()}")
    print(f"app_len: {app_len}")
    print(f"web_sha256_matches: {'yes' if web_ok else 'no'}")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--web", help="web SPIFFS image, e.g. p4/build/web_0.bin")
    parser.add_argument("--app", help="P4 app image, e.g. p4/build/p4.bin")
    parser.add_argument("--out", help="bundle to write")
    parser.add_argument(
        "--inspect", metavar="BUNDLE", help="print the header fields of a bundle"
    )
    args = parser.parse_args(argv)
    if args.inspect:
        return inspect(args.inspect)
    if not (args.web and args.app and args.out):
        parser.error("--web, --app and --out are required (or --inspect <bundle>)")
    return build(args.web, args.app, args.out)


if __name__ == "__main__":
    sys.exit(main())
