"""The base: back plate, four walls, standoffs and every opening (spec sections 5.1, 5.1a, 5.2). Board frame."""

import math

from build123d import (
    Align,
    Box,
    Circle,
    Cone,
    Cylinder,
    Part,
    Plane,
    Polygon,
    Pos,
    RectangleRounded,
    Rot,
    SlotCenterToCenter,
    extrude,
)

from p4_case import params as P
from p4_case.board import FACTS
from p4_case.params import board_z

EPS = 0.01  # overlap so cuts never leave zero-thickness skins
MIN = (Align.MIN, Align.MIN, Align.MIN)


def rounded_block(
    x0: float, x1: float, y0: float, y1: float, z0: float, z1: float, r: float
) -> Part:
    """Vertical prism over a rounded rectangle, board frame."""
    profile = RectangleRounded(x1 - x0, y1 - y0, r)
    return Pos((x0 + x1) / 2, (y0 + y1) / 2, z0) * extrude(profile, z1 - z0)


def block(x0: float, x1: float, y0: float, y1: float, z0: float, z1: float) -> Part:
    return Pos(x0, y0, z0) * Box(x1 - x0, y1 - y0, z1 - z0, align=MIN)


def x_cylinder(y: float, z: float, d: float, x0: float, x1: float) -> Part:
    """Cylinder along X from x0 to x1, axis at (y, z)."""
    return (
        Pos(x0, y, z)
        * Rot(0, 90, 0)
        * Cylinder(d / 2, x1 - x0, align=(Align.CENTER, Align.CENTER, Align.MIN))
    )


def _shell() -> Part:
    ox0, ox1, oy0, oy1 = P.OUTER
    ix0, ix1, iy0, iy1 = P.INNER
    top = board_z(P.TOP)
    outer = rounded_block(ox0, ox1, oy0, oy1, board_z(0.0), top, P.OUTER_R)
    cavity = rounded_block(ix0, ix1, iy0, iy1, board_z(P.PLATE_T), top + 1, P.INNER_R)
    w = P.REBATE_W
    rebate = rounded_block(
        ix0 - w,
        ix1 + w,
        iy0 - w,
        iy1 + w,
        board_z(P.REBATE_FLOOR),
        top + 1,
        P.INNER_R + w,
    )
    return outer - cavity - rebate


def _standoffs() -> Part:
    h = P.STANDOFF_TOP - P.PLATE_T
    posts = [
        Pos(x, y, board_z(P.PLATE_T) - EPS)
        * Cylinder(
            P.STANDOFF_D / 2, h + EPS, align=(Align.CENTER, Align.CENTER, Align.MIN)
        )
        for x, y in P.SCREW_HOLES
    ]
    return Part() + posts


def _screw_holes() -> Part:
    cut = []
    csk_h = (P.CSK_D - P.SCREW_HOLE_D) / 2 / math.tan(math.radians(P.CSK_ANGLE / 2))
    for x, y in P.SCREW_HOLES:
        bore = Pos(x, y, board_z(0.0) - 1) * Cylinder(
            P.SCREW_HOLE_D / 2,
            P.STANDOFF_TOP + 2,
            align=(Align.CENTER, Align.CENTER, Align.MIN),
        )
        csk = Pos(x, y, board_z(0.0) - EPS) * Cone(
            P.CSK_D / 2,
            P.SCREW_HOLE_D / 2,
            csk_h + EPS,
            align=(Align.CENTER, Align.CENTER, Align.MIN),
        )
        cut += [bore, csk]
    return Part() + cut


def _keyholes() -> Part:
    cut = []
    sep = P.KEY_ARM_L
    for x, y in P.KEYHOLES:
        entry = Pos(x, y) * Circle(P.KEY_ENTRY_D / 2)
        arm_y = (
            Pos(x, y + sep / 2) * Rot(0, 0, 90) * SlotCenterToCenter(sep, P.KEY_ARM_W)
        )
        arm_x = Pos(x + sep / 2, y) * SlotCenterToCenter(sep, P.KEY_ARM_W)
        profile = entry + arm_y + arm_x
        cut.append(Pos(0, 0, board_z(0.0) - 1) * extrude(profile, P.PLATE_T + 2))
    return Part() + cut


def _cable_wall_openings() -> Part:
    y0, y1 = P.OUTER[2] - 1, P.INNER[2] + EPS
    sx0, sx1, sz0, sz1 = P.SD_SLOT
    cx0, cx1 = P.CABLE_OPENING_X
    cuts = [
        # one opening for USB and RJ45, open to the wall top: no thin pillar, no bridge; the lid's lintel closes it
        block(cx0, cx1, y0, y1, P.OPENING_BOTTOM, board_z(P.TOP) + 1),
        block(sx0, sx1, y0, y1, sz0, sz1),
    ]
    scoop_y = P.OUTER[2] - (P.SCOOP_R - P.SCOOP_DEPTH)
    cuts.append(x_cylinder(scoop_y, sz0, 2 * P.SCOOP_R, *P.SCOOP_X))
    return Part() + cuts


