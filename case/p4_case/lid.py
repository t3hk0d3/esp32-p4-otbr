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
    """Honeycomb cell centres inside the frame, skipping the cross rib, plus the cells over the LED."""
    fx0, fx1, fy0, fy1, _ = _flange_outline()
    gx0, gx1 = fx0 + P.FRAME_W, fx1 - P.FRAME_W
    gy0, gy1 = fy0 + P.FRAME_W, fy1 - P.FRAME_W
    pitch_y = P.CELL + P.CELL_WALL
    pitch_x = 1.5 * pitch_y / math.sqrt(3)
    half_x, half_y = CELL_R, P.CELL / 2
    rib_keep = P.RIB_W / 2 + half_y
    out = []
    col = 0
    x = gx0 + half_x
    while x + half_x <= gx1:
        y = gy0 + half_y + (pitch_y / 2 if col % 2 else 0.0)
        while y + half_y <= gy1:
            if abs(y - P.RIB_Y) >= rib_keep:
                out.append((x, y))
            y += pitch_y
        x += pitch_x
        col += 1
    lx, ly = P.LED_XY
    led_x = IX0 + P.LID_CLEAR + half_x + 0.15  # first cell inside the lid body edge
    out += [(max(lx, led_x), ly + k * pitch_y) for k in (-1, 0, 1)]
    return out


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


def build_lid() -> Part:
    """The lid as one solid, in the board frame as fitted (top face flush at case Z 27.6)."""
    return _plate() + _tabs() - _grille()


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
