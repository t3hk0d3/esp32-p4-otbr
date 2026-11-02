"""Throwaway sketch generator, v5: static four-wall base, honeycomb lid held by inside tabs.

Run: python3 case/sketches/sketch_v5.py <out_dir>
"""

import sys

from sketch_v3 import (
    BASE_C,
    FLOOR,
    KEYS,
    PCB0,
    PCB1,
    TOP,
    X0,
    X1,
    Y0,
    Y1,
    ZMAX,
    C,
    Svg,
    View,
    W,
    title,
)
from sketch_v4 import CELL, FRAME, LID_C, RIB, SMA, grille

TABS_Y = (3.0, 65.0)  # label positions
# corner tabs: (y_lo, y_hi, drop below lid underside, end-leg along the cable wall?)
CORNER_TABS = ((-0.5, 6.5, 6.5, True), (61.5, 68.5, 3.5, False))
TAB_W = 7.0
TAB_DROP = 6.5
TAB_ROOT = 2.0  # wedge-shaped tab: thickness at the lid
TAB_TIP = 1.4  # ... and at the tip; taper on the inner face
PRY_Y = ()  # pry cut-outs dropped: deferred to a later revision
PRY_W = 10.0
PRY_D = 3.0


def legend(svg):
    rows = (
        (BASE_C, "BASE (one part): back plate + all four walls + standoffs"),
        (LID_C, "LID: honeycomb grille plate, 4 corner tabs"),
    )
    for i, (c, t) in enumerate(rows):
        svg.rect(60, 62 + i * 20 - 11, 22, 13, "#374151", fill=c, sw=1)
        svg.text(90, 62 + i * 20, t, size=12)


def front(svg):
    title(
        svg,
        60,
        130,
        "A. Front (as seen on the wall)",
        "shaded = corner tabs under the lid (snaps + guide pins)",
    )
    v = View(svg, 120, 175, X0 - 6, Y1 + 4)
    v.rect(X0, Y0, X1, Y1, "#374151", fill=BASE_C, sw=2, rx=8)
    lx0, ly0, lx1, ly1 = X0 + W / 2, Y0 + W / 2, X1 - W / 2, Y1 - W / 2
    v.rect(lx0, ly0, lx1, ly1, "#374151", fill=LID_C, sw=1.5, rx=6)
    grille(v, lx0 + FRAME, ly0 + FRAME, lx1 - FRAME, ly1 - FRAME, "hex")
    v.rect(lx0 + FRAME, 33.1, lx1 - FRAME, 34.9, "#374151", fill=LID_C, sw=0.8)
    for ylo, yhi, _, end_leg in CORNER_TABS:
        for x, d in ((X0 + W, 1), (X1 - W, -1)):
            v.rect(x, ylo, x + d * TAB_ROOT, yhi, "#111", fill="#111", op=0.35, sw=1)
            if end_leg:
                v.rect(
                    x,
                    ylo,
                    x + d * 3.5,
                    ylo + TAB_ROOT,
                    "#111",
                    fill="#111",
                    op=0.35,
                    sw=1,
                )
    for y in PRY_Y:
        for x, d in ((X0, 1), (X1, -1)):
            v.rect(
                x,
                y - PRY_W / 2,
                x + d * (W / 2 + 1.2),
                y + PRY_W / 2,
                C["cut"],
                fill="white",
                sw=1.5,
                rx=2,
            )
    v.text(X1 + 2, TABS_Y[1] - 1, "corner tab (short)", size=10)
    v.text(X1 + 2, TABS_Y[0] - 1, "corner tab (L)", size=10)
    v.rect(X1, SMA[0] - 3.5, X1 + 4, SMA[0] + 3.5, "#374151", fill="#fbbf24", sw=1.5)
    v.text(X1 + 5, SMA[0] - 6, "SMA", size=11, weight="bold")
    v.hdim(X0, X1, Y1 + 2.0, f"{X1 - X0:.0f}")
    v.vdim(X0 - 3.5, Y0, Y1, f"{Y1 - Y0:.0f}")
    v.text(27.5, Y0 - 4, "▼ cable end", size=10, anchor="middle", color=C["note"])


