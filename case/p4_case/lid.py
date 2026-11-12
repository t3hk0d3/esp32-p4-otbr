"""The lid: stepped grille plate, four wedged corner tabs, the 40-pin curtain and two screw ears (spec section 5.3).

Board frame, as fitted on the base.
"""

import math

from build123d import (
    Axis,
    Face,
    Part,
    Plane,
    Polygon,
    Pos,
    Rectangle,
    RegularPolygon,
    chamfer,
    extrude,
)

from p4_case import params as P
from p4_case.base import (
    EPS,
    block,
    rounded_block,
    side_walls,
    tab_y_centres,
    top_bevel,
    u_outline,
    x_countersink,
    x_cylinder,
    x_prism,
)
from p4_case.params import board_z

IX0, IX1, IY0, IY1 = P.INNER
UNDER = board_z(P.LID_UNDERSIDE)
FLOOR = board_z(P.REBATE_FLOOR)
TOP = board_z(P.TOP)
TIP = UNDER - P.TAB_DROP
CELL_R = P.CELL / math.sqrt(3)  # circumradius of a hex with CELL across flats


def _flange_outline() -> tuple[float, float, float, float, float]:
    g = P.REBATE_W - P.LID_CLEAR
    return IX0 - g, IX1 + g, IY0 - g, IY1 + g, P.INNER_R + g


def _body_outline() -> tuple[float, float, float, float, float]:
    c = P.LID_CLEAR
    return IX0 + c, IX1 - c, IY0 + c, IY1 - c, P.INNER_R - c


def _plate() -> Part:
    fx0, fx1, fy0, fy1, fr = _flange_outline()
    bx0, bx1, by0, by1, br = _body_outline()
    flange = rounded_block(fx0, fx1, fy0, fy1, FLOOR, TOP, fr)
    # the top face prints on the bed: chamfer its edge so elephant's foot doesn't widen the flange
    top_edges = flange.faces().sort_by(Axis.Z)[-1].outer_wire().edges()
    flange = chamfer(top_edges, P.LID_TOP_CHAMFER)
    body = rounded_block(bx0, bx1, by0, by1, UNDER, FLOOR + EPS, br)
    # at the header end the base is square above the slot floor, so the body's corners are square there
    return flange + body + _header_corners(UNDER, FLOOR + EPS)


def _header_corners(z0: float, z1: float) -> Part:
    """The body outline's square header-end corners, between z0 and z1 (the 40-pin slot squares the base there)."""
    bx0, bx1, _, by1, _ = _body_outline()
    return block(bx0, bx1, by1 - P.INNER_R, by1, z0, z1)


def inside_ear_footprints() -> list[tuple[float, float, float, float]]:
    """(x0, x1, y0, y1) of each inside ear on the lid's underside: block, buttress and end gussets."""
    reach = P.TAB_CLEAR + P.IN_EAR_T + (P.LID_UNDERSIDE - P.IN_EAR_BOTTOM)
    half = P.IN_EAR_LEN / 2 + P.IN_EAR_GUSSET
    y0, y1 = P.EAR_Y - half, P.EAR_Y + half
    return [(IX0, IX0 + reach, y0, y1), (IX1 - reach, IX1, y0, y1)]


def _over_footprint(x: float, y: float) -> bool:
    for x0, x1, y0, y1 in inside_ear_footprints():
        dx = max(x0 - x, 0.0, x - x1)
        dy = max(y0 - y, 0.0, y - y1)
        if math.hypot(dx, dy) < CELL_R:
            return True
    return False


