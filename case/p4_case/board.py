"""Board facts and the vendor board model (spec section 3).

All coordinates are in the board frame of the vendor STEP: X 0-55 (button edge at 0), Y 0-68 (cable edge at 0), Z 0 at
the PCB's top face. The constants below are what the case is designed around; `check_facts` compares them with the
model so a changed vendor file is caught.
"""

import hashlib
import math
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import trimesh
from build123d import GeomType, export_stl, import_step

from p4_case.results import CheckResult


@dataclass(frozen=True)
class BBox:
    """Axis-aligned box in the board frame (mm)."""

    x0: float
    x1: float
    y0: float
    y1: float
    z0: float
    z1: float


# Bounding boxes of the named parts, measured from the STEP (spec section 3).
FACTS: dict[str, BBox] = {
    "pcb": BBox(0.0, 55.0, 0.0, 68.0, -1.6, 0.0),
    "usb": BBox(
        4.26, 19.93, -1.70, 15.76, -3.70, 15.99
    ),  # USB-C below, USB-A above; faces -Y
    "rj45": BBox(21.86, 37.90, -1.99, 19.41, -3.90, 13.30),
    "sd": BBox(39.63, 50.98, 1.23, 12.48, 0.0, 1.32),
    "button_a": BBox(
        -0.35, 3.65, 3.35, 7.95, -0.50, 3.30
    ),  # side-actuated, plunger faces -X
    "button_b": BBox(-0.35, 3.65, 10.05, 14.65, -0.50, 3.30),
    "led": BBox(0.30, 1.62, 17.35, 19.10, 0.0, 0.50),
    "header": BBox(2.10, 52.90, 61.46, 66.54, -3.00, 8.30),
    "transformer": BBox(33.75, 47.05, 27.65, 44.42, 0.0, 12.0),
}
# The big capacitors are part of the board body solid, so they are measured from the envelope over this region.
CAPS_REGION = (48.0, 54.5, 30.0, 45.0)
CAPS_TOP = 7.8  # the spec's 'about 7'; measured 7.8, the board body's highest point
CAPS_TOP_TOL = 0.3
HOLES: tuple[tuple[float, float], ...] = (
    (3.5, 24.3),
    (51.5, 24.3),
    (3.5, 57.3),
    (51.5, 57.3),
)
HOLE_RADIUS = 1.5
HOLE_MERGE = 0.5  # faces of one hole wall touch; neighbouring holes are 30+ mm apart
FACT_TOL = 0.05

# STEP labels of the named parts; buttons share one label and are told apart by Y.
LABELS = {
    "pcb": "BOARD-1069",
    "usb": "USB_TPYPE-000",
    "rj45": "RJ45_HR911105A",
    "sd": "SD1_ASM",
    "button": "SWITCHER-4_5X3_3XH3_4",
    "led": "LED1_ASM",
    "header": "PIN_HEADER-2_54MM_2X20PIN",
    "transformer": "T1_ASM",
}
PCB_THICKNESS = 1.6

ENV_RES = 0.25  # envelope grid (mm)
ENV_ORIGIN = (-3.0, -4.0)  # grid covers X -3..59, Y -4..72
ENV_SIZE = (62.0, 76.0)
ENV_SAMPLES = 4_000_000
TESS_TOL = 0.05
MAKE_HINT = "run `make case-board` to download the board model"


