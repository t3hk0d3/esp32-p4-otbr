"""Throwaway sketch, v8: SMA wall thinned by an inside recess; the knock-out becomes a break-off plug.

Run: PYTHONPATH=case python case/sketches/sketch_v8.py <out_dir>
"""

import math
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Polygon, Rectangle
from p4_case import params as P

BASE, NEW, CUT, EDGE = "#cfd3d9", "#f2a65a", "#ffffff", "#3a3f47"
HEX_AF = 8.2  # inside hex pocket: an 8 mm (5/16") SMA nut + 0.2, flat side up so its roof is a short bridge
RECESS_D = HEX_AF * 2 / math.sqrt(3)  # across corners
PANEL = 2.0  # wall left at the SMA hole
GAP = 0.4  # round the plug's top and sides
NECK = 0.8  # left at the break line along the flat
ZC, R = P.SMA_CASE_Z, P.SMA_D / 2
FLAT = ZC - (P.SMA_FLAT - R)  # case Z of the flat (bottom)
X_OUT, X_IN = P.OUTER[1], P.INNER[1]  # 59.5, 55.5


def d_shape(r, flat, n=64):
    """The D outline in YZ around (SMA_Y, ZC): circle of radius r cut flat at z = flat."""
    pts = []
    for k in range(n + 1):
        a = -math.pi / 2 + 2 * math.pi * k / n
        y, z = P.SMA_Y + r * math.cos(a), ZC + r * math.sin(a)
        pts.append((y, max(z, flat)))
    return pts


def section(ax):
    """Plane Y = SMA_Y, looking along -Y: the SMA wall, the recess and the plug."""
    ax.add_patch(Rectangle((X_IN, ZC - 8), X_OUT - X_IN, 16, fc=BASE, ec=EDGE, lw=0.7))
    # inside recess: Ø RECESS_D, leaves PANEL at the outer face
    ax.add_patch(
        Rectangle(
            (X_IN - 0.01, ZC - RECESS_D / 2),
            X_OUT - PANEL - X_IN,
            RECESS_D,
            fc=CUT,
            ec=EDGE,
            lw=0.7,
        )
    )
    # the hole through the panel: from the flat up to the top of the circle
    x0 = X_OUT - PANEL
    ax.add_patch(Rectangle((x0, FLAT), PANEL, ZC + R - FLAT, fc=CUT, ec=EDGE, lw=0.7))
    # the plug: fills the hole less GAP at the top, sits on the flat through a V-notched neck
    notch = (PANEL - NECK) / 2
    plug = [
        (x0, ZC + R - GAP),
        (X_OUT, ZC + R - GAP),
        (X_OUT, FLAT + notch),
        (X_OUT - notch, FLAT),
        (x0 + notch, FLAT),
        (x0, FLAT + notch),
    ]
    ax.add_patch(Polygon(plug, closed=True, fc=NEW, ec=EDGE, lw=0.7))
    ax.annotate(
        f"inside hex pocket {HEX_AF:g} mm across flats: {PANEL:g} mm panel left\n(the rest of the wall stays 4 mm)",
        (x0 - 0.8, ZC + 4.5),
        (X_IN - 9.5, ZC + 7.5),
        fontsize=7,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    ax.annotate(
        f"plug, {PANEL:g} mm thick, {GAP} mm gap at the top",
        (x0 + 1, ZC + 1),
        (X_OUT + 1.0, ZC + 5),
        fontsize=7,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    ax.annotate(
        f"V-notched neck along the flat:\n{NECK} mm, on a layer line: bend it to snap off",
        (X_OUT - 1.0, FLAT + 0.1),
        (X_OUT + 1.0, FLAT - 4.5),
        fontsize=7,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    ax.set_xlim(X_IN - 10, X_OUT + 10)
    ax.set_ylim(ZC - 8, ZC + 9)
    ax.set_aspect("equal")
    ax.set_title(
        "Section through the SMA axis (right wall; inside on the left)", fontsize=9
    )
    ax.set_xlabel("board X (mm)")
    ax.set_ylabel("case Z (mm)")


def outside(ax):
    """The SMA wall from outside: the D-hole, the gap round the plug's top and sides, the neck along the flat."""
    ax.add_patch(Rectangle((P.SMA_Y - 8, ZC - 8), 16, 16, fc=BASE, ec=EDGE, lw=0.7))
    hexagon = [
        (
            P.SMA_Y + RECESS_D / 2 * math.cos(k * math.pi / 3),
            ZC + RECESS_D / 2 * math.sin(k * math.pi / 3),
        )
        for k in range(6)
    ]
    ax.add_patch(Polygon(hexagon, closed=True, fill=False, ec="#555", ls="--", lw=0.9))
    ax.text(
        P.SMA_Y,
        ZC + HEX_AF / 2 + 0.4,
        f"hex pocket behind ({HEX_AF:g} AF, flat side up)",
        ha="center",
        fontsize=7,
    )
    ax.add_patch(Polygon(d_shape(R, FLAT), closed=True, fc=CUT, ec=EDGE, lw=0.7))
    ax.add_patch(Polygon(d_shape(R - GAP, FLAT), closed=True, fc=NEW, ec=EDGE, lw=0.7))
    chord = 2 * math.sqrt(R * R - (ZC - FLAT) ** 2)
    ax.plot(
        [P.SMA_Y - chord / 2, P.SMA_Y + chord / 2], [FLAT, FLAT], color="#c0392b", lw=2
    )
    ax.annotate(
        f"neck: the whole flat ({chord:.1f} mm)",
        (P.SMA_Y, FLAT),
        (P.SMA_Y - 7.5, ZC - 6.5),
        fontsize=7,
        arrowprops={"arrowstyle": "->", "lw": 0.6},
    )
    ax.set_xlim(P.SMA_Y - 8, P.SMA_Y + 8)
    ax.set_ylim(ZC - 8, ZC + 8)
    ax.set_aspect("equal")
    ax.set_title(
        f"From outside: Ø{P.SMA_D} D-hole, flat {P.SMA_FLAT} at the bottom", fontsize=9
    )
    ax.set_xlabel("board Y (mm)")


def main() -> None:
    out = Path(sys.argv[1])
    fig, (a, b) = plt.subplots(
        1, 2, figsize=(13, 5), dpi=120, gridspec_kw={"width_ratios": [1.5, 1]}
    )
    section(a)
    outside(b)
    fig.tight_layout()
    out.mkdir(parents=True, exist_ok=True)
    fig.savefig(out / "v8-sma.png")
    fig.savefig(out / "v8-sma.svg")


if __name__ == "__main__":
    main()