def side(svg, ox, oy, which):
    s = View(svg, ox, oy, Y0 - 4, ZMAX + 3)
    s.rect(Y0, 0, Y1, ZMAX, "#374151", fill=BASE_C, sw=2)
    s.rect(Y0, TOP, Y1, ZMAX, "#374151", fill=LID_C, sw=1, op=0.6)
    for y in PRY_Y:
        s.rect(
            y - PRY_W / 2,
            ZMAX - PRY_D,
            y + PRY_W / 2,
            ZMAX,
            C["cut"],
            fill="white",
            sw=1.5,
            rx=2,
        )
    for ylo, yhi, drop, _ in CORNER_TABS:
        s.rect(ylo, TOP - drop, yhi, TOP, "#111", dash="2,1.5", sw=1)
    s.rect(0, PCB0, 68, PCB1, C["board"], dash="3,3", sw=1)
    if which == "left":
        for yb, lab, dx, anc in (
            (5.65, "BOOT?", -2.0, "end"),
            (12.35, "RST?", 2.0, "start"),
        ):
            s.circ(yb, PCB1 + 1.65, 1.25, C["cut"], fill="#111", sw=1.5)
            s.text(yb + dx, PCB1 + 4.0, lab, size=10, anchor=anc)
    else:
        s.circ(SMA[0], SMA[1], 3.3, C["cut"], fill="white", sw=2)
        s.text(SMA[0], SMA[1] - 6.5, "SMA", size=10, anchor="middle")
    s.text(Y0, -3.5, "← cable end", size=10, color=C["note"])
    s.text(Y1, -3.5, "header end →", size=10, color=C["note"], anchor="end")


def end_view(svg, ox, oy, which):
    e = View(svg, ox, oy, X0 - 4, ZMAX + 3)
    e.rect(X0, 0, X1, ZMAX, "#374151", fill=BASE_C, sw=2)
    e.rect(X0 + W / 2, TOP, X1 - W / 2, ZMAX, "#374151", fill=LID_C, sw=1.2)
    e.rect(0, PCB0, 55, PCB1, C["board"], dash="3,3", sw=1)
    if which == "cable":
        e.rect(3.8, PCB1 - 1, 20.4, PCB1 + 16.5, C["cut"], fill="white", sw=2)
        e.text(12.1, PCB1 + 9, "USB-A", size=10, anchor="middle")
        e.text(12.1, PCB1 + 2, "USB-C", size=10, anchor="middle")
        e.rect(21.4, PCB1 - 1, 38.4, PCB1 + 14.3, C["cut"], fill="white", sw=2)
        e.text(29.9, PCB1 + 6, "RJ45 (PoE)", size=10, anchor="middle")
        e.rect(39.2, PCB1 - 0.6, 51.4, PCB1 + 2.0, C["cut"], fill="white", sw=2)
        e.text(45.3, PCB1 + 4.5, "µSD", size=10, anchor="middle")
        e.rect(41.5, PCB1 - 3.0, 49.0, PCB1 - 0.6, C["cut"], dash="2,2", sw=1)
    else:
        e.rect(0.5, TOP - 4.0, 54.5, TOP, C["cut"], fill="white", sw=2)
        e.text(27.5, TOP - 2.8, "40-pin cable slot 54 × 3.5", size=10, anchor="middle")
        e.rect(2.1, PCB1, 52.9, PCB1 + 8.3, C["part"], dash="3,3", sw=1)
        e.text(
            27.5,
            PCB1 + 4,
            "header behind this wall",
            size=10,
            anchor="middle",
            color=C["part"],
        )
    e.vdim(X0 - 2, 0, ZMAX, f"{ZMAX:.0f}")


def sheet_views(path):
    svg = Svg(1180, 1060)
    svg.text(
        60,
        32,
        "ESP32-P4 case — sketch v5 (honeycomb lid on inside tabs)",
        size=18,
        weight="bold",
    )
    legend(svg)
    front(svg)
    title(svg, 600, 130, "C. Left side — buttons", "dashed = lid tabs inside the wall")
    side(svg, 640, 175, "left")
    title(svg, 600, 360, "D. Right side — SMA", "")
    side(svg, 640, 395, "right")

    title(
        svg,
        600,
        590,
        "G. Back plate (seen from behind)",
        "flat; two L-keyholes 32 mm apart",
    )
    b = View(svg, 640, 620, -X1 - 6, Y1 + 2)
    b.rect(-X1, Y0, -X0, Y1, "#374151", fill=BASE_C, sw=2, rx=8)
    for kx, ky in KEYS:
        u = -kx
        b.rect(u - 2.15, ky, u + 2.15, ky + 7, "#374151", fill="white", sw=1.5, rx=2)
        b.rect(u - 7, ky - 2.15, u, ky + 2.15, "#374151", fill="white", sw=1.5, rx=2)
        b.circ(u, ky, 4.0, "#374151", fill="white", sw=1.5)
    title(
        svg,
        60,
        610,
        "B. Cable end (bottom on the wall)",
        "board drops in tilted: connectors into the holes first",
    )
    end_view(svg, 100, 650, "cable")
    title(
        svg,
        60,
        825,
        "H. Header end (top on the wall)",
        "ribbon / jumper wires leave through the slot",
    )
    end_view(svg, 100, 865, "header")
    svg.text(
        60,
        1045,
        "Sketch only — approximate (mm). Amber = openings, blue dashed = parts inside, "
        "green dashed = board.",
        size=11,
        color=C["note"],
    )
    svg.save(path)