class ColumnEnvelope:
    """Top and bottom height maps of the board on a regular XY grid; NaN where the board has no material."""

    def __init__(self, top: np.ndarray, bottom: np.ndarray):
        self.top_map = top
        self.bottom_map = bottom

    @classmethod
    def from_points(cls, pts: np.ndarray) -> "ColumnEnvelope":
        w = int(ENV_SIZE[0] / ENV_RES)
        h = int(ENV_SIZE[1] / ENV_RES)
        ix = np.floor((pts[:, 0] - ENV_ORIGIN[0]) / ENV_RES).astype(int)
        iy = np.floor((pts[:, 1] - ENV_ORIGIN[1]) / ENV_RES).astype(int)
        ok = (ix >= 0) & (ix < w) & (iy >= 0) & (iy < h)
        lin = iy[ok] * w + ix[ok]
        top = np.full(h * w, np.nan)
        bottom = np.full(h * w, np.nan)
        np.fmax.at(top, lin, pts[ok, 2])
        np.fmin.at(bottom, lin, pts[ok, 2])
        return cls(top.reshape(h, w), bottom.reshape(h, w))

    def _cell(self, x: float, y: float) -> tuple[int, int] | None:
        ix = math.floor((x - ENV_ORIGIN[0]) / ENV_RES)
        iy = math.floor((y - ENV_ORIGIN[1]) / ENV_RES)
        h, w = self.top_map.shape
        if 0 <= ix < w and 0 <= iy < h:
            return iy, ix
        return None

    def top(self, x: float, y: float) -> float:
        c = self._cell(x, y)
        return float(self.top_map[c]) if c else math.nan

    def bottom(self, x: float, y: float) -> float:
        c = self._cell(x, y)
        return float(self.bottom_map[c]) if c else math.nan

    def _slice(self, region: tuple[float, float, float, float]):
        x0, x1, y0, y1 = region
        ix0 = max(0, math.floor((x0 - ENV_ORIGIN[0]) / ENV_RES))
        ix1 = math.ceil((x1 - ENV_ORIGIN[0]) / ENV_RES)
        iy0 = max(0, math.floor((y0 - ENV_ORIGIN[1]) / ENV_RES))
        iy1 = math.ceil((y1 - ENV_ORIGIN[1]) / ENV_RES)
        return slice(iy0, iy1), slice(ix0, ix1)

    def region_max_top(self, region: tuple[float, float, float, float]) -> float:
        """Highest board point over the XY region (NaN if the board is absent there)."""
        sl = self._slice(region)
        block = self.top_map[sl]
        return float(np.nanmax(block)) if np.isfinite(block).any() else math.nan

    def region_min_bottom(self, region: tuple[float, float, float, float]) -> float:
        """Lowest board point over the XY region (NaN if the board is absent there)."""
        sl = self._slice(region)
        block = self.bottom_map[sl]
        return float(np.nanmin(block)) if np.isfinite(block).any() else math.nan

    def free_height_above_plate(
        self, region: tuple[float, float, float, float], plate_top_z: float
    ) -> float:
        """Smallest gap between a plate at `plate_top_z` and the board's underside over the region."""
        low = self.region_min_bottom(region)
        return math.inf if math.isnan(low) else low - plate_top_z


@dataclass
class BoardModel:
    """The loaded vendor model: named part boxes, the board body solid and the column envelope."""

    parts: dict[str, list[BBox]]
    body: object = field(
        repr=False
    )  # build123d shape of the board body (PCB and modules)
    envelope: ColumnEnvelope = field(repr=False)

    @property
    def pcb(self) -> BBox:
        """The PCB slab: the board body's XY outline and its known thickness below Z 0."""
        b = self.parts[LABELS["pcb"]][0]
        return BBox(b.x0, b.x1, b.y0, b.y1, -PCB_THICKNESS, 0.0)


def _bbox(shape) -> BBox:
    b = shape.bounding_box()
    return BBox(b.min.X, b.max.X, b.min.Y, b.max.Y, b.min.Z, b.max.Z)


def _envelope(shape, cache: Path) -> ColumnEnvelope:
    if cache.exists():
        data = np.load(cache)
        return ColumnEnvelope(data["top"], data["bottom"])
    # export_stl skips the few vendor faces that do not triangulate; Shape.tessellate raises on them
    with tempfile.TemporaryDirectory() as tmp:
        stl = Path(tmp) / "board.stl"
        export_stl(shape, stl, tolerance=TESS_TOL)
        mesh = trimesh.load(stl)
    pts, _ = trimesh.sample.sample_surface(mesh, ENV_SAMPLES, seed=1)
    env = ColumnEnvelope.from_points(pts)
    np.savez_compressed(cache, top=env.top_map, bottom=env.bottom_map)
    return env


def load_board(step_path: Path) -> BoardModel:
    """Loads the vendor STEP once. The envelope is cached next to the STEP, keyed by the file's hash."""
    step_path = Path(step_path)
    if not step_path.is_file():
        raise FileNotFoundError(f"{step_path} not found; {MAKE_HINT}")
    digest = hashlib.sha256(step_path.read_bytes()).hexdigest()[:16]
    assembly = import_step(step_path)
    parts: dict[str, list[BBox]] = {}
    body = None
    for child in assembly.children:
        parts.setdefault(child.label, []).append(_bbox(child))
        if child.label == LABELS["pcb"]:
            body = child
    if body is None:
        raise ValueError(f"{step_path}: no {LABELS['pcb']} part")
    cache = step_path.with_name(f"{step_path.stem}.envelope-{digest}.npz")
    return BoardModel(parts=parts, body=body, envelope=_envelope(assembly, cache))


