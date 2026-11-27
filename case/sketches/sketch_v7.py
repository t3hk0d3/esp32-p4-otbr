"""Throwaway sketch, v7: no screw ears; two keying teeth on each side wall, above the corner snap tabs.

Run: PYTHONPATH=case python case/sketches/sketch_v7.py <out_dir>
"""

import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Polygon, Rectangle
from p4_case import params as P
from p4_case.base import tab_y_centres

BASE, LID, NEW, CUT, EDGE = "#cfd3d9", "#9fb2f5", "#f2a65a", "#ffffff", "#3a3f47"
TOOTH_LEN = 6.0  # along the wall
TOOTH_DEPTH = 2.0  # down from the top (case Z 26.6-28.6)
LEAD = 0.5  # 45° lead-in on the tooth's lower ends
GAP = 0.2


def rect(ax, x0, x1, z0, z1, fc):
    ax.add_patch(Rectangle((x0, z0), x1 - x0, z1 - z0, fc=fc, ec=EDGE, lw=0.7))


def tooth(ax, yc, fc, grow=0.0):
    h, z0 = TOOTH_LEN / 2 + grow, P.TOP - TOOTH_DEPTH - grow
    pts = [
        (yc - h, P.TOP + 0.01),
        (yc - h, z0 + LEAD),
        (yc - h + LEAD, z0),
        (yc + h - LEAD, z0),
        (yc + h, z0 + LEAD),
        (yc + h, P.TOP + 0.01),
    ]
    ax.add_patch(Polygon(pts, closed=True, fc=fc, ec=EDGE, lw=0.7))


def main() -> None:
    out = Path(sys.argv[1])
    fig, (a, b) = plt.subplots(
        1, 2, figsize=(14, 4.6), dpi=120, gridspec_kw={"width_ratios": [2.4, 1]}
    )
    y0, y1 = P.OUTER[2], P.OUTER[3]
    for ax in (a, b):
        rect(ax, y0, y1, 0, P.TOP, BASE)
        for yc in tab_y_centres():
            tooth(ax, yc, CUT, GAP)  # notch in the wall top
            tooth(ax, yc, NEW)  # the lid's tooth
            # the corner tab below, inside the wall (dashed)
            ax.add_patch(
                Rectangle(
                    (yc - P.TAB_LEN / 2, P.LID_UNDERSIDE - P.TAB_DROP),
                    P.TAB_LEN,
                    P.TAB_DROP,
                    fill=False,
                    ec="#555",
                    ls="--",
                    lw=0.8,
                )
            )
        ax.set_aspect("equal")
        ax.set_xlabel("board Y (mm)")
        ax.set_ylabel("case Z (mm)")
    a.set_xlim(y0 - 2, y1 + 2)
    a.set_ylim(-1, P.TOP + 2)
    a.set_title(
        "Left side wall from outside (cable end on the left); right wall mirrored",
        fontsize=10,
    )
    yc = tab_y_centres()[0]
    b.set_xlim(yc - 7, yc + 7)
    b.set_ylim(P.LID_UNDERSIDE - 5, P.TOP + 1.5)
    b.set_title("Close-up: tooth over the cable-end tab", fontsize=10)
    b.annotate(
        f"lid tooth {TOOTH_LEN:g} × {TOOTH_DEPTH:g} mm, full wall thickness,\nflush outside and on top; "
        f"{LEAD} mm 45° lead-in",
        (yc, P.TOP - 1),
        (yc - 6.5, P.TOP + 0.6),
        fontsize=7,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    b.annotate(
        f"notch {GAP} mm all round",
        (yc + TOOTH_LEN / 2 + 0.1, P.TOP - 1.2),
        (yc + 3.2, P.TOP - 3.6),
        fontsize=7,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    b.annotate(
        "corner snap tab (inside, unchanged)",
        (yc, P.LID_UNDERSIDE - 2),
        (yc - 6.5, P.LID_UNDERSIDE - 4.5),
        fontsize=7,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    fig.tight_layout()
    out.mkdir(parents=True, exist_ok=True)
    fig.savefig(out / "v7-teeth.png")
    fig.savefig(out / "v7-teeth.svg")


if __name__ == "__main__":
    main()
