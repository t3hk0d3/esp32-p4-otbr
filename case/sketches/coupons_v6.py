"""Throwaway test coupons for the v6 screw ears: option O (outside) and option I (inside, buttressed).

Each coupon is a 20 mm slice of the left side wall (from case Z 14 to the v6 top, with the rebate) and the matching
strip of the 3 mm v6 lid, with one ear at the slice's middle. The wall prints upright like the base; the lid strip
prints top face down like the lid. STLs are written in print pose.

Run: PYTHONPATH=case python case/sketches/coupons_v6.py <out_dir>
"""

import math
import sys
from pathlib import Path

from build123d import Align, Cone, Cylinder, Part, Plane, Polygon, Pos, Rot, extrude

from p4_case import params as P
from p4_case.base import EPS, block

TOP = P.TOP + 1.0  # 28.6
REBATE_FLOOR = P.REBATE_FLOOR + 1.0  # 27.6
UNDER = P.LID_UNDERSIDE  # 25.6
OX, IX = P.OUTER[0], P.INNER[0]  # -4.5, -0.5
Y0, Y1 = 22.0, 42.0
EAR_Y = 32.0
CUT_Z = 14.0  # the wall slice starts here
LID_X1 = 16.0  # lid strip reaches this far in

# option O
O_W = 10.0
O_T = 1.6
O_Z = 22.3
O_GAP = 0.2
O_CLEAR_D = 2.9
O_CSK_D = 5.0
O_PILOT_D = 2.2

# option I
I_LEN = 12.0
I_T = 5.0
I_BOTTOM = 18.5
I_GUSSET = 3.0
I_Z = 22.0
I_PILOT_D = 2.4
I_PILOT_STOP = 0.4
I_CLEAR_D = 3.0
I_CSK_D = 5.0


def x_rod(y: float, z: float, d: float, x0: float, x1: float) -> Part:
    return Pos(x0, y, z) * Rot(0, 90, 0) * Cylinder(d / 2, x1 - x0, align=(Align.CENTER, Align.CENTER, Align.MIN))


def x_cone(y: float, z: float, d0: float, d1: float, x0: float) -> Part:
    """90° countersink opening toward -X: diameter d0 at x0, narrowing to d1 going +X."""
    h = (d0 - d1) / 2
    return Pos(x0 - EPS, y, z) * Rot(0, 90, 0) * Cone(d0 / 2, d1 / 2, h + EPS, align=(Align.CENTER, Align.CENTER,
                                                                                         Align.MIN))


def u_prism(x0: float, x1: float, r: float, zc: float, top: float) -> Part:
    """Along X from x0 to x1: a U outline in YZ (half-circle bottom of radius r around (EAR_Y, zc), sides up to top)."""
    arc = [(EAR_Y + r * math.cos(a), zc + r * math.sin(a)) for a in [math.pi + k * math.pi / 48 for k in range(49)]]
    pts = [(EAR_Y - r, top), *arc, (EAR_Y + r, top)]
    # the extrusion direction is given explicitly: on Plane.YZ it otherwise follows the outline's winding
    return Pos(x0, 0, 0) * extrude(Plane.YZ * Polygon(*pts, align=None), x1 - x0, dir=(1, 0, 0))


def wall_slice() -> Part:
    wall = block(OX, IX, Y0, Y1, CUT_Z, TOP)
    rebate = block(IX - P.REBATE_W, IX + 1, Y0 - 1, Y1 + 1, REBATE_FLOOR, TOP + 1)
    return wall - rebate


def lid_strip() -> Part:
    flange = block(IX - P.REBATE_W + P.LID_CLEAR, LID_X1, Y0, Y1, REBATE_FLOOR, TOP)
    body = block(IX + P.LID_CLEAR, LID_X1, Y0, Y1, UNDER, REBATE_FLOOR + EPS)
    return flange + body


def option_o() -> tuple[Part, Part]:
    half = O_W / 2
    base = wall_slice()
    base -= block(OX - 1, IX + 1, EAR_Y - half - O_GAP, EAR_Y + half + O_GAP, UNDER, TOP + 1)  # wall-top cut
    base -= u_prism(OX - 1, OX + O_T, half + O_GAP, O_Z, TOP + 1)  # pocket
    base -= x_rod(EAR_Y, O_Z, O_PILOT_D, OX + O_T - EPS, IX + 1)  # pilot through the wall
    lid = lid_strip()
    lid += block(OX, IX + P.LID_CLEAR + EPS, EAR_Y - half, EAR_Y + half, UNDER, TOP)  # fill over the cut
    lid += u_prism(OX, OX + O_T, half, O_Z, UNDER + EPS)  # the ear
    lid -= x_rod(EAR_Y, O_Z, O_CLEAR_D, OX - 1, OX + O_T + 1)
    lid -= x_cone(EAR_Y, O_Z, O_CSK_D, O_CLEAR_D, OX)
    return base, lid


def option_i() -> tuple[Part, Part]:
    base = wall_slice()
    base -= x_rod(EAR_Y, I_Z, I_CLEAR_D, OX - 1, IX + 1)
    base -= x_cone(EAR_Y, I_Z, I_CSK_D, I_CLEAR_D, OX)
    ex = IX + P.TAB_CLEAR
    y0, y1 = EAR_Y - I_LEN / 2, EAR_Y + I_LEN / 2
    depth = UNDER - I_BOTTOM
    # ear and buttress: XZ trapezoid, along Y
    section = [(ex, UNDER + EPS), (ex + I_T + depth, UNDER + EPS), (ex + I_T, I_BOTTOM), (ex, I_BOTTOM)]
    ear = Pos(0, EAR_Y, 0) * extrude(Plane.XZ * Polygon(*section, align=None), I_LEN / 2, both=True)
    # 45° end gussets under the lid, across the ear's thickness
    g = I_GUSSET
    for y_end, out in ((y0, -1), (y1, 1)):
        tri = [(y_end, UNDER + EPS), (y_end + out * g, UNDER + EPS), (y_end, UNDER - g)]
        ear += Pos(ex, 0, 0) * extrude(Plane.YZ * Polygon(*tri, align=None), I_T, dir=(1, 0, 0))
    lid = lid_strip() + ear
    lid -= x_rod(EAR_Y, I_Z, I_PILOT_D, ex - 1, ex + I_T - I_PILOT_STOP)
    return base, lid


def main() -> None:
    from p4_case.checks import print_pose

    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for name, (base, lid) in (("O-outside", option_o()), ("I-inside", option_i())):
        for part_name, part, top_down in (("wall", base, False), ("lid", lid, True)):
            assert part.is_valid and len(part.solids()) == 1, (name, part_name)
            path = out / f"coupon_{name}_{part_name}.stl"
            print_pose(part, top_down).export(path)
            print(f"wrote {path}")


if __name__ == "__main__":
    main()