def _hole_centres(board: BoardModel) -> list[tuple[float, float]]:
    """Centres of the Ø3 through-holes: vertical radius-1.5 cylinder faces crossing the PCB, merged per hole.

    The vendor model splits each hole wall into several faces, so faces closer than HOLE_MERGE are one hole and its
    centre is the middle of their combined XY extent.
    """
    groups: list[list[float]] = []  # [x0, x1, y0, y1]
    for face in board.body.faces().filter_by(GeomType.CYLINDER):
        if abs(face.radius - HOLE_RADIUS) > FACT_TOL:
            continue
        b = face.bounding_box()
        if b.max.Z - b.min.Z < PCB_THICKNESS - FACT_TOL or b.max.Z > FACT_TOL:
            continue
        box = [b.min.X, b.max.X, b.min.Y, b.max.Y]
        for g in groups:
            if (
                box[0] < g[1] + HOLE_MERGE
                and g[0] < box[1] + HOLE_MERGE
                and (box[2] < g[3] + HOLE_MERGE and g[2] < box[3] + HOLE_MERGE)
            ):
                g[:] = [
                    min(g[0], box[0]),
                    max(g[1], box[1]),
                    min(g[2], box[2]),
                    max(g[3], box[3]),
                ]
                break
        else:
            groups.append(box)
    return [((g[0] + g[1]) / 2, (g[2] + g[3]) / 2) for g in groups]


def measure(board: BoardModel) -> dict:
    """Measures the spec section 3 facts from the model: part boxes, the capacitor height, the mounting holes."""
    p = board.parts
    buttons = sorted(p[LABELS["button"]], key=lambda b: b.y0)
    caps_top = board.envelope.region_max_top(CAPS_REGION)
    x0, x1, y0, y1 = CAPS_REGION
    return {
        "pcb": board.pcb,
        "usb": p[LABELS["usb"]][0],
        "rj45": p[LABELS["rj45"]][0],
        "sd": p[LABELS["sd"]][0],
        "button_a": buttons[0],
        "button_b": buttons[1],
        "led": p[LABELS["led"]][0],
        "header": p[LABELS["header"]][0],
        "transformer": p[LABELS["transformer"]][0],
        "caps_top": BBox(x0, x1, y0, y1, 0.0, caps_top),
        "holes": _hole_centres(board),
    }


def _box_diff(a: BBox, b: BBox) -> float:
    return max(
        abs(getattr(a, k) - getattr(b, k)) for k in ("x0", "x1", "y0", "y1", "z0", "z1")
    )


def _fmt(b: BBox) -> str:
    return f"X {b.x0:.2f}..{b.x1:.2f} Y {b.y0:.2f}..{b.y1:.2f} Z {b.z0:.2f}..{b.z1:.2f}"


def check_facts(
    board: BoardModel, expected: dict[str, BBox] = FACTS
) -> list[CheckResult]:
    """One result per fact (and one for the mounting holes): does the model still match the constants?"""
    got = measure(board)
    results = []
    for name, want in expected.items():
        have = got[name]
        diff = _box_diff(have, want)
        results.append(
            CheckResult(
                f"board fact {name}",
                diff <= FACT_TOL,
                f"expected {_fmt(want)}, model {_fmt(have)} (max diff {diff:.2f})",
            )
        )
    caps = got["caps_top"].z1
    results.append(
        CheckResult(
            "board fact caps_top",
            abs(caps - CAPS_TOP) <= CAPS_TOP_TOL,
            f"expected {CAPS_TOP:.2f}, model {caps:.2f}",
        )
    )
    holes = got["holes"]
    worst = (
        max(min(math.dist(h, w) for h in holes) for w in HOLES) if holes else math.inf
    )
    results.append(
        CheckResult(
            "board fact holes",
            len(holes) == len(HOLES) and worst <= FACT_TOL,
            f"{len(holes)} holes found, worst offset {worst:.3f} mm",
        )
    )
    return results