def _header_slot() -> Part:
    x0, x1 = P.HEADER_SLOT_X
    return block(
        x0,
        x1,
        P.INNER[3] - EPS,
        P.OUTER[3] + 1,
        board_z(P.HEADER_SLOT_BOTTOM),
        board_z(P.TOP) + 1,
    )


def _ribbon_recesses() -> Part:
    r = P.RIBBON_RECESS
    ix0, ix1, _, iy1 = P.INNER
    z0, z1 = 0.0, P.RIBBON_RECESS_TOP
    # the ceiling slopes back to the wall face, so it prints without support
    rise = z1 + P.RECESS_CHAMFER_RISE * r
    y0, y1 = P.RIBBON_RECESS_Y0, iy1 + r
    cuts = [rounded_block(ix0 - r, ix1 + r, y0, y1, z0, z1, P.RIBBON_RECESS_CORNER_R)]
    for face_x, out in ((ix0, -1), (ix1, 1)):
        pts = [
            (face_x - out * EPS, z0),
            (face_x + out * r, z0),
            (face_x + out * r, z1),
            (face_x - out * EPS, rise),
        ]
        prism = extrude(Plane.XZ * Polygon(*pts, align=None), (y1 - y0) / 2, both=True)
        cuts.append(Pos(0, (y0 + y1) / 2, 0) * prism)
    pts = [(iy1 - EPS, z0), (iy1 + r, z0), (iy1 + r, z1), (iy1 - EPS, rise)]
    x0, x1 = ix0 - r, ix1 + r
    prism = extrude(Plane.YZ * Polygon(*pts, align=None), (x1 - x0) / 2, both=True)
    cuts.append(Pos((x0 + x1) / 2, 0, 0) * prism)
    # keep the recess inside its rounded outline, so the wall stays 1 mm thick round the outer corners
    keep = rounded_block(
        ix0 - r, ix1 + r, y0, y1, z0 - 1, rise + 1, P.RIBBON_RECESS_CORNER_R
    )
    return (Part() + cuts) & keep


def _buttons() -> Part:
    a, b = FACTS["button_a"], FACTS["button_b"]
    m = P.BUTTON_RECESS_MARGIN
    cuts = [
        block(
            P.BUTTON_RECESS_X, P.INNER[0] + EPS, a.y0 - m, b.y1 + m, a.z0 - m, a.z1 + m
        )
    ]
    for btn in (a, b):
        yc, zc = (btn.y0 + btn.y1) / 2, (btn.z0 + btn.z1) / 2
        cuts.append(
            x_cylinder(yc, zc, P.BUTTON_HOLE_D, P.OUTER[0] - 1, P.INNER[0] + EPS)
        )
    return Part() + cuts


def _sma() -> Part:
    zc = board_z(P.SMA_CASE_Z)
    x0, x1 = P.INNER[1] - EPS, P.OUTER[1] + 1
    hole = x_cylinder(P.SMA_Y, zc, P.SMA_D, x0, x1)
    flat_z = zc + P.SMA_FLAT - P.SMA_D / 2
    keep = block(x0, x1, P.SMA_Y - P.SMA_D, P.SMA_Y + P.SMA_D, zc - P.SMA_D, flat_z)
    return hole & keep


def tab_y_centres() -> tuple[float, float]:
    """Y centres of the lid's corner tabs (and of the dents that take their bumps)."""
    half = P.TAB_LEN / 2
    return P.INNER[2] + P.TAB_CLEAR + half, P.INNER[3] - P.TAB_CLEAR - half


def _dents() -> Part:
    z = board_z(P.BUMP_Z)
    d = P.DENT_DEPTH
    flank = d / math.tan(math.radians(P.BUMP_ANGLE))
    cuts = []
    for yc in tab_y_centres():
        for face_x, out in ((P.INNER[0], -1), (P.INNER[1], 1)):
            pts = [
                (face_x - out * 0.2, z - flank - 0.2),
                (face_x + out * d, z),
                (face_x - out * 0.2, z + flank + 0.2),
            ]
            prism = extrude(
                Plane.XZ * Polygon(*pts, align=None), P.DENT_LEN / 2, both=True
            )
            cuts.append(Pos(0, yc, 0) * prism)
    return Part() + cuts


def build_base() -> Part:
    """The base as one solid in the board frame (back face at case Z 0)."""
    part = _shell() + _standoffs()
    part -= _screw_holes()
    part -= _keyholes()
    part -= _cable_wall_openings()
    part -= _header_slot()
    part -= _ribbon_recesses()
    part -= _buttons()
    part -= _sma()
    part -= _dents()
    return part
