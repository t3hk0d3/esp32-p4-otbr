"""The base: back plate, four walls, standoffs and every opening (spec sections 5.1, 5.1a, 5.2). Board frame."""

import math

from build123d import (
    Align,
    Box,
    Circle,
    Cone,
    Cylinder,
    Kind,
    Part,
    Plane,
    Polygon,
    Pos,
    RectangleRounded,
    Rot,
    SlotCenterToCenter,
    extrude,
    offset,
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


def _neck_lobes(x: float, y: float) -> list:
    """Round lobes narrowing both arms of a keyhole to KEY_NECK_W over about KEY_NECK_LEN."""
    r = P.KEY_NECK_LEN / 2 + 0.1
    off = P.KEY_NECK_W / 2 + r  # lobe centres sit just outside the neck
    at = P.KEY_NECK_AT
    centres = [
        (x - off, y + at),
        (x + off, y + at),
        (x + at, y - off),
        (x + at, y + off),
    ]
    return [Pos(cx, cy) * Circle(r) for cx, cy in centres]


def _keyholes() -> Part:
    cut = []
    sep = P.KEY_ARM_L
    for x, y in P.KEYHOLES:
        entry = Pos(x, y) * Circle(P.KEY_ENTRY_D / 2)
        arm_y = (
            Pos(x, y + sep / 2) * Rot(0, 0, 90) * SlotCenterToCenter(sep, P.KEY_ARM_W)
        )
        arm_x = Pos(x + sep / 2, y) * SlotCenterToCenter(sep, P.KEY_ARM_W)
        outline = entry + arm_y + arm_x
        profile = outline
        for lobe in _neck_lobes(x, y):
            profile -= lobe
        cut.append(Pos(0, 0, board_z(0.0) - 1) * extrude(profile, P.PLATE_T + 2))
        # 45° chamfer at the back face (the bed side): the outline (without the neck) grown by KEY_CHAMFER, tapering
        # back to it; it trims the neck lobes' first KEY_CHAMFER too, so squish can't narrow the neck either.
        # It starts EPS below the face and ends EPS inside the bore, so its cone crosses the bore wall cleanly.
        c = P.KEY_CHAMFER
        mouth = offset(outline, c + EPS, kind=Kind.ARC)
        cut.append(
            Pos(0, 0, board_z(0.0) - EPS) * extrude(mouth, c + 3 * EPS, taper=45)
        )
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
    # thumb scoop above the slot's top edge, past a SCOOP_LIP lip: a wedge into the outer face (YZ profile, along X)
    face = P.OUTER[2]
    z0 = sz1 + P.SCOOP_LIP
    wedge = [
        (face - EPS, z0),
        (face + P.SCOOP_DEPTH, z0),
        (face - EPS, z0 + P.SCOOP_RISE),
    ]
    sx_a, sx_b = P.SCOOP_X
    prism = extrude(
        Plane.YZ * Polygon(*wedge, align=None), (sx_b - sx_a) / 2, both=True
    )
    cuts.append(Pos((sx_a + sx_b) / 2, 0, 0) * prism)
    return Part() + cuts


def _header_slot() -> Part:
    x0, x1 = P.HEADER_SLOT_X
    # starts INNER_R inside the cavity: it takes the cavity's rounded corners too, leaving no sliver against the slot
    return block(
        x0,
        x1,
        P.INNER[3] - P.INNER_R,
        P.OUTER[3] + 1,
        board_z(P.HEADER_SLOT_BOTTOM),
        board_z(P.TOP) + 1,
    )


def _buttons() -> Part:
    a, b = FACTS["button_a"], FACTS["button_b"]
    m = P.BUTTON_RECESS_MARGIN
    # recess into the wall's inner face; its ceiling slopes up at 45° to the face, so it needs no bridge
    x0, x1 = P.BUTTON_RECESS_X, P.INNER[0] + EPS
    z0, z1 = a.z0 - m, a.z1 + m
    profile = [(x0, z0), (x1, z0), (x1, z1 + (x1 - x0)), (x0, z1)]
    span = b.y1 - a.y0 + 2 * m
    recess = extrude(Plane.XZ * Polygon(*profile, align=None), span / 2, both=True)
    cuts = [Pos(0, (a.y0 + b.y1) / 2, 0) * recess]
    for btn in (a, b):
        yc, zc = (btn.y0 + btn.y1) / 2, (btn.z0 + btn.z1) / 2
        cuts.append(
            x_cylinder(yc, zc, P.BUTTON_HOLE_D, P.OUTER[0] - 1, P.INNER[0] + EPS)
        )
        depth = (
            P.BUTTON_FUNNEL_D - P.BUTTON_HOLE_D
        ) / 2  # 90 degrees: depth equals the radius step
        funnel = Cone(
            P.BUTTON_FUNNEL_D / 2,
            P.BUTTON_HOLE_D / 2,
            depth + EPS,
            align=(Align.CENTER, Align.CENTER, Align.MIN),
        )
        cuts.append(Pos(P.OUTER[0] - EPS, yc, zc) * Rot(0, 90, 0) * funnel)
    return Part() + cuts


def _sma() -> Part:
    zc = board_z(P.SMA_CASE_Z)
    r = P.SMA_D / 2
    # the hole stops short of the outer face: a two-line knock-out closes it until an SMA is fitted
    x0, x1 = P.INNER[1] - EPS, P.OUTER[1] - P.SMA_KNOCKOUT_T
    hole = x_cylinder(P.SMA_Y, zc, P.SMA_D, x0, x1)
    # teardrop top: a 45° roof tangent to the circle, so the top prints without support
    roof = [
        (P.SMA_Y - r / math.sqrt(2), zc + r / math.sqrt(2)),
        (P.SMA_Y, zc + r * math.sqrt(2)),
        (P.SMA_Y + r / math.sqrt(2), zc + r / math.sqrt(2)),
        (P.SMA_Y, zc),
    ]
    # extrudes toward -X
    hole += Pos(x1, 0, 0) * extrude(Plane.YZ * Polygon(*roof, align=None), x1 - x0)
    # the flat is at the bottom, where it prints as a floor; it keeps the bulkhead from turning
    flat_z = zc - (P.SMA_FLAT - r)
    keep = block(x0, x1, P.SMA_Y - P.SMA_D, P.SMA_Y + P.SMA_D, flat_z, zc + P.SMA_D)
    return hole & keep


def tab_y_centres() -> tuple[float, float]:
    """Y centres of the lid's corner tabs (and of the dents that take their bumps)."""
    half = P.TAB_LEN / 2
    return P.INNER[2] + P.TAB_CLEAR + half, P.INNER[3] - P.TAB_CLEAR - half


def _dents() -> Part:
    """Dents shaped like the bumps (lead face below, hold face above), DENT_PRELOAD lower than the bump crests."""
    z = board_z(P.BUMP_Z - P.DENT_PRELOAD)
    d = P.DENT_DEPTH
    # the cut starts this far inside the cavity, continuing both faces' slopes
    over = 0.2
    lead = (d + over) / math.tan(math.radians(P.BUMP_LEAD_ANGLE))
    hold = (d + over) / math.tan(math.radians(P.BUMP_HOLD_ANGLE))
    cuts = []
    for yc in tab_y_centres():
        for face_x, out in ((P.INNER[0], -1), (P.INNER[1], 1)):
            pts = [
                (face_x - out * over, z - lead),
                (face_x + out * d, z),
                (face_x - out * over, z + hold),
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
    part -= _buttons()
    part -= _sma()
    part -= _dents()
    return part
