"""Fit and printability checks (spec section 8). Every check returns CheckResult and never raises for a failure.

The board is represented by its column envelope (top and bottom per 0.25 mm cell), which is conservative: anything
between a cell's bottom and top counts as board.
"""

import math
from collections.abc import Iterable

import numpy as np
import shapely
import trimesh
from build123d import Align, Box, Cylinder, Part, Pos, Rot, Vector

from p4_case import params as P
from p4_case.base import rounded_block, side_walls, tab_y_centres
from p4_case.board import ENV_RES, FACTS, BoardModel, check_facts
from p4_case.lid import opening_widths
from p4_case.params import board_z
from p4_case.results import CheckResult

CLEARANCE = 0.3  # 8: minimum gap between a printed part and the board, outside intended contacts
SAMPLES_PER_MM2 = 12
TESS_TOL = 0.02
BOOL_EPS = 0.01  # mm³: intersections smaller than this are numerical noise
OVERHANG_NZ = (
    -math.cos(math.radians(45)) - 0.01
)  # facets pointing down more steeply than 45° from vertical
HORIZONTAL_NZ = -0.99
BRIDGE_MAX = 18.0  # 8: longest accepted bridge
# small round tops (pin-holes, SMA) sag less than this; accepted (print feedback: the Ø6.5 SMA top prints fine)
DROOP_MAX = 1.0
DROOP_SPAN_MAX = 8.0  # ... as long as they are small
BRIDGE_WIDTH_MAX = 3.0  # bridges here run across a 2 mm wall
FIT_BACK = 0.2  # connector fronts this far inside the cable wall's inner face while dropping in
FIT_LIFT = 0.5  # the board slides this high above the standoffs
FIT_STEP_MM = 0.25
SMA_ENVELOPE_R = 5.0  # 5.2: free radius around the SMA axis inside the wall
SMA_ENVELOPE_LEN = 12.0
HEADER_RIBBON_X = (2.1, 52.9)  # a 40-wire ribbon centred on the header
# header-slot probes run between the header-end tabs' root gussets (the ribbon's edges may brush them, spec 5.3)
SLOT_PROBE_X = (
    HEADER_RIBBON_X[0] + P.TAB_GUSSET + 0.2,
    HEADER_RIBBON_X[1] - P.TAB_GUSSET - 0.2,
)


def to_mesh(part: Part) -> trimesh.Trimesh:
    verts, tris = part.tessellate(TESS_TOL, 0.2)
    return trimesh.Trimesh(
        np.array([(v.X, v.Y, v.Z) for v in verts]), np.array(tris), process=True
    )


def _volume(shape) -> float:
    """Volume of a boolean result; an empty result is 0. A failed boolean counts as a collision (inf), never as clear."""
    if shape is None:
        return 0.0
    try:
        return float(shape.volume)
    except (AttributeError, ValueError):
        return math.inf


def _intersection(a: Part, b: Part) -> float:
    return _volume(a & b)


def _box(x0, x1, y0, y1, z0, z1) -> Part:
    return Pos(x0, y0, z0) * Box(
        x1 - x0, y1 - y0, z1 - z0, align=(Align.MIN, Align.MIN, Align.MIN)
    )


# --- board clearance -----------------------------------------------------------------------------------------------


def _on_standoff(x: np.ndarray, y: np.ndarray, grow: float) -> np.ndarray:
    """Points within grow of a standoff's footprint: the post, or its web out to the side wall."""
    ok = np.zeros_like(x, dtype=bool)
    r = P.STANDOFF_D / 2 + grow
    mid = (P.INNER[0] + P.INNER[1]) / 2
    for hx, hy in P.SCREW_HOLES:
        ok |= (x - hx) ** 2 + (y - hy) ** 2 <= r * r
        wall = P.INNER[0] if hx < mid else P.INNER[1]
        x0, x1 = sorted((hx, wall))
        ok |= (x >= x0 - grow) & (x <= x1 + grow) & (np.abs(y - hy) <= r)
    return ok


def _allowed_contact(x: np.ndarray, y: np.ndarray) -> np.ndarray:
    """The round standoff tops and their bores touch the board by design (the webs sit lower and must clear it)."""
    ok = np.zeros_like(x, dtype=bool)
    r = P.STANDOFF_D / 2 + CLEARANCE
    for hx, hy in P.SCREW_HOLES:
        ok |= (x - hx) ** 2 + (y - hy) ** 2 <= r * r
    return ok


