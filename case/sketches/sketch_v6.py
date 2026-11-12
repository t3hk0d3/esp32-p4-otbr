"""Throwaway sketch generator, v6 proposals: side screw ears, break-off header curtain, microSD wall, round SMA hole.

Sections are drawn to scale from params plus the proposed v6 numbers below; board parts come from the vendor model's
column envelope, so clearances are real.

Run: PYTHONPATH=case python case/sketches/sketch_v6.py .artifacts/board/board.stp <out_dir>
"""

import math
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.patches import Circle, Polygon, Rectangle
from p4_case import params as P
from p4_case.board import FACTS, load_board
from p4_case.params import case_z

# --- proposed v6 numbers (case Z) ---
GROW = 1.0  # case 1 mm taller
TOP = P.TOP + GROW  # 28.6
REBATE_FLOOR = P.REBATE_FLOOR + GROW  # 27.6: flange stays 1 mm
UNDER = P.LID_UNDERSIDE  # 25.6: unchanged, so nothing inside moves
EAR_Y = 32.0  # ear centre along the side wall
EAR_LEN = 10.0
EAR_T = 1.6  # ear thickness = pocket depth in the 4 mm side wall
SCREW_Z = 22.3
EAR_BOTTOM = SCREW_Z - EAR_LEN / 2  # half-circle bottom, concentric with the screw
EAR_CLEAR_D = 2.9  # clearance hole in the ear
WALL_PILOT_D = 2.2  # pilot in the wall, as the standoffs
CSK_D = 5.0
SLOT_FLOOR = 20.6  # header slot floor: 5 mm clear under the lid underside
GROOVE_W, GROOVE_D = 1.0, 0.8
CURTAIN_T = P.WALL - 2 * 0.2  # 1.6
NECK = 0.8  # material left at the break line (V-notch both faces)
SEGMENTS = 4
SLIT = 0.4
SD_WALL_IN = 1.0  # cable wall thickened inward by this, above the microSD slot only
SCOOP_DEPTH = 2.2
SCOOP_RISE = 3.0
SCOOP_X = (40.5, 50.5)

BASE, LID, BOARD, NEW, CUT = "#cfd3d9", "#9fb2f5", "#7cc49a", "#f2a65a", "#ffffff"
EDGE = "#3a3f47"


def poly(ax, pts, fc, **kw):
    ax.add_patch(Polygon(pts, closed=True, fc=fc, ec=EDGE, lw=0.7, **kw))


def rect(ax, x0, x1, z0, z1, fc, **kw):
    ax.add_patch(Rectangle((x0, z0), x1 - x0, z1 - z0, fc=fc, ec=EDGE, lw=0.7, **kw))


def board_profile_x(env, y: float, xs: np.ndarray):
    """Board columns along X at one Y, in case Z: (x, bottom, top) where the board is."""
    out = []
    for x in xs:
        t = env.top(x, y)
        if not math.isnan(t):
            out.append((x, case_z(env.bottom(x, y)), case_z(t)))
    return out


def draw_board_x(ax, env, y, x0, x1):
    xs = np.arange(x0, x1, 0.25)
    for x, b, t in board_profile_x(env, y, xs):
        ax.add_patch(Rectangle((x, b), 0.25, t - b, fc=BOARD, ec="none", alpha=0.8))


