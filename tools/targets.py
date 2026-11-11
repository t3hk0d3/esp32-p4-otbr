#!/usr/bin/env python3
"""The registry of firmware targets (board_id x chip_id): p4/targets/targets.json.

Usage: tools/targets.py [--registry FILE] resolve [<board_id> <chip_id>]
       tools/targets.py [--registry FILE] list
resolve  prints key=value lines for a target (the default target without ids): board_id, chip_id, build_dir, sdkconfig
         and defaults (';'-separated, in the order they are applied), paths relative to the repository root. The default
         target builds into p4/build and p4/sdkconfig, every other one into p4/build_<board_id>_<chip_id> and
         p4/sdkconfig_<board_id>_<chip_id>. Exits 2 for an unknown id or a pair that is not a listed target.
list     prints every target as <board_id>/<chip_id>, one per line.
"""

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REGISTRY = ROOT / "p4" / "targets" / "targets.json"
COMMON_DEFAULTS = "p4/sdkconfig.defaults"


def load(path=REGISTRY):
    """Read and check the registry; ValueError names what is wrong."""
    data = json.loads(Path(path).read_text())
    for kind in ("boards", "chips"):
        for ident in data[kind]:
            if "_" in ident:
                raise ValueError(f"{kind[:-1]} id '{ident}' contains '_'")
    for target in data["targets"] + [data["default"]]:
        if target["board"] not in data["boards"] or target["chip"] not in data["chips"]:
            raise ValueError(
                f"target {target['board']}/{target['chip']} uses an unknown id"
            )
    if data["default"] not in data["targets"]:
        raise ValueError("the default target is not in the target list")
    return data


def pairs(data):
    return [f"{t['board']}/{t['chip']}" for t in data["targets"]]


def refuse(message):
    print(f"targets.py: {message}", file=sys.stderr)
    sys.exit(2)


def resolve(data, board, chip):
    """The files of one target; SystemExit(2) when the ids are unknown or not a listed pair."""
    if board not in data["boards"] or chip not in data["chips"]:
        refuse(
            f"unknown board or chip '{board}/{chip}'\n"
            f"boards: {' '.join(data['boards'])}\n"
            f"chips: {' '.join(data['chips'])}"
        )
    if f"{board}/{chip}" not in pairs(data):
        refuse(f"'{board}/{chip}' is not a target; targets: {' '.join(pairs(data))}")
    if data["default"] == {"board": board, "chip": chip}:
        suffix = ""
    else:
        suffix = f"_{board}_{chip}"
    return {
        "board_id": board,
        "chip_id": chip,
        "build_dir": f"p4/build{suffix}",
        "sdkconfig": f"p4/sdkconfig{suffix}",
        "defaults": ";".join(
            [
                COMMON_DEFAULTS,
                f"p4/targets/boards/{board}.defaults",
                f"p4/targets/chips/{chip}.defaults",
            ]
        ),
    }


def main(argv):
    parser = argparse.ArgumentParser(
        prog="targets.py",
        usage="targets.py [--registry FILE] {resolve [<board_id> <chip_id>] | list}",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--registry", default=REGISTRY)
    sub = parser.add_subparsers(dest="command", required=True)
    resolve_parser = sub.add_parser("resolve")
    resolve_parser.add_argument("ids", nargs="*")
    sub.add_parser("list")
    args = parser.parse_args(argv)
    try:
        data = load(args.registry)
    except ValueError as error:
        print(f"targets.py: {error}", file=sys.stderr)
        return 1
    if args.command == "list":
        print("\n".join(pairs(data)))
        return 0
    if len(args.ids) not in (0, 2):
        parser.error("resolve takes <board_id> <chip_id> or nothing")
    ids = args.ids or [data["default"]["board"], data["default"]["chip"]]
    for key, value in resolve(data, *ids).items():
        print(f"{key}={value}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