def board_clearance(board: BoardModel, part: Part, name: str) -> CheckResult:
    """The part's surface stays CLEARANCE away from the board's columns, except at the standoffs."""
    mesh = to_mesh(part)
    n = int(mesh.area * SAMPLES_PER_MM2)
    pts, _ = trimesh.sample.sample_surface(mesh, n, seed=2)
    env = board.envelope
    from p4_case.board import ENV_ORIGIN, ENV_RES

    h, w = env.top_map.shape
    ix = np.floor((pts[:, 0] - ENV_ORIGIN[0]) / ENV_RES).astype(int)
    iy = np.floor((pts[:, 1] - ENV_ORIGIN[1]) / ENV_RES).astype(int)
    inside = (ix >= 0) & (ix < w) & (iy >= 0) & (iy < h)
    top = np.full(len(pts), np.nan)
    bot = np.full(len(pts), np.nan)
    top[inside] = env.top_map[iy[inside], ix[inside]]
    bot[inside] = env.bottom_map[iy[inside], ix[inside]]
    z = pts[:, 2]
    hit = np.isfinite(top) & (z > bot - CLEARANCE) & (z < top + CLEARANCE)
    hit &= ~_allowed_contact(pts[:, 0], pts[:, 1])
    if not hit.any():
        return CheckResult(
            f"{name} clears the board",
            True,
            f"≥ {CLEARANCE} mm everywhere ({n} samples)",
        )
    depth = np.where(
        hit, np.minimum(z - (bot - CLEARANCE), (top + CLEARANCE) - z), -np.inf
    )
    worst = int(np.argmax(depth))
    x, y, zz = pts[worst]
    return CheckResult(
        f"{name} clears the board",
        False,
        f"{int(hit.sum())} samples too close; e.g. at ({x:.2f}, {y:.2f}, {zz:.2f}), board column "
        f"{bot[worst]:.2f}..{top[worst]:.2f}",
    )


# --- openings --------------------------------------------------------------------------------------------------------


def _x_rod(y: float, z: float, d: float, x0: float, x1: float) -> Part:
    return (
        Pos(x0, y, z)
        * Rot(0, 90, 0)
        * Cylinder(d / 2, x1 - x0, align=(Align.CENTER, Align.CENTER, Align.MIN))
    )


def opening_paths_spec() -> dict[str, Part]:
    """A straight probe from outside to each part the case gives access to (slightly smaller than the opening)."""
    usb, rj, sd = FACTS["usb"], FACTS["rj45"], FACTS["sd"]
    out_y = P.OUTER[2] - 2
    paths = {
        "USB-C": _box(usb.x0 + 2, usb.x1 - 2, out_y, usb.y0, 1.5, 4.0),
        "USB-A": _box(usb.x0 + 1, usb.x1 - 1, out_y, usb.y0, 10.0, 15.0),
        "RJ45": _box(rj.x0 + 1, rj.x1 - 1, out_y, rj.y0, 1.0, 12.5),
        "microSD": _box(sd.x0 + 0.5, sd.x1 - 0.5, out_y, sd.y0, 0.1, 1.2),
        "header slot": _box(
            SLOT_PROBE_X[0],
            SLOT_PROBE_X[1],
            FACTS["header"].y1,
            P.OUTER[3] + 2,
            board_z(P.HEADER_SLOT_BOTTOM) + 0.05,
            board_z(P.LID_UNDERSIDE) - 0.05,
        ),
        # up to the knock-out, which is cut out by hand when an SMA is fitted
        "SMA": _x_rod(
            P.SMA_Y,
            board_z(P.SMA_CASE_Z),
            5.0,
            P.INNER[1] - 1,
            P.OUTER[1] - P.SMA_KNOCKOUT_T - 0.05,
        ),
    }
    for key in ("button_a", "button_b"):
        b = FACTS[key]
        paths[f"button {key[-1].upper()}"] = _x_rod(
            (b.y0 + b.y1) / 2, (b.z0 + b.z1) / 2, 2.0, P.OUTER[0] - 2, b.x0
        )
    return paths