def ear_panel(ax, env):
    """Section across the left wall at the ear (plane Y = EAR_Y): ear outside, in a pocket in the wall's outer face."""
    ix0 = P.INNER[0]
    ox0 = P.OUTER[0]
    pocket = (
        ox0 + EAR_T
    )  # the pocket's back face: the ear sits flush with the wall's outer face
    # base: in the ear zone the wall top drops to the lid's underside, and the outer face has the ear pocket
    poly(
        ax,
        [
            (pocket, EAR_BOTTOM - 0.3),
            (ox0, EAR_BOTTOM - 0.3),
            (ox0, 0),
            (12, 0),
            (12, P.PLATE_T),
            (ix0 + 2.5, P.PLATE_T),
            (ix0, P.PLATE_T + 2.5),
            (ix0, UNDER),
            (pocket, UNDER),
        ],
        BASE,
    )
    draw_board_x(ax, env, EAR_Y, ix0, 12)
    # wall pilot, horizontal, through the 2.4 mm left behind the pocket
    rect(ax, pocket, ix0, SCREW_Z - WALL_PILOT_D / 2, SCREW_Z + WALL_PILOT_D / 2, CUT)
    # lid: full 3 mm thickness runs out over the wall to the outer face here
    poly(ax, [(ox0, UNDER), (12, UNDER), (12, TOP), (ox0, TOP)], LID)
    # the ear hangs from the lid's edge into the pocket; clearance hole and countersink for a flat head
    rect(ax, ox0, pocket, EAR_BOTTOM, UNDER + 0.01, NEW)
    rect(ax, ox0, pocket, SCREW_Z - EAR_CLEAR_D / 2, SCREW_Z + EAR_CLEAR_D / 2, CUT)
    poly(
        ax,
        [
            (ox0, SCREW_Z - CSK_D / 2),
            (ox0 + (CSK_D - EAR_CLEAR_D) / 2, SCREW_Z - EAR_CLEAR_D / 2),
            (ox0 + (CSK_D - EAR_CLEAR_D) / 2, SCREW_Z + EAR_CLEAR_D / 2),
            (ox0, SCREW_Z + CSK_D / 2),
        ],
        CUT,
    )
    ax.plot([ox0, ox0 + 6], [SCREW_Z, SCREW_Z], color="#c0392b", lw=1.6)
    ax.annotate(
        "M2.5 × 6 flat head (or the board's pan head):\nthreads into the wall, tip pokes 2 mm in",
        (ox0 + 2.5, SCREW_Z),
        (ox0 - 3.8, SCREW_Z + 5.0),
        fontsize=7,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    ax.annotate(
        f"ear {EAR_T} thick, {EAR_LEN:g} long:\nclearance hole only, flush",
        (ox0 + 0.8, EAR_BOTTOM + 1),
        (ox0 - 3.8, 14.5),
        fontsize=7,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    ax.annotate(
        f"Ø{WALL_PILOT_D} pilot, {ix0 - pocket:.1f} mm of thread",
        (ix0 - 0.6, SCREW_Z - 0.6),
        (2.0, 19.0),
        fontsize=7,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    ax.annotate(
        f"lid 3 mm, top at {TOP:g}; runs over the wall here",
        (6, TOP - 0.5),
        (0.5, TOP + 1.6),
        fontsize=7,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    tall = max(
        (t for _, _, t in board_profile_x(env, EAR_Y, np.arange(ix0, ix0 + 7, 0.25))),
        default=0,
    )
    ax.annotate(
        f"board parts ≤ {tall:.1f}",
        (8, tall),
        (8.5, 11.5),
        fontsize=7,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    ax.set_xlim(ox0 - 4, 12)
    ax.set_ylim(5, TOP + 3)
    ax.set_title(
        f"1 · Screw ear outside (section at Y {EAR_Y:g}, left wall; right is mirrored)",
        fontsize=9,
    )
    ax.set_xlabel("board X (mm)")
    ax.set_ylabel("case Z (mm)")


def curtain_elevation(ax):
    """The header-end wall from outside: slot filled by the lid's curtain, four break-off segments."""
    ox0, ox1 = P.OUTER[0], P.OUTER[1]
    ix0, ix1 = P.INNER[0], P.INNER[1]
    rect(ax, ox0, ox1, 0, TOP, BASE)
    rect(ax, ix0, ix1, SLOT_FLOOR, TOP + 0.01, CUT)
    c = P.LID_CLEAR
    rect(ax, ix0 + c, ix1 - c, UNDER, TOP, LID)  # stays on the lid
    seg = (ix1 - ix0 - 2 * c - (SEGMENTS - 1) * SLIT) / SEGMENTS
    x = ix0 + c
    for i in range(SEGMENTS):
        rect(ax, x, x + seg, SLOT_FLOOR + 0.2, UNDER, NEW)
        ax.text(
            x + seg / 2,
            (SLOT_FLOOR + UNDER) / 2,
            f"{i + 1}",
            ha="center",
            va="center",
            fontsize=8,
        )
        x += seg + SLIT
    ax.plot([ix0, ix1], [UNDER, UNDER], ls="--", color="#c0392b", lw=1)
    ax.text(ix1 + 0.8, UNDER, "break line", fontsize=7, va="center")
    ax.text(
        (ix0 + ix1) / 2,
        SLOT_FLOOR - 1.6,
        f"{SEGMENTS} segments ≈ {seg:.1f} mm, {SLIT} mm slits; opening when broken out: "
        f"{UNDER - SLOT_FLOOR:.1f} mm × {ix1 - ix0:.0f} mm",
        ha="center",
        fontsize=7,
    )
    ax.set_xlim(ox0 - 2, ox1 + 9)
    ax.set_ylim(SLOT_FLOOR - 5, TOP + 2)
    ax.set_title(
        "2a · 40-pin slot from outside: break-off curtain on the lid", fontsize=9
    )
    ax.set_xlabel("board X (mm)")


def curtain_section(ax):
    """Section through the header-end wall (plane X = 27.5): groove in the floor, tongue, V-notch break line."""
    iy1, oy1 = P.INNER[3], P.OUTER[3]
    mid = (iy1 + oy1) / 2
    # wall below the slot, with the floor groove
    poly(
        ax,
        [
            (iy1 - 3, 0),
            (oy1, 0),
            (oy1, SLOT_FLOOR),
            (mid + GROOVE_W / 2, SLOT_FLOOR),
            (mid + GROOVE_W / 2, SLOT_FLOOR - GROOVE_D),
            (mid - GROOVE_W / 2, SLOT_FLOOR - GROOVE_D),
            (mid - GROOVE_W / 2, SLOT_FLOOR),
            (iy1, SLOT_FLOOR),
            (iy1, P.PLATE_T + 2.5),
            (iy1 - 2.5, P.PLATE_T),
            (iy1 - 3, P.PLATE_T),
        ],
        BASE,
    )
    # lid: body inside, flange, and over the slot it reaches the outer face
    rect(ax, iy1 - 3, oy1, UNDER, TOP, LID)
    t0, t1 = mid - CURTAIN_T / 2, mid + CURTAIN_T / 2
    notch = (CURTAIN_T - NECK) / 2
    tongue = GROOVE_W - 0.3
    poly(
        ax,
        [
            (t0, UNDER),
            (t0 + notch, UNDER - notch),
            (t0, UNDER - 2 * notch),
            (t0, SLOT_FLOOR + 0.2),
            (mid - tongue / 2, SLOT_FLOOR + 0.2),
            (mid - tongue / 2, SLOT_FLOOR - GROOVE_D + 0.2),
            (mid + tongue / 2, SLOT_FLOOR - GROOVE_D + 0.2),
            (mid + tongue / 2, SLOT_FLOOR + 0.2),
            (t1, SLOT_FLOOR + 0.2),
            (t1, UNDER - 2 * notch),
            (t1 - notch, UNDER - notch),
            (t1, UNDER),
        ],
        NEW,
    )
    hdr = FACTS["header"]
    plug_top = case_z(P.HEADER_BODY_TOP + P.RIBBON_PLUG[2])
    ax.add_patch(
        Rectangle(
            (hdr.y0, case_z(P.HEADER_BODY_TOP)),
            hdr.y1 - hdr.y0,
            plug_top - case_z(P.HEADER_BODY_TOP),
            fc=BOARD,
            ec="none",
            alpha=0.6,
        )
    )
    ax.text(
        (hdr.y0 + hdr.y1) / 2, plug_top - 1.5, "ribbon\nplug", ha="center", fontsize=6
    )
    ax.annotate(
        f"V-notches leave {NECK} mm:\nsnaps along a layer",
        (t1 - notch, UNDER - notch),
        (oy1 + 0.8, UNDER + 1.2),
        fontsize=7,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    ax.annotate(
        f"tongue in a {GROOVE_W}×{GROOVE_D} groove",
        (mid, SLOT_FLOOR - 0.4),
        (oy1 + 0.8, SLOT_FLOOR - 3),
        fontsize=7,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    ax.set_xlim(hdr.y0 - 1, oy1 + 9)
    ax.set_ylim(case_z(P.HEADER_BODY_TOP) - 1, TOP + 2)
    ax.set_title("2b · Section through the slot (X 27.5)", fontsize=9)
    ax.set_xlabel("board Y (mm)")


def sd_panel(ax):
    """Section through the cable wall at the microSD scoop (plane X = 45): wall thickened inward above the slot."""
    oy0, iy0 = P.OUTER[2], P.INNER[2]
    _, _, sz0, sz1 = P.SD_SLOT
    z0, z1 = case_z(sz0), case_z(sz1)
    face = oy0
    in_face = iy0 + SD_WALL_IN
    ledge_top = z1 + SCOOP_RISE + 2.0
    poly(
        ax,
        [
            (oy0, 0),
            (iy0 + 4, 0),
            (iy0 + 4, P.PLATE_T),
            (iy0 + 2.5, P.PLATE_T),
            (iy0, P.PLATE_T + 2.5),
            (iy0, z0),
            (oy0, z0),
        ],
        BASE,
    )
    # above the slot: original wall plus the inward ledge, with the scoop cut into the outer face
    poly(
        ax,
        [
            (face + SCOOP_DEPTH, z1),
            (in_face, z1),
            (in_face, ledge_top),
            (iy0, ledge_top + SD_WALL_IN),
            (iy0, TOP),
            (oy0, TOP),
            (oy0, z1 + SCOOP_RISE),
        ],
        BASE,
    )
    poly(
        ax,
        [(iy0, z1), (in_face, z1), (in_face, ledge_top), (iy0, ledge_top + SD_WALL_IN)],
        NEW,
        alpha=0.9,
    )
    sd = FACTS["sd"]
    ax.add_patch(
        Rectangle(
            (sd.y0, case_z(sd.z0)),
            6,
            case_z(sd.z1) - case_z(sd.z0),
            fc=BOARD,
            ec="none",
            alpha=0.8,
        )
    )
    ax.add_patch(
        Rectangle((-0.0, case_z(-1.6)), 6, 1.6, fc=BOARD, ec="none", alpha=0.5)
    )
    ax.add_patch(
        Rectangle(
            (oy0 - 3.5, case_z(0.15)),
            3.5 + sd.y0 + 1,
            0.75,
            fc="#555",
            ec="none",
            alpha=0.35,
        )
    )
    ax.text(oy0 - 3.3, case_z(0.15) + 1.0, "card", fontsize=6)
    ax.annotate(
        f"scoop {SCOOP_DEPTH} deep (no lip),\n{SCOOP_X[1] - SCOOP_X[0]:g} mm wide, {SCOOP_RISE} tall",
        (face + 1, z1 + 0.8),
        (oy0 - 4.5, z1 + 6),
        fontsize=7,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    ax.annotate(
        f"wall +{SD_WALL_IN} inward above the slot\n(0.8 left behind the scoop)",
        (in_face - 0.4, z1 + 2),
        (in_face + 1.2, z1 + 7),
        fontsize=7,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    ax.annotate(
        f"SD front at Y {sd.y0}",
        (sd.y0, case_z(sd.z1)),
        (sd.y0 + 1, case_z(sd.z1) - 3.5),
        fontsize=7,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    ax.set_xlim(oy0 - 5, iy0 + 8)
    ax.set_ylim(3, z1 + 10)
    ax.set_title("3 · microSD: section through the cable wall (X 45)", fontsize=9)
    ax.set_xlabel("board Y (mm)")
    ax.set_ylabel("case Z (mm)")


def sma_panel(ax):
    """The SMA hole seen from outside: round top, flat at the bottom (no teardrop)."""
    zc, r = P.SMA_CASE_Z, P.SMA_D / 2
    rect(ax, P.SMA_Y - 7, P.SMA_Y + 7, zc - 6, zc + 6, BASE)
    ax.add_patch(Circle((P.SMA_Y, zc), r, fc=CUT, ec=EDGE, lw=0.7))
    flat = zc - (P.SMA_FLAT - r)
    rect(ax, P.SMA_Y - r - 0.1, P.SMA_Y + r + 0.1, zc - r - 0.1, flat, BASE, zorder=3)
    ax.text(
        P.SMA_Y,
        zc - 5,
        f"Ø{P.SMA_D}, flat {P.SMA_FLAT} at the bottom",
        ha="center",
        fontsize=7,
    )
    ax.set_xlim(P.SMA_Y - 8, P.SMA_Y + 8)
    ax.set_ylim(zc - 7, zc + 7)
    ax.set_title("4 · SMA hole: round top again", fontsize=9)
    ax.set_xlabel("board Y (mm)")


def main() -> None:
    board_step, out = Path(sys.argv[1]), Path(sys.argv[2])
    env = load_board(board_step).envelope
    fig = plt.figure(figsize=(15, 10), dpi=110)
    gs = fig.add_gridspec(2, 3, width_ratios=[1.15, 1.6, 0.9])
    for draw, spec in (
        (ear_panel, gs[0, 0]),
        (curtain_elevation, gs[0, 1:]),
        (sd_panel, gs[1, 0]),
        (curtain_section, gs[1, 1]),
        (sma_panel, gs[1, 2]),
    ):
        ax = fig.add_subplot(spec)
        ax.set_aspect("equal")
        draw(ax, env) if draw is ear_panel else draw(ax)
    handles = [Rectangle((0, 0), 1, 1, fc=c, ec=EDGE) for c in (BASE, LID, NEW, BOARD)]
    fig.legend(
        handles,
        ["base", "lid", "new in v6", "board / plug"],
        loc="upper right",
        ncol=4,
        fontsize=8,
    )
    fig.suptitle(
        "Case v6 proposals (to scale; case 1 mm taller, lid 3 mm)", fontsize=12
    )
    fig.tight_layout(rect=(0, 0, 1, 0.96))
    out.mkdir(parents=True, exist_ok=True)
    fig.savefig(out / "v6-proposals.svg")
    fig.savefig(out / "v6-proposals.png")


def u_shape(r: float, top: float) -> list[tuple[float, float]]:
    """Outline (Y, Z) with straight sides up to top and a half-circle bottom of radius r around the screw axis."""
    arc = [
        (EAR_Y + r * math.cos(a), SCREW_Z + r * math.sin(a))
        for a in np.linspace(math.pi, 2 * math.pi, 40)
    ]
    return [(EAR_Y - r, top), *arc, (EAR_Y + r, top)]


def ear_side(out: Path) -> None:
    """The left side wall seen from outside (looking along +X): the ear in its pocket, the lid above it."""
    ox_y0, ox_y1 = P.OUTER[2], P.OUTER[3]
    gap = 0.2
    y0, y1 = EAR_Y - EAR_LEN / 2, EAR_Y + EAR_LEN / 2
    fig, axes = plt.subplots(
        1, 2, figsize=(14, 5), dpi=120, gridspec_kw={"width_ratios": [2.2, 1]}
    )
    for ax, (ya, yb, za, zb), title in (
        (
            axes[0],
            (ox_y0 - 2, ox_y1 + 2, -1, TOP + 2),
            "Left side wall from outside (cable end on the left)",
        ),
        (axes[1], (y0 - 4, y1 + 4, 14.5, TOP + 1.5), "Close-up of the ear"),
    ):
        ax.add_patch(
            Rectangle((ox_y0, 0), ox_y1 - ox_y0, TOP, fc=BASE, ec=EDGE, lw=0.8)
        )
        # cut in the wall: pocket for the ear (with clearance) up through the wall top
        poly(ax, u_shape(EAR_LEN / 2 + gap, TOP + 0.01), CUT)
        # the lid fills the cut above its underside, the ear hangs below
        ax.add_patch(
            Rectangle((y0, UNDER), EAR_LEN, TOP - UNDER, fc=LID, ec=EDGE, lw=0.8)
        )
        poly(ax, u_shape(EAR_LEN / 2, UNDER), NEW)
        ax.add_patch(Circle((EAR_Y, SCREW_Z), CSK_D / 2, fc="#e8e8e8", ec=EDGE, lw=0.8))
        ax.add_patch(Circle((EAR_Y, SCREW_Z), EAR_CLEAR_D / 2, fc=CUT, ec=EDGE, lw=0.8))
        ax.plot([y0, y1], [UNDER, UNDER], ls="--", color="#555", lw=0.6)
        for key in ("button_a", "button_b"):
            b = FACTS[key]
            ax.add_patch(
                Circle(
                    ((b.y0 + b.y1) / 2, case_z((b.z0 + b.z1) / 2)),
                    P.BUTTON_FUNNEL_D / 2,
                    fc=CUT,
                    ec=EDGE,
                    lw=0.6,
                )
            )
        ax.set_xlim(ya, yb)
        ax.set_ylim(za, zb)
        ax.set_aspect("equal")
        ax.set_title(title, fontsize=10)
        ax.set_xlabel("board Y (mm)")
        ax.set_ylabel("case Z (mm)")
    ax = axes[1]
    ax.annotate(
        "lid runs out to the face here",
        (EAR_Y + 3, (UNDER + TOP) / 2),
        (y1 + 0.5, TOP + 0.6),
        fontsize=8,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    ax.annotate(
        f"ear {EAR_LEN:g} wide, half-circle bottom\n(R{EAR_LEN / 2:g} around the screw), {EAR_T} thick",
        (EAR_Y - 3, EAR_BOTTOM + 1.2),
        (y0 - 3.8, 15.2),
        fontsize=8,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    ax.annotate(
        f"M2.5 flat head\n(countersink Ø{CSK_D:g})",
        (EAR_Y + CSK_D / 2 * 0.7, SCREW_Z - 1.5),
        (y1 + 0.3, 17.2),
        fontsize=8,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    ax.annotate(
        "0.2 mm gap all round",
        (y1 + 0.1, SCREW_Z + 1),
        (y1 + 0.8, 21.0),
        fontsize=8,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    axes[0].text(8, 4, "BOOT / RESET", fontsize=7, ha="center")
    fig.tight_layout()
    fig.savefig(out / "v6-ear-side.png")
    fig.savefig(out / "v6-ear-side.svg")


# --- inside ear (option I), reinforced ---
IN_LEN = 12.0
IN_T = 5.0  # reaches this far in from its wall-facing face
IN_BOTTOM = 18.5
IN_BUTTRESS = (
    UNDER - IN_BOTTOM
)  # 45° buttress behind the ear, from its bottom edge up to the lid: 7.1 mm deep
IN_GUSSET = 3.0  # 45° gussets at both ends, under the lid
IN_SCREW_Z = 22.0
IN_PILOT_D = 2.4
IN_PILOT_STOP = 0.4  # blind: stops this short of the ear's inner face
WALL_CLEAR_D = 3.0


def inside_section(ax, env):
    """Option I: section across the left wall at Y 32, ear inside the case, screw from outside through the wall."""
    ix0, ox0 = P.INNER[0], P.OUTER[0]
    poly(
        ax,
        [
            (ox0, 0),
            (15, 0),
            (15, P.PLATE_T),
            (ix0 + 2.5, P.PLATE_T),
            (ix0, P.PLATE_T + 2.5),
            (ix0, REBATE_FLOOR),
            (ix0 - P.REBATE_W, REBATE_FLOOR),
            (ix0 - P.REBATE_W, TOP),
            (ox0, TOP),
        ],
        BASE,
    )
    draw_board_x(ax, env, EAR_Y, ix0, 15)
    fx = ix0 - P.REBATE_W + P.LID_CLEAR
    poly(ax, [(fx, REBATE_FLOOR), (15, REBATE_FLOOR), (15, TOP), (fx, TOP)], LID)
    rect(ax, ix0 + P.LID_CLEAR, 15, UNDER, REBATE_FLOOR + 0.01, LID)
    ex = ix0 + P.TAB_CLEAR
    poly(
        ax,
        [
            (ex, UNDER),
            (ex + IN_T + IN_BUTTRESS, UNDER),
            (ex + IN_T, IN_BOTTOM),
            (ex, IN_BOTTOM),
        ],
        NEW,
    )
    ax.plot([ex + IN_T, ex + IN_T], [IN_BOTTOM, UNDER], ls=":", color=EDGE, lw=0.7)
    ax.annotate(
        "45° buttress: the layer at the pilot\nis ~2× bigger, so a split must break more",
        (ex + IN_T + 2.0, IN_SCREW_Z + 1.6),
        (ex + 2.0, TOP + 1.0),
        fontsize=7,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    rect(
        ax, ox0, ix0, IN_SCREW_Z - WALL_CLEAR_D / 2, IN_SCREW_Z + WALL_CLEAR_D / 2, CUT
    )
    poly(
        ax,
        [
            (ox0, IN_SCREW_Z - CSK_D / 2),
            (ox0 + (CSK_D - WALL_CLEAR_D) / 2, IN_SCREW_Z - WALL_CLEAR_D / 2),
            (ox0 + (CSK_D - WALL_CLEAR_D) / 2, IN_SCREW_Z + WALL_CLEAR_D / 2),
            (ox0, IN_SCREW_Z + CSK_D / 2),
        ],
        CUT,
    )
    rect(
        ax,
        ex,
        ex + IN_T - IN_PILOT_STOP,
        IN_SCREW_Z - IN_PILOT_D / 2,
        IN_SCREW_Z + IN_PILOT_D / 2,
        CUT,
    )
    ax.plot([ox0, ox0 + 10], [IN_SCREW_Z, IN_SCREW_Z], color="#c0392b", lw=1.6)
    ax.annotate(
        "M2.5 × 10 flat head,\nflush in the wall",
        (ox0 + 0.6, IN_SCREW_Z + 1.4),
        (ox0 - 4.3, TOP + 1.2),
        fontsize=7,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    ax.annotate(
        f"Ø{IN_PILOT_D} blind pilot:\n{IN_T - IN_PILOT_STOP:.1f} mm of thread",
        (ex + 3, IN_SCREW_Z - 0.6),
        (ex + 7.5, 17.0),
        fontsize=7,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    reach = ex + IN_T + IN_BUTTRESS
    tall = max(
        (
            t
            for y in np.arange(EAR_Y - IN_LEN / 2, EAR_Y + IN_LEN / 2, 0.25)
            for _, _, t in board_profile_x(env, y, np.arange(ix0, reach + 0.5, 0.25))
        ),
        default=0,
    )
    ax.annotate(
        f"board parts under the ear and buttress ≤ {tall:.1f}",
        (8, 14.3),
        (5.0, 12.0),
        fontsize=7,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    ax.set_xlim(ox0 - 4.5, 15)
    ax.set_ylim(5, TOP + 3.5)
    ax.set_title(
        "I · inside ear with buttress: section at Y 32 (left wall)", fontsize=9
    )
    ax.set_xlabel("board X (mm)")
    ax.set_ylabel("case Z (mm)")


def inside_view(ax, env):
    """Option I seen from inside the case, looking at the left wall: ear block, end gussets, board parts below."""
    y0, y1 = EAR_Y - IN_LEN / 2, EAR_Y + IN_LEN / 2
    ix0 = P.INNER[0]
    rect(ax, y0 - 9, y1 + 9, 0, TOP, BASE)
    rect(ax, y0 - 9, y1 + 9, UNDER, TOP, LID)
    g = IN_GUSSET
    poly(
        ax,
        [
            (y0 - g, UNDER),
            (y1 + g, UNDER),
            (y1, UNDER - g),
            (y1, IN_BOTTOM),
            (y0, IN_BOTTOM),
            (y0, UNDER - g),
        ],
        NEW,
    )
    ax.add_patch(
        Circle((EAR_Y, IN_SCREW_Z), IN_PILOT_D / 2, fc=CUT, ec=EDGE, lw=0.7, ls="--")
    )
    ys = np.arange(y0 - 9, y1 + 9, 0.25)
    tops = []
    for y in ys:
        t = [env.top(x, y) for x in np.arange(ix0, ix0 + 7, 0.25)]
        t = [v for v in t if not math.isnan(v)]
        tops.append(case_z(max(t)) if t else 0.0)
    ax.fill_between(ys, case_z(-1.6), tops, color=BOARD, alpha=0.8, lw=0)
    ax.annotate(
        f"{IN_LEN:g} mm block, {IN_T:g} thick, to Z {IN_BOTTOM}",
        (EAR_Y, IN_BOTTOM + 0.5),
        (y0 - 8, 14.5),
        fontsize=7,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    ax.annotate(
        "45° end gussets",
        (y1 + 1.2, UNDER - 0.8),
        (y1 + 1.5, 19.0),
        fontsize=7,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    ax.text(
        y0 - 8,
        TOP + 0.4,
        f"the buttress behind covers ~{IN_T + IN_BUTTRESS:.0f} × {IN_LEN:g} mm of the lid's "
        "underside: the grille cells over it go",
        fontsize=7,
    )
    ax.text(y0 - 8, 9.5, "board parts within 7 mm of the wall", fontsize=7)
    ax.set_xlim(y0 - 9, y1 + 9)
    ax.set_ylim(5, TOP + 1.5)
    ax.set_title("I · inside ear seen from inside the case", fontsize=9)
    ax.set_xlabel("board Y (mm)")


def outside_view(ax):
    """Option O seen from outside (the close-up of ear_side)."""
    gap = 0.2
    y0, y1 = EAR_Y - EAR_LEN / 2, EAR_Y + EAR_LEN / 2
    rect(ax, y0 - 9, y1 + 9, 0, TOP, BASE)
    poly(ax, u_shape(EAR_LEN / 2 + gap, TOP + 0.01), CUT)
    rect(ax, y0, y1, UNDER, TOP, LID)
    poly(ax, u_shape(EAR_LEN / 2, UNDER), NEW)
    ax.add_patch(Circle((EAR_Y, SCREW_Z), CSK_D / 2, fc="#e8e8e8", ec=EDGE, lw=0.8))
    ax.add_patch(Circle((EAR_Y, SCREW_Z), EAR_CLEAR_D / 2, fc=CUT, ec=EDGE, lw=0.8))
    ax.annotate(
        f"ear {EAR_LEN:g} wide, R{EAR_LEN / 2:g} bottom, {EAR_T} thick, flush",
        (EAR_Y - 3, EAR_BOTTOM + 1.2),
        (y0 - 8, 12.0),
        fontsize=7,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    ax.annotate(
        "lid runs out to the face here",
        (EAR_Y + 3, (UNDER + TOP) / 2),
        (y1 + 0.5, TOP + 0.5),
        fontsize=7,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    ax.set_xlim(y0 - 9, y1 + 9)
    ax.set_ylim(5, TOP + 1.5)
    ax.set_title("O · outside ear seen from outside the case", fontsize=9)
    ax.set_xlabel("board Y (mm)")


def ears_compare(board_step: Path, out: Path) -> None:
    env = load_board(board_step).envelope
    fig, axes = plt.subplots(2, 2, figsize=(13, 10), dpi=120)
    ear_panel(axes[0, 0], env)
    axes[0, 0].set_title("O · outside ear: section at Y 32 (left wall)", fontsize=9)
    outside_view(axes[0, 1])
    inside_section(axes[1, 0], env)
    inside_view(axes[1, 1], env)
    for ax in axes.flat:
        ax.set_aspect("equal")
    handles = [Rectangle((0, 0), 1, 1, fc=c, ec=EDGE) for c in (BASE, LID, NEW, BOARD)]
    fig.legend(
        handles, ["base", "lid", "ear", "board"], loc="upper right", ncol=4, fontsize=8
    )
    fig.suptitle(
        "Screw ears, both options (to scale; right wall mirrored)\n"
        f"O: thread {P.INNER[0] - (P.OUTER[0] + EAR_T):.1f} mm in the base wall, ear clamped against the wall"
        f"   ·   I: thread {IN_T - IN_PILOT_STOP:.1f} mm in the lid's ear, along its layers",
        fontsize=11,
    )
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    fig.savefig(out / "v6-ears.png")
    fig.savefig(out / "v6-ears.svg")


if __name__ == "__main__":
    if len(sys.argv) > 3 and sys.argv[3] == "ear-side":
        ear_side(Path(sys.argv[2]))
    elif len(sys.argv) > 3 and sys.argv[3] == "ears":
        ears_compare(Path(sys.argv[1]), Path(sys.argv[2]))
    else:
        main()