def cell_centres(ears: str = P.EAR_STYLE) -> list[tuple[float, float]]:
    """Honeycomb cell centres, centred on the lid and symmetric about the cross rib, all inside the solid frame.

    With inside ears, cells over an ear's footprint are left out, so the grille doesn't cut into the ears.
    """
    fx0, fx1, fy0, fy1, _ = _flange_outline()
    gx0, gx1 = fx0 + P.FRAME_W, fx1 - P.FRAME_W
    gy0, gy1 = fy0 + P.FRAME_W, fy1 - P.FRAME_W
    cx, cy = (gx0 + gx1) / 2, (gy0 + gy1) / 2
    pitch_y = P.CELL + P.CELL_WALL
    pitch_x = 1.5 * pitch_y / math.sqrt(3)
    half_x, half_y = CELL_R, P.CELL / 2
    rib_keep = P.RIB_W / 2 + half_y
    cols = int((gx1 - gx0 - 2 * half_x) / pitch_x) + 1
    cols -= 1 - cols % 2  # odd, so the honeycomb can mirror about the centre column
    out = []
    for i in range(cols):
        d = i - (cols - 1) // 2
        x = cx + d * pitch_x
        # columns alternate between a cell on the centre line and cells half a pitch off it
        offset = 0.0 if d % 2 == 0 else pitch_y / 2
        k = 0
        while True:
            ys = {cy + offset + k * pitch_y, cy - offset - k * pitch_y}
            inside = [y for y in ys if y - half_y >= gy0 and y + half_y <= gy1]
            if not inside:
                break
            out += [(x, y) for y in inside if abs(y - P.RIB_Y) >= rib_keep]
            k += 1
    if ears == "inside":
        out = [(x, y) for x, y in out if not _over_footprint(x, y)]
    return sorted(set(out))


def _grille(ears: str) -> Part:
    cells = [Pos(x, y) * RegularPolygon(CELL_R, 6) for x, y in cell_centres(ears)]
    sketch = cells[0]
    for c in cells[1:]:
        sketch += c
    return Pos(0, 0, UNDER - 1) * extrude(sketch, TOP - UNDER + 2)


def _tab(side: int, end: int) -> Part:
    """One corner tab. side -1/+1: left/right wall; end -1/+1: cable/header end."""
    x_face = IX0 + P.TAB_CLEAR if side < 0 else IX1 - P.TAB_CLEAR
    inward = -side
    y_end = IY0 + P.TAB_CLEAR if end < 0 else IY1 - P.TAB_CLEAR
    along = -end  # from the end face toward the middle of the case
    y_far = y_end + along * P.TAB_LEN

    def xp(u: float) -> float:
        return x_face + inward * u

    def yp(u: float) -> float:
        return y_end + along * u

    flat = UNDER - P.TAB_FLAT
    # cross-section across the side wall (XZ): flat wall face, then lead-in; inner face tapers
    xz = [
        (xp(0), UNDER + EPS),
        (xp(P.TAB_ROOT_T), UNDER + EPS),
        (xp(P.TAB_TIP_T), TIP),
        (xp(P.TAB_LEAD), TIP),
        (xp(0), flat),
    ]
    across = extrude(Plane.XZ * Polygon(*xz, align=None), P.TAB_LEN, both=True)
    across = Pos(0, (y_end + y_far) / 2, 0) * across
    # profile along the side wall (YZ): the end face gets the same flat + lead-in
    yz = [
        (yp(0), UNDER + EPS),
        (yp(P.TAB_LEN), UNDER + EPS),
        (yp(P.TAB_LEN), TIP),
        (yp(P.TAB_LEAD), TIP),
        (yp(0), flat),
    ]
    lengthwise = extrude(Plane.YZ * Polygon(*yz, align=None), 10, both=True)
    lengthwise = Pos(x_face, 0, 0) * lengthwise
    tab = across & lengthwise

    # root gusset: a 45° wedge along the inner root edge (it prints as steps; a fillet would mostly vanish). It
    # starts 0.5 inside the tab: the tab's inner face tapers away from the root, and the gusset must stay joined to it
    g = P.TAB_GUSSET
    xr, xin = xp(P.TAB_ROOT_T), xp(P.TAB_ROOT_T - 0.5)
    gusset = [
        (xin, UNDER + EPS),
        (xp(P.TAB_ROOT_T + g), UNDER + EPS),
        (xr, UNDER - g),
        (xin, UNDER - g),
    ]
    wedge = extrude(Plane.XZ * Polygon(*gusset, align=None), P.TAB_LEN / 2, both=True)
    return tab + Pos(0, (y_end + y_far) / 2, 0) * wedge