def opening_paths(base: Part, lid: Part, lid_open: Part) -> CheckResult:
    """Each probe passes through the assembled case without touching it.

    The header-slot probe runs against the lid with its curtain broken out (lid_open); with the curtain on (lid) the
    slot must be closed.
    """
    case, case_open = base + lid, base + lid_open
    paths = opening_paths_spec()
    header = paths.pop("header slot")
    blocked = [
        name for name, probe in paths.items() if _intersection(probe, case) > BOOL_EPS
    ]
    if _intersection(header, case_open) > BOOL_EPS:
        blocked.append("header slot (curtain broken out)")
    closed = _intersection(header, case) > BOOL_EPS
    if not closed:
        blocked.append("header slot not closed by the curtain")
    return CheckResult(
        "every opening has a clear path",
        not blocked,
        "blocked: " + ", ".join(blocked)
        if blocked
        else "USB-C, USB-A, RJ45, microSD, buttons, SMA; header slot once the curtain is broken out "
        "(closed by the curtain until then)",
    )


# --- fitting motion ------------------------------------------------------------------------------------------------


BOARD_CORNER_R = 2.0  # the PCB's corner radius (spec section 3)


def board_bodies(rise: float = 0.0) -> list[Part]:
    """The board for the motion check: the rounded PCB and the boxes of the parts above it, raised by rise on top.

    Part boxes are clipped to Z ≥ 0: below the PCB they hold only pins, which never reach the walls.
    """
    pcb = FACTS["pcb"]
    bodies = [
        rounded_block(
            pcb.x0, pcb.x1, pcb.y0, pcb.y1, pcb.z0, pcb.z1 + rise, BOARD_CORNER_R
        )
    ]
    for k in ("usb", "rj45", "sd", "button_a", "button_b", "header", "transformer"):
        b = FACTS[k]
        bodies.append(_box(b.x0, b.x1, b.y0, b.y1, max(b.z0, 0.0), b.z1 + rise))
    return bodies


def _posed(bodies: list[Part], shift_y: float, lift: float) -> Part:
    """Board bodies shifted toward the header end by shift_y and raised by lift."""
    return Part() + [Pos(0, shift_y, lift) * b for b in bodies]


def _underside_over_standoffs(board: BoardModel, shift_max: float) -> float:
    """Deepest board underside part (below the PCB) that crosses a standoff top (post or web) while the board slides
    shift_max → 0."""
    env = board.envelope
    pcb_bottom = FACTS["pcb"].z0
    xs = np.arange(P.INNER[0], P.INNER[1] + ENV_RES / 2, ENV_RES)
    ys = np.arange(P.INNER[2], P.INNER[3] + ENV_RES / 2, ENV_RES)
    gx, gy = np.meshgrid(xs, ys)
    keep = _on_standoff(gx, gy, 0.0)
    pts = list(zip(gx[keep], gy[keep], strict=True))
    deepest = 0.0
    for s in np.arange(0.0, shift_max + 1e-6, ENV_RES):
        for x, y in pts:
            b = env.bottom(x, y - s)
            if not math.isnan(b):
                deepest = max(deepest, pcb_bottom - b)
    return deepest


def fitting_motion(base: Part, board: BoardModel | None = None) -> CheckResult:
    """The board drops straight in, shifted toward the header end, then slides its connectors into the cable wall.

    Drop: shifted back FIT_BACK beyond the connectors' fronts and raised FIT_LIFT above the standoffs, the board's swept
    volume from above the wall tops down to that pose is clear of the base. Slide: at that height it moves forward in
    FIT_STEP_MM steps until home, clear of the base; underside parts crossing the standoffs hang less than FIT_LIFT.
    Settle: at home it lowers onto the standoffs.
    """
    bodies = board_bodies()
    front = min(FACTS[k].y0 for k in ("usb", "rj45"))
    back = P.INNER[2] - front + FIT_BACK
    rise = board_z(P.TOP) + 5 - FACTS["pcb"].z0
    if _intersection(_posed(board_bodies(rise), back, FIT_LIFT), base) > BOOL_EPS:
        return CheckResult(
            "board fits in",
            False,
            f"the straight drop {back:.2f} mm back hits the base",
        )
    n = math.ceil(back / FIT_STEP_MM)
    for k in range(n + 1):
        shift = back * (1 - k / n)
        if _intersection(_posed(bodies, shift, FIT_LIFT), base) > BOOL_EPS:
            return CheckResult(
                "board fits in", False, f"the slide collides {shift:.2f} mm before home"
            )
    if _intersection(_posed(bodies, 0.0, 0.0), base) > BOOL_EPS:
        return CheckResult(
            "board fits in", False, "the board collides when it settles at home"
        )
    if board is not None:
        deepest = _underside_over_standoffs(board, back)
        if deepest >= FIT_LIFT:
            return CheckResult(
                "board fits in",
                False,
                f"an underside part {deepest:.2f} mm deep crosses a standoff during the slide",
            )
    return CheckResult(
        "board fits in",
        True,
        f"drop in {back:.2f} mm back, lift {FIT_LIFT} mm, slide forward and settle: clear",
    )


