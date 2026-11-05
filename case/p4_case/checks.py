"""Fit and printability checks (spec section 8). Every check returns CheckResult and never raises for a failure.

The board is represented by its column envelope (top and bottom per 0.25 mm cell), which is conservative: anything
between a cell's bottom and top counts as board.
"""

import math
from collections.abc import Iterable

import numpy as np
import trimesh
from build123d import Align, Box, Cylinder, Part, Pos, Rot

from p4_case import params as P
from p4_case.base import rounded_block
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
DROOP_MAX = 0.5  # small round tops (pin-holes, SMA) sag less than this; accepted
DROOP_SPAN_MAX = 8.0  # ... as long as they are small
BRIDGE_WIDTH_MAX = 3.0  # bridges here run across a 2 mm wall
FIT_BACK = 0.2  # connector fronts this far inside the cable wall's inner face while dropping in
FIT_LIFT = 0.5  # the board slides this high above the standoffs
FIT_STEP_MM = 0.25
SMA_ENVELOPE_R = 5.0  # 5.2: free radius around the SMA axis inside the wall
SMA_ENVELOPE_LEN = 12.0
HEADER_RIBBON_X = (2.1, 52.9)  # a 40-wire ribbon centred on the header
# header-slot probes run between the header-end tabs' root fillets (the ribbon's edges may brush them, spec 5.3)
SLOT_PROBE_X = (
    HEADER_RIBBON_X[0] + P.TAB_FILLET + 0.2,
    HEADER_RIBBON_X[1] - P.TAB_FILLET - 0.2,
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


def _allowed_contact(x: np.ndarray, y: np.ndarray) -> np.ndarray:
    """Standoff tops and their bores touch the board by design."""
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


def opening_paths(base: Part, lid: Part) -> CheckResult:
    """Each probe passes through the assembled case (base and lid) without touching it."""
    case = base + lid
    blocked = [
        name
        for name, probe in opening_paths_spec().items()
        if _intersection(probe, case) > BOOL_EPS
    ]
    return CheckResult(
        "every opening has a clear path",
        not blocked,
        "blocked: " + ", ".join(blocked)
        if blocked
        else "USB-C, USB-A, RJ45, microSD, buttons, header slot, SMA",
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
    """Deepest board underside part (below the PCB) that crosses a standoff top while the board slides shift_max → 0."""
    env = board.envelope
    r = P.STANDOFF_D / 2
    pcb_bottom = FACTS["pcb"].z0
    deepest = 0.0
    offsets = np.arange(-r, r + ENV_RES / 2, ENV_RES)
    for hx, hy in P.SCREW_HOLES:
        for s in np.arange(0.0, shift_max + 1e-6, ENV_RES):
            for dx in offsets:
                for dy in offsets:
                    if dx * dx + dy * dy > r * r:
                        continue
                    b = env.bottom(hx + dx, hy + dy - s)
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


def lid_clearance(board: BoardModel, lid: Part) -> list[CheckResult]:
    """The lid clears the board, the SMA body, a ribbon plug and the ribbon between the tab roots."""
    results = [board_clearance(board, lid, "lid")]
    others = {
        "SMA body": sma_envelope(),
        "ribbon plug": ribbon_plug_block(),
        "ribbon": _box(
            HEADER_RIBBON_X[0]
            + P.TAB_FILLET,  # the ribbon may brush the header-end tabs (5.3); not counted
            HEADER_RIBBON_X[1] - P.TAB_FILLET,
            FACTS["header"].y0,
            P.INNER[3],
            P.HEADER_BODY_TOP + P.RIBBON_PLUG[2],
            board_z(P.LID_UNDERSIDE) - 0.05,
        ),
    }
    hit = [k for k, body in others.items() if _intersection(body, lid) > BOOL_EPS]
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


def opening_size(base: Part, lid: Part) -> CheckResult:
    """No way into the assembled case is wider than CELL: lid cells measured, the header-slot passage probed."""
    widths = opening_widths(lid)
    widest = max(widths) if widths else 0.0
    case = base + lid

    # slabs across the wall's inner face, between the tabs
    def slab(z0: float, z1: float) -> Part:
        return _box(*SLOT_PROBE_X, P.INNER[3] - 0.3, P.INNER[3] + 0.3, z0, z1)

    # find the real slot floor: walk down from the lid underside while a thin slab stays clear
    step = 0.05
    floor = board_z(P.LID_UNDERSIDE) - step
    while (
        floor > board_z(P.STANDOFF_TOP)
        and _intersection(slab(floor - step, floor), case) <= BOOL_EPS
    ):
        floor -= step
    fits = _intersection(slab(floor + 0.01, floor + P.CELL - 0.05), case) <= BOOL_EPS
    too_big = _intersection(slab(floor + 0.01, floor + P.CELL + 0.1), case) > BOOL_EPS
    ok = widest <= P.CELL + 1e-6 and fits and too_big
    return CheckResult(
        "no opening wider than 3.5 mm",
        ok,
        f"widest lid opening {widest:.2f} mm; header-slot passage {'≥' if fits else '<'} {P.CELL - 0.05:.2f} and "
        f"{'<' if too_big else '≥'} {P.CELL + 0.1:.2f} mm",
    )


def lid_seats(base: Part, lid: Part) -> CheckResult:
    """The fitted lid does not overlap the base anywhere (tabs, bumps in their dents, flange in the rebate)."""
    overlap = _intersection(base, lid)
    return CheckResult(
        "lid seats on the base", overlap <= BOOL_EPS, f"overlap {overlap:.3f} mm³"
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


# --- all ---------------------------------------------------------------------------------------------------------------


def run_all(board: BoardModel, base: Part, lid: Part) -> list[CheckResult]:
    return [
        *check_facts(board),
        board_clearance(board, base, "base"),
        *lid_clearance(board, lid),
        opening_paths(base, lid),
        lid_seats(base, lid),
        fitting_motion(base, board),
        ribbon_plug_headroom(),
        keyhole_volumes(board),
        nut_circles(board),
        opening_size(base, lid),
        watertight(base, "base"),
        watertight(lid, "lid"),
        printability(base, "base", top_down=False),
        printability(lid, "lid", top_down=True),
    ]
