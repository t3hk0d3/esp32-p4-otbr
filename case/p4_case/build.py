"""Command line: build the case, write STL/STEP files and previews, run the checks.

    python -m p4_case.build --board .artifacts/board/board.stp --out dist/case [--check]

STL files are in print pose (base: back plate on the bed; lid: top face on the bed). STEP files are in the case frame
as assembled (case Z 0 at the outer back face). Exit codes: 0 all checks pass (or --check not given), 1 a check failed
with --check, 2 bad input.
"""

import argparse
import sys
from pathlib import Path

import trimesh
from build123d import Part, Pos, export_step

from p4_case import params as P
from p4_case.base import build_base
from p4_case.board import MAKE_HINT, load_board
from p4_case.checks import board_bodies, print_pose, run_all, to_mesh
from p4_case.lid import build_lid
from p4_case.render import render

LID_LIFT = 25.0  # exploded preview: how far the lid floats above the base


def write_outputs(base: Part, lid: Part, out: Path) -> list[Path]:
    """Writes the model files and preview PNGs; returns their paths."""
    out.mkdir(parents=True, exist_ok=True)
    to_case = Pos(0, 0, P.BOARD_Z_IN_CASE)
    written = []
    for name, part, top_down in (("base", base, False), ("lid", lid, True)):
        stl = out / f"p4_case_{name}.stl"
        print_pose(part, top_down).export(stl)
        step = out / f"p4_case_{name}.step"
        export_step(to_case * part, step)
        written += [stl, step]

    base_m, lid_m = to_mesh(base), to_mesh(lid)
    board_m = trimesh.util.concatenate([to_mesh(b) for b in board_bodies()])
    lifted = lid_m.copy()
    lifted.apply_translation((0, 0, LID_LIFT))
    scenes = {
        "front": [(base_m, "base"), (lid_m, "lid")],
        "back": [(base_m, "base")],
        "cable_end": [(base_m, "base"), (lid_m, "lid"), (board_m, "board")],
        "header_end": [(base_m, "base"), (lid_m, "lid"), (board_m, "board")],
        "button_side": [(base_m, "base"), (lid_m, "lid"), (board_m, "board")],
        "exploded": [(base_m, "base"), (board_m, "board"), (lifted, "lid")],
    }
    for view, meshes in scenes.items():
        written.append(render(meshes, view, out / f"preview_{view}.png"))
    return written


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="p4_case.build", description=__doc__.splitlines()[0]
    )
    ap.add_argument(
        "--board", type=Path, required=True, help="vendor STEP (make case-board)"
    )
    ap.add_argument("--out", type=Path, required=True, help="output directory")
    ap.add_argument("--check", action="store_true", help="exit 1 if any check fails")
    args = ap.parse_args(argv)
    if not args.board.is_file():
        print(f"{args.board} not found; {MAKE_HINT}", file=sys.stderr)
        return 2
    board = load_board(args.board)
    base, lid = build_base(), build_lid()
    for path in write_outputs(base, lid, args.out):
        print(f"wrote {path}")
    results = run_all(board, base, lid)
    for r in results:
        print(f"{'PASS' if r.ok else 'FAIL'} {r.name}: {r.detail}")
    failed = [r for r in results if not r.ok]
    print(f"{len(results) - len(failed)}/{len(results)} checks pass")
    return 1 if failed and args.check else 0


if __name__ == "__main__":
    sys.exit(main())