# --- lid, ribbon plug, SMA ---------------------------------------------------------------------------------------------


def ribbon_plug_block(grow: float = 0.0) -> Part:
    length, depth, height = P.RIBBON_PLUG
    hx = (FACTS["header"].x0 + FACTS["header"].x1) / 2
    hy = (FACTS["header"].y0 + FACTS["header"].y1) / 2
    z0 = P.HEADER_BODY_TOP
    return _box(
        hx - length / 2 - grow,
        hx + length / 2 + grow,
        hy - depth / 2 - grow,
        hy + depth / 2 + grow,
        z0 - grow,
        z0 + height + grow,
    )


def sma_envelope() -> Part:
    return _x_rod(
        P.SMA_Y,
        board_z(P.SMA_CASE_Z),
        2 * SMA_ENVELOPE_R,
        P.INNER[1] - SMA_ENVELOPE_LEN,
        P.INNER[1],
    )


def sma_room(board: BoardModel, radius: float = SMA_ENVELOPE_R) -> CheckResult:
    """The SMA bulkhead's nut and washer envelope inside the wall is free of the board's columns."""
    env = board.envelope
    zc = board_z(P.SMA_CASE_Z)
    hits = []
    for x in np.arange(P.INNER[1] - SMA_ENVELOPE_LEN, P.INNER[1], ENV_RES):
        for y in np.arange(P.SMA_Y - radius, P.SMA_Y + radius, ENV_RES):
            top, bottom = env.top(x, y), env.bottom(x, y)
            if math.isnan(top):
                continue
            dz = math.sqrt(max(0.0, radius * radius - (y - P.SMA_Y) ** 2))
            if bottom < zc + dz and top > zc - dz:
                hits.append((x, y, bottom, top))
    if not hits:
        return CheckResult(
            "SMA nut clears the board",
            True,
            f"Ø{2 * radius:.0f} × {SMA_ENVELOPE_LEN:.0f} mm free inside the wall",
        )
    x, y, bottom, top = hits[0]
    return CheckResult(
        "SMA nut clears the board",
        False,
        f"{len(hits)} cells inside the envelope; e.g. ({x:.2f}, {y:.2f}), board column {bottom:.2f}..{top:.2f}",
    )


def lid_clearance(
    board: BoardModel, lid: Part, lid_open: Part | None = None
) -> list[CheckResult]:
    """The lid clears the board, the SMA body, a ribbon plug and the ribbon between the tab roots.

    The ribbon is checked against lid_open (the curtain broken out, as it is when a ribbon is fitted) when given.
    """
    results = [board_clearance(board, lid, "lid")]
    others = {
        "SMA body": sma_envelope(),
        "ribbon plug": ribbon_plug_block(),
        "ribbon": _box(
            HEADER_RIBBON_X[0]
            # the ribbon may brush the header-end tabs (5.3); not counted
            + P.TAB_GUSSET,
            HEADER_RIBBON_X[1] - P.TAB_GUSSET,
            FACTS["header"].y0,
            P.INNER[3],
            P.HEADER_BODY_TOP + P.RIBBON_PLUG[2],
            board_z(P.LID_UNDERSIDE) - 0.05,
        ),
    }
    against = {"ribbon": lid_open if lid_open is not None else lid}
    hit = [
        k
        for k, body in others.items()
        if _intersection(body, against.get(k, lid)) > BOOL_EPS
    ]
    results.append(
        CheckResult(
            "lid clears SMA, ribbon plug and ribbon",
            not hit,
            "touches: " + ", ".join(hit) if hit else "clear",
        )
    )
    return results