def _bump(side: int, end: int) -> Part:
    """Detent bump on a tab's wall face, centred on the tab, reaching BUMP_H toward the wall.

    Its lower (lead) face slopes at BUMP_LEAD_ANGLE from the tab face, its upper (hold) face at BUMP_HOLD_ANGLE.
    """
    x_face = IX0 + P.TAB_CLEAR if side < 0 else IX1 - P.TAB_CLEAR
    inward = -side
    zc = board_z(P.BUMP_Z)
    sink = 0.1  # the bump's base sits this far inside the tab, continuing both faces' slopes
    lead = (P.BUMP_H + sink) / math.tan(math.radians(P.BUMP_LEAD_ANGLE))
    hold = (P.BUMP_H + sink) / math.tan(math.radians(P.BUMP_HOLD_ANGLE))
    tri = [
        (x_face + inward * sink, zc - lead),
        (x_face - inward * P.BUMP_H, zc),
        (x_face + inward * sink, zc + hold),
    ]
    bump = extrude(Plane.XZ * Polygon(*tri, align=None), P.BUMP_LEN / 2, both=True)
    yc = tab_y_centres()[0 if end < 0 else 1]
    return Pos(0, yc, 0) * bump


def _tabs() -> Part:
    # clip to the lid body's outline: its rounded corners are concentric with the wall's inner corners, TAB_CLEAR in
    bx0, bx1, by0, by1, br = _body_outline()
    clip = rounded_block(bx0, bx1, by0, by1, TIP - 1, UNDER + EPS, br)
    clip += _header_corners(TIP - 1, UNDER + EPS)
    corners = [(side, end) for side in (-1, 1) for end in (-1, 1)]
    tabs = (Part() + [_tab(s, e) for s, e in corners]) & clip
    return tabs + [_bump(s, e) for s, e in corners]


def _lintel() -> Part:
    """Fills the base's cable-wall opening above the connectors, flush with the wall's outer face.

    It stops OPENING_TOP_MARGIN above the USB stack and above the RJ45 (stepping at LINTEL_SPLIT_X), with LID_CLEAR to
    the opening's sides.
    """
    cx0, cx1 = P.CABLE_OPENING_X
    c = P.LID_CLEAR
    y0, y1 = P.OUTER[2], IY0 + P.LID_CLEAR + EPS
    usb_bottom = P.USB_BOX.z1 + P.OPENING_TOP_MARGIN
    rj_bottom = P.RJ45_BOX.z1 + P.OPENING_TOP_MARGIN
    return block(
        cx0 + c, P.LINTEL_SPLIT_X, y0, y1, min(usb_bottom, UNDER), TOP
    ) + block(P.LINTEL_SPLIT_X - EPS, cx1 - c, y0, y1, rj_bottom, TOP)


def _header_fill() -> Part:
    """Fills the header-end slot above the lid's underside, flush with the wall's outer face, LID_CLEAR from its ends."""
    c = P.LID_CLEAR
    return block(IX0 + c, IX1 - c, IY1 - c - EPS, P.OUTER[3], UNDER, TOP)


def curtain_segments() -> list[tuple[float, float]]:
    """(x0, x1) of each break-off curtain segment."""
    x0, x1 = IX0 + P.LID_CLEAR, IX1 - P.LID_CLEAR
    n, slit = P.CURTAIN_SEGMENTS, P.CURTAIN_SLIT
    w = (x1 - x0 - (n - 1) * slit) / n
    return [(x0 + i * (w + slit), x0 + i * (w + slit) + w) for i in range(n)]


def _curtain() -> Part:
    """Break-off segments hanging in the slot below the lid's underside.

    Each is CURTAIN_T thick, centred in the wall. At the underside (a printed layer boundary) 45° V-notches on both
    faces narrow it to CURTAIN_NECK, so it snaps off there. A tongue on its bottom sits in the floor groove;
    the segment stops CURTAIN_FLOOR_GAP above the floor.
    """
    mid = (IY1 + P.OUTER[3]) / 2
    t, n, tw = P.CURTAIN_T / 2, P.CURTAIN_NECK / 2, P.CURTAIN_TONGUE_W / 2
    floor = board_z(P.HEADER_SLOT_BOTTOM)
    bottom = floor + P.CURTAIN_FLOOR_GAP
    tip = floor - P.SLOT_GROOVE_D + P.CURTAIN_FLOOR_GAP
    widen = UNDER - (t - n)  # the notch's 45° flank reaches the full thickness here
    outline = [
        (mid - n, UNDER + EPS),
        (mid + n, UNDER + EPS),
        (mid + t, widen),
        (mid + t, bottom),
        (mid + tw, bottom),
        (mid + tw, tip),
        (mid - tw, tip),
        (mid - tw, bottom),
        (mid - t, bottom),
        (mid - t, widen),
    ]
    return Part() + [x_prism(outline, x0, x1) for x0, x1 in curtain_segments()]


