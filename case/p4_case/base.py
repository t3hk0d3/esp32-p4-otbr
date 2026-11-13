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
    Rectangle,
    RectangleRounded,
    Rot,
    Sketch,
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
    """Cylinder along X between x0 and x1 (either order), axis at (y, z)."""
    x0, x1 = sorted((x0, x1))
    return (
        Pos(x0, y, z)
        * Rot(0, 90, 0)
        * Cylinder(d / 2, x1 - x0, align=(Align.CENTER, Align.CENTER, Align.MIN))
    )


def x_prism(pts_yz: list[tuple[float, float]], x0: float, x1: float) -> Part:
    """A YZ outline extruded along X between x0 and x1 (either order).

    The direction is given explicitly: on Plane.YZ an extrusion otherwise follows the outline's winding.
    """
    x0, x1 = sorted((x0, x1))
    face = Plane.YZ * Polygon(*pts_yz, align=None)
    return Pos(x0, 0, 0) * extrude(face, x1 - x0, dir=(1, 0, 0))


def side_walls() -> tuple[tuple[float, float, int], ...]:
    """(outer face X, inner face X, inward direction) of the left and right side walls."""
    return (P.OUTER[0], P.INNER[0], 1), (P.OUTER[1], P.INNER[1], -1)


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
    return outer - cavity - rebate - bottom_bevel() - top_bevel()


def _edge_ring(c: float) -> Part:
    """The ring a 45° bevel of size c removes from the outer outline's edge, c tall, at the origin.

    At z 0 it reaches c into the outline, at z c it reaches the outline itself.
    """
    ox0, ox1, oy0, oy1 = P.OUTER
    outline = RectangleRounded(ox1 - ox0, oy1 - oy0, P.OUTER_R)
    keep = extrude(offset(outline, -c, kind=Kind.ARC), c + EPS, taper=-45)
    return extrude(offset(outline, 1.0, kind=Kind.ARC), c) - keep


def _outline_centre() -> tuple[float, float]:
    ox0, ox1, oy0, oy1 = P.OUTER
    return (ox0 + ox1) / 2, (oy0 + oy1) / 2


def bottom_bevel() -> Part:
    """The outer bottom edge's bevel (it prints on the bed: no elephant's-foot lip)."""
    cx, cy = _outline_centre()
    return Pos(cx, cy, board_z(0.0)) * _edge_ring(P.OUTER_BOTTOM_CHAMFER)


def top_bevel() -> Part:
    """The outer top edge's bevel; the lid subtracts it too where it reaches the outer face."""
    cx, cy = _outline_centre()
    return (
        Pos(cx, cy, board_z(P.TOP)) * Rot(180, 0, 0) * _edge_ring(P.OUTER_TOP_CHAMFER)
    )


def standoff_footprint(x: float, y: float) -> Sketch:
    """Plan of one standoff: the Ø STANDOFF_D post plus a web as wide as the post, out to its side wall."""
    return Pos(x, y) * Circle(P.STANDOFF_D / 2) + standoff_web(x, y)


def standoff_web(x: float, y: float) -> Sketch:
    """Plan of the web joining a standoff to its side wall (reaching EPS into the wall)."""
    r = P.STANDOFF_D / 2
    wall = P.INNER[0] if x < (P.INNER[0] + P.INNER[1]) / 2 else P.INNER[1]
    x0, x1 = sorted((x, wall - (EPS if wall < x else -EPS)))
    return Pos((x0 + x1) / 2, y) * Rectangle(x1 - x0, 2 * r)


def _standoffs() -> Part:
    z = board_z(P.PLATE_T) - EPS
    h = P.STANDOFF_TOP - P.PLATE_T + EPS
    c = P.STANDOFF_ROOT_CHAMFER
    parts = []
    for x, y in P.SCREW_HOLES:
        # the round post reaches the board; the web stops STANDOFF_WEB_DROP lower, so the board only bears on the pad
        parts.append(Pos(x, y, z) * extrude(Circle(P.STANDOFF_D / 2), h))
        parts.append(
            Pos(0, 0, z) * extrude(standoff_web(x, y), h - P.STANDOFF_WEB_DROP)
        )
        # 45° flare at the root: the footprint grown by c at the plate, tapering back to it c higher
        flare = offset(standoff_footprint(x, y), c + EPS, kind=Kind.ARC)
        parts.append(Pos(0, 0, z) * extrude(flare, c + EPS, taper=45))
    return Part() + parts