RIBBON_HEADROOM = (
    1.3  # mm between a ribbon plug's top and the lid's underside, for the ribbon's bend
)


def ribbon_plug_headroom() -> CheckResult:
    """A 2×20 IDC plug on the header leaves room under the lid for its ribbon to bend toward the header slot.

    Its 55.9 mm length fits the 56 mm cavity snugly; the walls are not recessed for it (print feedback).
    """
    headroom = board_z(P.LID_UNDERSIDE) - (P.HEADER_BODY_TOP + P.RIBBON_PLUG[2])
    return CheckResult(
        "ribbon plug has headroom",
        headroom >= RIBBON_HEADROOM,
        f"{headroom:.1f} mm under the lid",
    )


# --- keyholes and nuts -----------------------------------------------------------------------------------------------


def keyhole_volumes(board: BoardModel) -> CheckResult:
    """Each keyhole's head sweep (entry and both arms, head radius, KEY_HEAD_H tall) is free of the board."""
    plate_top = board_z(P.PLATE_T)
    r = P.KEY_HEAD_D / 2
    worst = math.inf
    for kx, ky in P.KEYHOLES:
        for region in (
            (kx - r, kx + r, ky - r, ky + P.KEY_ARM_L + r),
            (kx - r, kx + P.KEY_ARM_L + r, ky - r, ky + r),
        ):
            worst = min(
                worst, board.envelope.free_height_above_plate(region, plate_top)
            )
    return CheckResult(
        "keyhole screw heads fit",
        worst >= P.KEY_HEAD_H,
        f"least free height {worst:.2f} mm (need {P.KEY_HEAD_H})",
    )


NUT_D = 6.5
NUT_H = 2.5


def nut_circles(board: BoardModel) -> CheckResult:
    """A Ø6.5 × 2.5 mm nut circle on top of the board around each mounting hole is free of parts."""
    from p4_case.board import ENV_ORIGIN, ENV_RES

    env = board.envelope
    bad = []
    for hx, hy in P.SCREW_HOLES:
        r = NUT_D / 2
        xs = np.arange(hx - r, hx + r, ENV_RES / 2)
        ys = np.arange(hy - r, hy + r, ENV_RES / 2)
        gx, gy = np.meshgrid(xs, ys)
        keep = (gx - hx) ** 2 + (gy - hy) ** 2 <= r * r
        ix = np.floor((gx[keep] - ENV_ORIGIN[0]) / ENV_RES).astype(int)
        iy = np.floor((gy[keep] - ENV_ORIGIN[1]) / ENV_RES).astype(int)
        tops = env.top_map[iy, ix]
        high = float(np.nanmax(tops))
        if high > 0.05:
            bad.append(f"({hx}, {hy}) has a part {high:.2f} mm tall")
    return CheckResult(
        "nut circles are free",
        not bad,
        "; ".join(bad) if bad else f"Ø{NUT_D} free at all four holes",
    )


# --- opening size, watertight, printability ------------------------------------------------------------------------


def opening_size(base: Part, lid: Part, lid_open: Part) -> CheckResult:
    """No way into the closed case is wider than CELL: lid cells measured, the header slot closed by the curtain.

    The passage left when the curtain is broken out (lid_open) is measured and reported, not limited.
    """
    widths = opening_widths(lid)
    widest = max(widths) if widths else 0.0
    # the curtain spans the slot inside the wall's thickness
    wall = _box(
        *SLOT_PROBE_X,
        P.INNER[3] + 0.1,
        P.OUTER[3] - 0.1,
        board_z(P.HEADER_SLOT_BOTTOM) + 0.3,
        board_z(P.LID_UNDERSIDE) - 0.3,
    )
    closed = _intersection(wall, lid) > BOOL_EPS
    case_open = base + lid_open

    # slabs across the wall's inner face, between the tabs
    def slab(z0: float, z1: float) -> Part:
        return _box(*SLOT_PROBE_X, P.INNER[3] - 0.3, P.INNER[3] + 0.3, z0, z1)

    # the broken-out passage: walk down from the lid underside while a thin slab stays clear
    step = 0.05
    top = board_z(P.LID_UNDERSIDE)
    floor = top - step
    while (
        floor > board_z(P.STANDOFF_TOP)
        and _intersection(slab(floor - step, floor), case_open) <= BOOL_EPS
    ):
        floor -= step
    ok = widest <= P.CELL + 1e-6 and closed
    return CheckResult(
        "no opening wider than 3.5 mm",
        ok,
        f"widest lid opening {widest:.2f} mm; header slot "
        f"{'closed by the curtain' if closed else 'open'}, {top - floor:.1f} mm when broken out",
    )