def _outside_ears() -> tuple[Part, Part]:
    """(material, holes) of the outside ears: the fill over each wall-top cut and a U ear in the wall's pocket."""
    parts, holes = [], []
    y, z = P.EAR_Y, board_z(P.EAR_SCREW_Z)
    half = P.EAR_W / 2
    for face, inner, inward in side_walls():
        body_edge = inner + inward * (P.LID_CLEAR + EPS)
        parts.append(block(*sorted((face, body_edge)), y - half, y + half, UNDER, TOP))
        ear = u_outline(y, z, half, UNDER + EPS)
        parts.append(x_prism(ear, face, face + inward * P.EAR_T))
        holes.append(
            x_cylinder(
                y, z, P.EAR_CLEAR_D, face - inward, face + inward * (P.EAR_T + 1)
            )
        )
        holes.append(x_countersink(y, z, P.EAR_CSK_D, P.EAR_CLEAR_D, face, inward))
    return Part() + parts, Part() + holes


def _inside_ears() -> tuple[Part, Part]:
    """(material, holes) of the inside ears: a block 0.2 off each wall with a 45° buttress, 45° end gussets and a blind
    pilot on the wall hole's axis."""
    parts, holes = [], []
    y, z = P.EAR_Y, board_z(P.IN_EAR_SCREW_Z)
    half, g = P.IN_EAR_LEN / 2, P.IN_EAR_GUSSET
    bottom = board_z(P.IN_EAR_BOTTOM)
    for _, inner, inward in side_walls():
        ex = inner + inward * P.TAB_CLEAR
        back = ex + inward * P.IN_EAR_T
        buttress = back + inward * (UNDER - bottom)
        section = [
            (ex, UNDER + EPS),
            (buttress, UNDER + EPS),
            (back, bottom),
            (ex, bottom),
        ]
        parts.append(
            Pos(0, y, 0)
            * extrude(Plane.XZ * Polygon(*section, align=None), half, both=True)
        )
        for y_end, out in ((y - half, -1), (y + half, 1)):
            tri = [
                (y_end, UNDER + EPS),
                (y_end + out * g, UNDER + EPS),
                (y_end, UNDER - g),
            ]
            parts.append(x_prism(tri, ex, back))
        stop = back - inward * P.IN_EAR_PILOT_STOP
        holes.append(x_cylinder(y, z, P.IN_EAR_PILOT_D, ex - inward, stop))
    return Part() + parts, Part() + holes


def build_lid(curtain: bool = True, ears: str = P.EAR_STYLE) -> Part:
    """The lid as one solid, in the board frame as fitted (top face flush at case Z 28.6), for one screw-ear option.

    With curtain=False it is the lid with all four curtain segments broken out.
    """
    if ears not in P.EAR_OPTIONS:
        raise ValueError(f"ears must be one of {P.EAR_OPTIONS}, not {ears!r}")
    part = _plate() + _lintel() + _tabs() + _header_fill()
    if curtain:
        part += _curtain()
    if ears == "none":  # the corner-tab snaps alone hold the lid
        return part - _grille(ears) - top_bevel()
    ear_parts, ear_holes = _outside_ears() if ears == "outside" else _inside_ears()
    return part + ear_parts - _grille(ears) - ear_holes - top_bevel()


def opening_widths(lid: Part) -> list[float]:
    """Width of every through-opening in the lid plate: twice the largest circle that fits, per opening."""
    section = lid & (Pos(27.5, 34.0, (UNDER + FLOOR) / 2) * Rectangle(200, 200))
    widths = []
    for face in section.faces():
        for wire in face.inner_wires():
            # openings are convex, so the largest circle sits at the centroid
            centre = Face(wire).center()
            widths.append(2 * wire.distance_to(centre))
    return widths