def _wall_root_chamfer() -> Part:
    """A 45° fillet-like chamfer along the floor's edge into all four walls, WALL_ROOT_CHAMFER wide and tall."""
    ix0, ix1, iy0, iy1 = P.INNER
    c = P.WALL_ROOT_CHAMFER
    z = board_z(P.PLATE_T) - EPS
    cavity = Pos((ix0 + ix1) / 2, (iy0 + iy1) / 2) * RectangleRounded(
        ix1 - ix0 + 2 * EPS, iy1 - iy0 + 2 * EPS, P.INNER_R + EPS
    )
    # the open floor: the cavity shrunk by c, growing back to the cavity outline c higher
    floor = offset(cavity, -(c + EPS), kind=Kind.ARC)
    return Pos(0, 0, z) * (
        extrude(cavity, c + EPS) - extrude(floor, c + EPS, taper=-45)
    )


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
    # thumb scoop right at the slot's top edge (no lip): a wedge into the outer face, deepest at the slot
    face = P.OUTER[2]
    wedge = [
        (face - EPS, sz1),
        (face + P.SCOOP_DEPTH, sz1),
        (face - EPS, sz1 + P.SCOOP_RISE),
    ]
    cuts.append(x_prism(wedge, *P.SCOOP_X))
    return Part() + cuts


def _sd_wall() -> Part:
    """The cable wall thickened SD_WALL_IN inward above the microSD slot, so 0.8 mm stays behind the scoop.

    It starts at the slot's top (a 1 mm ledge over the cavity, printed as a short bridge) and its top slopes back to
    the wall at 45°.
    """
    _, _, _, sz1 = P.SD_SLOT
    y0, t = P.INNER[2] - EPS, P.SD_WALL_IN
    z1 = sz1 + P.SCOOP_RISE + 1.0
    outline = [(y0, sz1), (y0 + t + EPS, sz1), (y0 + t + EPS, z1), (y0, z1 + t + EPS)]
    return x_prism(outline, *P.SD_WALL_X)


def _header_slot() -> Part:
    x0, x1 = P.HEADER_SLOT_X
    floor = board_z(P.HEADER_SLOT_BOTTOM)
    # starts INNER_R inside the cavity: it takes the cavity's rounded corners too, leaving no sliver against the slot
    slot = block(
        x0, x1, P.INNER[3] - P.INNER_R, P.OUTER[3] + 1, floor, board_z(P.TOP) + 1
    )
    # the groove along the floor takes the curtain's tongues
    mid, w = (P.INNER[3] + P.OUTER[3]) / 2, P.SLOT_GROOVE_W
    groove = block(
        x0, x1, mid - w / 2, mid + w / 2, floor - P.SLOT_GROOVE_D, floor + EPS
    )
    return slot + groove


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
    # round top (it prints fine), flat at the bottom, where it prints as a floor; the flat keeps the bulkhead from
    # turning
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


def tooth_outline(yc: float, grow: float = 0.0) -> list[tuple[float, float]]:
    """A keying tooth in YZ (board frame), centred at yc, grown by grow all round: flat top above the case top, 45°
    lead-ins on its lower corners."""
    h = P.TOOTH_LEN / 2 + grow
    top = board_z(P.TOP) + 1
    bottom = board_z(P.TOP - P.TOOTH_DEPTH) - grow
    lead = P.TOOTH_LEAD
    return [
        (yc - h, top),
        (yc - h, bottom + lead),
        (yc - h + lead, bottom),
        (yc + h - lead, bottom),
        (yc + h, bottom + lead),
        (yc + h, top),
    ]


def _tooth_notches() -> Part:
    """Notches in the side walls' tops, above the corner snap tabs, that take the lid's teeth (TOOTH_GAP all round)."""
    cuts = []
    for face, inner, inward in side_walls():
        for yc in tab_y_centres():
            cuts.append(
                x_prism(
                    tooth_outline(yc, P.TOOTH_GAP), face - inward, inner + inward * EPS
                )
            )
    return Part() + cuts


def build_base() -> Part:
    """The base as one solid in the board frame (back face at case Z 0)."""
    part = _shell() + _wall_root_chamfer() + _standoffs() + _sd_wall()
    part -= _screw_holes()
    part -= _keyholes()
    part -= _cable_wall_openings()
    part -= _header_slot()
    part -= _buttons()
    part -= _sma()
    part -= _dents()
    part -= _tooth_notches()
    return part