def _bump_zones() -> list[Part]:
    """The space past each side wall's inner face around a bump: where the bump presses into its dent by design."""
    zc = board_z(P.BUMP_Z)
    half = P.BUMP_LEN / 2 + 0.1
    zones = []
    for yc in tab_y_centres():
        for x0, x1 in ((P.INNER[0] - 1, P.INNER[0]), (P.INNER[1], P.INNER[1] + 1)):
            zones.append(_box(x0, x1, yc - half, yc + half, zc - 1.5, zc + 1.5))
    return zones


def lid_seats(base: Part, lid: Part) -> CheckResult:
    """The fitted lid does not overlap the base anywhere (tabs, flange in the rebate), apart from the bumps' preload."""
    overlap = _intersection(base, lid - (Part() + _bump_zones()))
    return CheckResult(
        "lid seats on the base",
        overlap <= BOOL_EPS,
        f"overlap {overlap:.3f} mm³ (bump preload excluded)",
    )


DETENT_REACH_MIN = 0.15  # a bump must reach this far past the wall face
# ... and press into its dent's hold face by no more than this
DETENT_PRELOAD_MAX = 0.15


def _boundary(solid_at, inside: float, outside: float) -> float:
    """Bisect for the surface between a solid point (inside) and an empty one (outside), to 0.005 mm."""
    while abs(outside - inside) > 0.005:
        mid = (inside + outside) / 2
        if solid_at(mid):
            inside = mid
        else:
            outside = mid
    return (inside + outside) / 2


def detent_engagement(base: Part, lid: Part) -> CheckResult:
    """At each bump's crest height the bump reaches past the wall face and presses lightly into its dent.

    reach: how far the lid gets past the wall's inner face; dent: how deep the wall is cut there; the difference is
    the preload that keeps the lid down without rattle.
    """
    z = board_z(P.BUMP_Z)
    reaches, preloads = [], []
    for yc in tab_y_centres():
        for face, out in ((P.INNER[0], -1), (P.INNER[1], 1)):

            def lid_at(u, face=face, out=out, yc=yc):
                return lid.is_inside(Vector(face + out * u, yc, z))

            def gap_at(u, face=face, out=out, yc=yc):
                return not base.is_inside(Vector(face + out * u, yc, z))

            reach = (
                _boundary(lid_at, -P.TAB_CLEAR - 0.3, 1.0)
                if lid_at(-P.TAB_CLEAR - 0.3)
                else -math.inf
            )
            dent = _boundary(gap_at, -0.05, 1.0)
            reaches.append(reach)
            preloads.append(reach - dent)
    reach, low, high = min(reaches), min(preloads), max(preloads)
    ok = reach >= DETENT_REACH_MIN and low > 0 and high <= DETENT_PRELOAD_MAX
    return CheckResult(
        "lid snaps engage the walls",
        ok,
        f"bumps reach {reach:.2f} mm past the wall (need {DETENT_REACH_MIN}); "
        f"preload {low:.2f}..{high:.2f} mm (need > 0, ≤ {DETENT_PRELOAD_MAX})",
    )


def watertight(part: Part, name: str) -> CheckResult:
    mesh = to_mesh(part)
    ok = part.is_valid and len(part.solids()) == 1 and mesh.is_watertight
    return CheckResult(
        f"{name} is one closed solid",
        ok,
        f"valid={part.is_valid}, solids={len(part.solids())}, watertight={mesh.is_watertight}",
    )


def print_pose(part: Part, top_down: bool) -> trimesh.Trimesh:
    """The part as it sits on the bed: lowest point at z 0 (flipped about X for top-down parts)."""
    mesh = to_mesh(part)
    if top_down:
        mesh.apply_transform(
            trimesh.transformations.rotation_matrix(math.pi, (1, 0, 0))
        )
    mesh.apply_translation((0, 0, -mesh.bounds[0][2]))
    return mesh