def sheet_detail(path):
    svg = Svg(1180, 400)
    svg.text(
        60,
        32,
        "ESP32-P4 case — sketch v5 (how the lid holds and comes off)",
        size=18,
        weight="bold",
    )
    svg.text(
        60,
        54,
        f"Honeycomb grille: holes ≤ {CELL} mm, {RIB:.0f} mm walls, {FRAME:.1f} mm solid frame "
        "(the tabs hang from the frame).",
        size=12,
        color=C["note"],
    )

    title(svg, 60, 100, "E. Section across the case through the cable-end corner tabs")
    v = View(svg, 100, 130, X0 - 4, ZMAX + 3)
    v.rect(X0, 0, X1, FLOOR, "#374151", fill=BASE_C)
    v.rect(X0, FLOOR, X0 + W, ZMAX, "#374151", fill=BASE_C)
    v.rect(X1 - W, FLOOR, X1, ZMAX, "#374151", fill=BASE_C)
    v.rect(X0 + W / 2, TOP, X1 - W / 2, ZMAX, "#374151", fill=LID_C)
    for x, d in ((X0 + W, 1), (X1 - W, -1)):
        pts = [
            v.p(x, TOP),
            v.p(x + d * TAB_ROOT, TOP),
            v.p(x + d * TAB_TIP, TOP - TAB_DROP + 0.6),
            v.p(x + d * (TAB_TIP - 0.6), TOP - TAB_DROP),
            v.p(x, TOP - TAB_DROP),
        ]
        svg.el.append(
            '<polygon points="'
            + " ".join(f"{a:.1f},{b:.1f}" for a, b in pts)
            + f'" fill="{LID_C}" stroke="#374151" stroke-width="1"/>'
        )
    v.rect(0, PCB0, 55, PCB1, C["board"], fill=C["board"], op=0.5)
    v.rect(4.3, PCB1, 19.9, PCB1 + 16, C["part"], fill=C["part"], op=0.15, sw=1)
    v.text(12, PCB1 + 8, "USB", size=10, anchor="middle")
    v.rect(47, PCB1, 54, PCB1 + 7, C["part"], fill=C["part"], op=0.15, sw=1)
    v.text(50.5, PCB1 + 3, "caps", size=9, anchor="middle")
    v.text(
        36, PCB1 + 12, "tab tip ≈ 10.5 mm above the board:", size=10, anchor="middle"
    )
    v.text(
        36, PCB1 + 9.8, "clears buttons (3.3) and caps (7)", size=10, anchor="middle"
    )

    title(svg, 520, 100, "F. Tab close-up (in the wall's inner face)")
    z = View(
        svg, 560, 140, -6, 8
    )  # 5x magnified detail: u = X (mm), v = Z relative to lid top
    k = 3.0

    def r(ua, va, ub, vb, *a, **kw):
        z.rect(ua * k, va * k, ub * k, vb * k, *a, **kw)

    r(-6, -9, -4, 0.0, "#374151", fill=BASE_C)  # wall (2 mm)
    r(-4, -1, 4, 0.0, "#374151", fill=LID_C)  # lid frame (lower 1 mm in rebate)
    r(-5, 0, 4, 1.0, "#374151", fill=LID_C)
    tab = [
        (-4, -1),
        (-4 + TAB_ROOT, -1),
        (-4 + TAB_TIP, -6.9),
        (-4 + TAB_TIP - 0.6, -7.5),
        (-4, -7.5),
    ]
    svg.el.append(
        '<polygon points="'
        + " ".join(f"{a:.1f},{b:.1f}" for a, b in (z.p(u * k, w * k) for u, w in tab))
        + f'" fill="{LID_C}" stroke="#374151" stroke-width="1"/>'
    )
    z.text(
        -1.5 * k,
        -3.6 * k,
        f"wedge: {TAB_ROOT} mm at the root, {TAB_TIP} at the tip",
        size=11,
    )
    pts = [z.p(-4 * k, -6.4 * k), z.p(-4.6 * k, -5.8 * k), z.p(-4 * k, -5.2 * k)]
    svg.el.append(
        '<polygon points="'
        + " ".join(f"{a:.1f},{b:.1f}" for a, b in pts)
        + f'" fill="{LID_C}" stroke="#374151" stroke-width="1"/>'
    )
    z.text(-1.5 * k, -5.6 * k, "ramped bump (both sides 45°):", size=11)
    z.text(-1.5 * k, -6.6 * k, "clicks in, pulls out", size=11)
    z.text(-1.5 * k, -1.8 * k, "lid frame sits in a 1 mm rebate", size=11)
    z.text(-7.5 * k, -9.8 * k, "wall", size=11)

    svg.text(
        60,
        340,
        "Opening: lever the lid up through the grille with a small flat screwdriver; pry cut-outs "
        "are left for a later revision.",
        size=12,
    )
    svg.text(
        60,
        362,
        "Board, cable end, header end, keyholes and the SMA are as in v4.",
        size=12,
        color=C["note"],
    )
    svg.save(path)


if __name__ == "__main__":
    out = sys.argv[1]
    sheet_views(f"{out}/v5-views.svg")
    sheet_detail(f"{out}/v5-detail.svg")
