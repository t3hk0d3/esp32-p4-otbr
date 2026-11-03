"""The lid: stepped grille plate and four wedged corner tabs (spec section 5.3). Board frame, as fitted on the base."""

import math

from build123d import (
    Align,
    Cylinder,
    Face,
    Part,
    Plane,
    Polygon,
    Pos,
    Rectangle,
    RegularPolygon,
    Rot,
    extrude,
)

from p4_case import params as P
from p4_case.base import EPS, block, rounded_block, tab_y_centres
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
    body = rounded_block(bx0, bx1, by0, by1, UNDER, FLOOR + EPS, br)
    return flange + body


def cell_centres() -> list[tuple[float, float]]:
    """Honeycomb cell centres, centred on the lid and symmetric about the cross rib, all inside the solid frame."""
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
    return sorted(set(out))


def _grille() -> Part:
    cells = [Pos(x, y) * RegularPolygon(CELL_R, 6) for x, y in cell_centres()]
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

    # root fillet: a concave quarter-round along the inner root edge
    r = P.TAB_FILLET
    xr = xp(P.TAB_ROOT_T)
    y0, y1 = sorted((y_end, y_far))
    wedge = block(
        min(xr, xp(P.TAB_ROOT_T + r)),
        max(xr, xp(P.TAB_ROOT_T + r)),
        y0,
        y1,
        UNDER - r,
        UNDER + EPS,
    )
    round_cut = (
        Pos(xp(P.TAB_ROOT_T + r), y0 - 1, UNDER - r)
        * Rot(-90, 0, 0)
        * Cylinder(r, y1 - y0 + 2, align=(Align.CENTER, Align.CENTER, Align.MIN))
    )
    tab += wedge - round_cut

    return tab


def _bump(side: int, end: int) -> Part:
    """Detent bump on a tab's wall face, centred on the tab, reaching BUMP_H toward the wall."""
    x_face = IX0 + P.TAB_CLEAR if side < 0 else IX1 - P.TAB_CLEAR
    inward = -side
    zc = board_z(P.BUMP_Z)
    flank = P.BUMP_H / math.tan(math.radians(P.BUMP_ANGLE))
    tri = [
        (x_face + inward * 0.1, zc - flank - 0.06),
        (x_face - inward * P.BUMP_H, zc),
        (x_face + inward * 0.1, zc + flank + 0.06),
    ]
    bump = extrude(Plane.XZ * Polygon(*tri, align=None), P.BUMP_LEN / 2, both=True)
    yc = tab_y_centres()[0 if end < 0 else 1]
    return Pos(0, yc, 0) * bump


def _tabs() -> Part:
    # clip to the lid body's outline: its rounded corners are concentric with the wall's inner corners, TAB_CLEAR in
    bx0, bx1, by0, by1, br = _body_outline()
    clip = rounded_block(bx0, bx1, by0, by1, TIP - 1, UNDER + EPS, br)
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


def build_lid() -> Part:
    """The lid as one solid, in the board frame as fitted (top face flush at case Z 27.6)."""
    return _plate() + _lintel() + _tabs() - _grille()


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