def _regions(mesh: trimesh.Trimesh, faces: np.ndarray) -> Iterable[np.ndarray]:
    if len(faces) == 0:
        return []
    sub = mesh.submesh([faces], append=True, repair=False)
    labels = trimesh.graph.connected_component_labels(
        sub.face_adjacency, node_count=len(sub.faces)
    )
    return [faces[labels == k] for k in np.unique(labels)]


def printability(part: Part, name: str, top_down: bool) -> CheckResult:
    """No overhang steeper than 45° from vertical, except bridges up to BRIDGE_MAX and tiny round tops."""
    mesh = print_pose(part, top_down)
    nz = mesh.face_normals[:, 2]
    zmin = mesh.triangles[:, :, 2].min(axis=1)
    down = np.nonzero((nz < OVERHANG_NZ) & (zmin > 0.05))[0]
    problems = []
    for region in _regions(mesh, down):
        tri = mesh.triangles[region]
        span = np.ptp(tri[:, :, :2].reshape(-1, 2), axis=0)
        height = float(np.ptp(tri[:, :, 2]))
        area = float(mesh.area_faces[region].sum())
        if (nz[region] < HORIZONTAL_NZ).all():
            # a bridge is a narrow strip spanning a gap: short enough and no wider on average than BRIDGE_WIDTH_MAX
            if max(span) <= BRIDGE_MAX and area / max(span) <= BRIDGE_WIDTH_MAX:
                continue
            kind = f"bridge or ledge {max(span):.1f} × {area / max(span):.1f} mm"
        elif height <= DROOP_MAX and max(span) <= DROOP_SPAN_MAX:
            continue
        else:
            kind = f"overhang {height:.1f} mm tall"
        c = tri.reshape(-1, 3).mean(axis=0)
        problems.append(f"{kind} at ({c[0]:.1f}, {c[1]:.1f}, z {c[2]:.1f})")
    return CheckResult(
        f"{name} prints without supports", not problems, "; ".join(problems[:5]) or "ok"
    )


MIN_PRINT_WIDTH = 0.8  # two 0.4 mm lines
PRINT_WIDTH_TOL = 0.05
STRAND_AREA_MIN = 0.2  # mm²: smaller thin bits are corner rounding
# a strand has the outside along both long sides; a bump on a wall along one
STRAND_EDGE_SHARE = 0.75


def min_print_width(part: Part, name: str, top_down: bool) -> CheckResult:
    """No layer holds a free-standing strand narrower than MIN_PRINT_WIDTH (a slicer prints it as one line or drops it).

    Each layer's section (at layer centres, LAYER_H apart, print pose) is opened by the width: the thin pieces it
    removes count as strands when the part's outline runs along most of their edge. Bumps on walls do not count.
    """
    mesh = print_pose(part, top_down)
    heights = np.arange(P.LAYER_H / 2, mesh.bounds[1][2], P.LAYER_H)
    sections = mesh.section_multiplane(
        plane_origin=(0, 0, 0), plane_normal=(0, 0, 1), heights=list(heights)
    )
    w = MIN_PRINT_WIDTH - PRINT_WIDTH_TOL
    problems = []
    for z, path in zip(heights, sections, strict=True):
        if path is None:
            continue
        for poly in path.polygons_full:
            opened = poly.buffer(-w / 2).buffer(w / 2)
            thin = poly.difference(opened)
            outline = poly.boundary.buffer(1e-3)
            for piece in getattr(thin, "geoms", [thin]):
                if piece.is_empty or piece.area < STRAND_AREA_MIN:
                    continue
                share = (
                    piece.boundary.intersection(outline).length / piece.boundary.length
                )
                if share > STRAND_EDGE_SHARE:
                    c = shapely.centroid(piece)
                    x, y, _ = trimesh.transform_points(
                        [(c.x, c.y, 0.0)], path.metadata["to_3D"]
                    )[0]
                    problems.append(
                        f"{piece.area / max(piece.length / 2, 1e-6):.2f} mm strand at ({x:.1f}, {y:.1f}, z {z:.1f})"
                    )
    return CheckResult(
        f"{name} has no strand under {MIN_PRINT_WIDTH} mm",
        not problems,
        "; ".join(problems[:5]) or "ok",
    )


EAR_RING = 8  # points round a hole's axis


def _ring_ok(part: Part, x: float, y: float, z: float, r: float) -> bool:
    """The axis point is empty and every point of a ring of radius r around it (in the YZ plane) is solid."""
    if part.is_inside(Vector(x, y, z)):
        return False
    return all(
        part.is_inside(
            Vector(
                x,
                y + r * math.cos(2 * math.pi * k / EAR_RING),
                z + r * math.sin(2 * math.pi * k / EAR_RING),
            )
        )
        for k in range(EAR_RING)
    )


def _hole_length(
    part: Part, x_at, y: float, z: float, r: float, u0: float, u1: float
) -> float:
    """Length along the axis (u from u0 to u1, x = x_at(u)) over which part holds a hole of radius < r round it."""
    us = np.arange(u0, u1 + 1e-9, 0.1)
    hits = [u for u in us if _ring_ok(part, x_at(u), y, z, r)]
    if not hits:
        return 0.0

    def edge(inside: float, outside: float) -> float:
        while abs(outside - inside) > 0.01:
            mid = (inside + outside) / 2
            if _ring_ok(part, x_at(mid), y, z, r):
                inside = mid
            else:
                outside = mid
        return (inside + outside) / 2

    return edge(hits[-1], min(hits[-1] + 0.1, u1)) - edge(
        hits[0], max(hits[0] - 0.1, u0)
    )


def ear_screws(base: Part, lid: Part, ears: str = P.EAR_STYLE) -> CheckResult:
    """Each screw ear's clearance hole and pilot share one axis, and the pilot is long enough.

    outside: the hole is in the lid's ear, the pilot (thread) in the base wall, at least 2.2 mm. inside: the hole is
    in the base wall, the pilot in the lid's block, at least 4.5 mm.
    """
    y = P.EAR_Y
    problems, lengths = [], []
    for (face, inner, inward), name in zip(
        side_walls(), ("left", "right"), strict=True
    ):
        if ears == "outside":
            z, need = board_z(P.EAR_SCREW_Z), 2.2
            hole_r, pilot_r = P.EAR_CLEAR_D / 2 + 0.25, P.EAR_PILOT_D / 2 + 0.2
            aligned = all(
                _ring_ok(lid, face + inward * u, y, z, hole_r) for u in (1.2, 1.4)
            )
            length = _hole_length(
                base, lambda u, f=face, i=inward: f + i * u, y, z, pilot_r, P.EAR_T, 4.0
            )
        else:
            z, need = board_z(P.IN_EAR_SCREW_Z), 4.5
            hole_r, pilot_r = P.IN_EAR_CLEAR_D / 2 + 0.25, P.IN_EAR_PILOT_D / 2 + 0.2
            aligned = all(
                _ring_ok(base, face + inward * u, y, z, hole_r) for u in (1.8, 2.8, 3.8)
            )
            length = _hole_length(
                lid,
                lambda u, f=inner, i=inward: f + i * u,
                y,
                z,
                pilot_r,
                P.TAB_CLEAR,
                P.TAB_CLEAR + P.IN_EAR_T,
            )
        lengths.append(f"{name} {length:.1f} mm")
        if not aligned:
            problems.append(f"{name}: hole off the screw axis")
        elif length < need:
            problems.append(f"{name}: thread {length:.1f} mm (need {need})")
    return CheckResult(
        "screw ears line up",
        not problems,
        "; ".join(problems)
        if problems
        else f"{ears} ears, thread " + ", ".join(lengths),
    )


# --- all ---------------------------------------------------------------------------------------------------------------


def run_all(
    board: BoardModel, base: Part, lid: Part, lid_open: Part, ears: str = P.EAR_STYLE
) -> list[CheckResult]:
    """Every check for one screw-ear option; lid_open is its lid with the curtain broken out."""
    return [
        *check_facts(board),
        board_clearance(board, base, "base"),
        *lid_clearance(board, lid, lid_open),
        opening_paths(base, lid, lid_open),
        ear_screws(base, lid, ears),
        lid_seats(base, lid),
        detent_engagement(base, lid),
        sma_room(board),
        fitting_motion(base, board),
        ribbon_plug_headroom(),
        keyhole_volumes(board),
        nut_circles(board),
        opening_size(base, lid, lid_open),
        watertight(base, "base"),
        watertight(lid, "lid"),
        printability(base, "base", top_down=False),
        printability(lid, "lid", top_down=True),
        min_print_width(base, "base", top_down=False),
        min_print_width(lid, "lid", top_down=True),
    ]
