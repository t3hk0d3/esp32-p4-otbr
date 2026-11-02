"""Throwaway sketch generator, v4: all four walls static (base), flat grille lid held by snap wings.

Run: python3 case/sketches/sketch_v4.py <out_dir>
"""

import math
import sys

from sketch_v3 import (
    BASE_C,
    FLOOR,
    GAP,
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

LID_C = "#a5b4fc"
FRAME = 5.5  # solid lid frame width
CELL = 3.5  # max hole size (across flats)
RIB = 1.0  # grille wall
SMA = (52.0, PCB1 + 9.0)
WINGS_Y = (10.0, 40.0)
WING_W = 7.0
WING_DROP = 7.0  # how far a wing hangs down the outside of the wall


def polygon(svg, pts, stroke, fill="white", sw=0.8):
    d = " ".join(f"{x:.1f},{y:.1f}" for x, y in pts)
    svg.el.append(
        f'<polygon points="{d}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"/>'
    )


def grille(v, x0, y0, x1, y1, kind):
    """Draws the holes of one lid pattern inside the rectangle (view mm)."""
    p = CELL + RIB
    if kind == "hex":
        r = CELL / math.sqrt(3)  # circumradius for the across-flats size
        dx, dy = p, p * math.sqrt(3) / 2
        row = 0
        y = y0 + r
        while y + r <= y1:
            x = x0 + CELL / 2 + (p / 2 if row % 2 else 0)
            while x + CELL / 2 <= x1:
                pts = [
                    v.p(
                        x + r * math.cos(math.radians(30 + 60 * k)),
                        y + r * math.sin(math.radians(30 + 60 * k)),
                    )
                    for k in range(6)
                ]
                polygon(v.s, pts, "#374151")
                x += dx
            y += dy
            row += 1
    elif kind == "slots":
        mid = (y0 + y1) / 2
        x = x0
        while x + CELL <= x1:
            v.rect(x, y0, x + CELL, mid - 1.0, "#374151", fill="white", sw=0.8, rx=2)
            v.rect(x, mid + 1.0, x + CELL, y1, "#374151", fill="white", sw=0.8, rx=2)
            x += p
    else:
        y = y0
        while y + CELL <= y1:
            x = x0
            while x + CELL <= x1:
                v.rect(
                    x, y, x + CELL, y + CELL, "#374151", fill="white", sw=0.8, rx=0.6
                )
                x += p
            y += p


def lid_plan(v, kind, show_board=True):
    v.rect(
        X0, Y0, X1, Y1, "#374151", fill=BASE_C, sw=2, rx=8
    )  # wall tops around the lid
    lx0, ly0, lx1, ly1 = (
        X0 + W / 2,
        Y0 + W / 2,
        X1 - W / 2,
        Y1 - W / 2,
    )  # lid sits in a rebate
    v.rect(lx0, ly0, lx1, ly1, "#374151", fill=LID_C, sw=1.5, rx=6)
    grille(v, lx0 + FRAME, ly0 + FRAME, lx1 - FRAME, ly1 - FRAME, kind)
    if kind != "slots":
        ym = 34.0
        v.rect(
            lx0 + FRAME, ym - 0.9, lx1 - FRAME, ym + 0.9, "#374151", fill=LID_C, sw=0.8
        )
    for y in WINGS_Y:
        for x, d in ((X0, -1), (X1, 1)):
            v.rect(
                x,
                y - WING_W / 2,
                x + d * 1.6,
                y + WING_W / 2,
                "#374151",
                fill=LID_C,
                sw=1.2,
            )
    if show_board:
        v.rect(0, 0, 55, 68, C["board"], dash="5,4", sw=1, rx=8)


def open_area(kind):
    p = CELL + RIB
    if kind == "slots":
        return CELL / p * 0.95
    return (CELL / p) ** 2


def sheet_views(path):
    svg = Svg(1180, 1140)
    svg.text(
        60, 32, "ESP32-P4 case — sketch v4 (outside views)", size=18, weight="bold"
    )
    for i, (c, t) in enumerate(
        (
            (BASE_C, "BASE (one part): back plate + all four walls + standoffs"),
            (LID_C, "LID: flat grille plate + 4 snap wings"),
        )
    ):
        svg.rect(60, 62 + i * 20 - 11, 22, 13, "#374151", fill=c, sw=1)
        svg.text(90, 62 + i * 20, t, size=12)

    title(svg, 60, 130, "A. Front (as seen on the wall) — honeycomb lid")
    v = View(svg, 120, 160, X0 - 6, Y1 + 4)
    lid_plan(v, "hex")
    v.rect(X1, SMA[0] - 3.5, X1 + 4, SMA[0] + 3.5, "#374151", fill="#fbbf24", sw=1.5)
    v.text(X1 + 5, SMA[0] - 1, "SMA", size=11, weight="bold")
    v.text(X1 + 3, WINGS_Y[0] - 1, "wing", size=10)
    v.text(X1 + 3, WINGS_Y[1] - 1, "wing", size=10)
    v.hdim(X0, X1, Y1 + 2.0, f"{X1 - X0:.0f}")
    v.vdim(X0 - 3.5, Y0, Y1, f"{Y1 - Y0:.0f}")
    v.text(27.5, Y0 - 4, "▼ cable end", size=10, anchor="middle", color=C["note"])

    def end_view(ox, oy, which):
        e = View(svg, ox, oy, X0 - 4, ZMAX + 3)
        e.rect(X0, 0, X1, ZMAX, "#374151", fill=BASE_C, sw=2)
        e.rect(X0 + W / 2, TOP, X1 - W / 2, ZMAX, "#374151", fill=LID_C, sw=1.2)
        for x, d in ((X0, -1), (X1, 1)):
            e.rect(
                x, ZMAX - WING_DROP, x + d * 1.6, ZMAX, "#374151", fill=LID_C, sw=1.2
            )
        e.rect(0, PCB0, 55, PCB1, C["board"], dash="3,3", sw=1)
        if which == "cable":
            e.rect(3.8, PCB1 - 1, 20.4, PCB1 + 17.0, C["cut"], fill="white", sw=2)
            e.text(12.1, PCB1 + 9, "USB-A", size=10, anchor="middle")
            e.text(12.1, PCB1 + 2, "USB-C", size=10, anchor="middle")
            e.rect(21.4, PCB1 - 1, 38.4, PCB1 + 14.3, C["cut"], fill="white", sw=2)
            e.text(29.9, PCB1 + 6, "RJ45 (PoE)", size=10, anchor="middle")
            e.rect(39.2, PCB1 - 0.6, 51.4, PCB1 + 2.0, C["cut"], fill="white", sw=2)
            e.text(45.3, PCB1 + 4.5, "µSD", size=10, anchor="middle")
            e.rect(41.5, PCB1 - 3.0, 49.0, PCB1 - 0.6, C["cut"], dash="2,2", sw=1)
        else:
            e.rect(0.5, TOP - 4.0, 54.5, TOP, C["cut"], fill="white", sw=2)
            e.text(
                27.5, TOP - 2.8, "40-pin cable slot 54 × 4", size=10, anchor="middle"
            )
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

    title(
        svg,
        600,
        130,
        "B. Cable end (bottom on the wall)",
        "board drops in tilted: connectors into the holes first",
    )
    end_view(640, 175, "cable")
    title(
        svg,
        600,
        345,
        "H. Header end (top on the wall)",
        "ribbon / jumper wires leave through the slot",
    )
    end_view(640, 390, "header")

    def side_view(ox, oy, which):
        s = View(svg, ox, oy, Y0 - 4, ZMAX + 3)
        s.rect(Y0, 0, Y1, ZMAX, "#374151", fill=BASE_C, sw=2)
        s.rect(0, PCB0, 68, PCB1, C["board"], dash="3,3", sw=1)
        for y in WINGS_Y:
            s.rect(
                y - WING_W / 2,
                ZMAX - WING_DROP,
                y + WING_W / 2,
                ZMAX,
                "#374151",
                fill=LID_C,
                sw=1.2,
            )
            s.rect(
                y - WING_W / 2 + 0.8,
                ZMAX - WING_DROP + 0.6,
                y + WING_W / 2 - 0.8,
                ZMAX - WING_DROP + 2.2,
                "#374151",
                fill="white",
                sw=0.8,
                dash="2,1",
            )
        s.rect(Y0, TOP, Y1, ZMAX, "#374151", fill=LID_C, sw=1, op=0.5)
        if which == "left":
            for yb, lab, dx, anc in (
                (5.65, "BOOT?", -2.0, "end"),
                (12.35, "RST?", 2.0, "start"),
            ):
                s.circ(yb, PCB1 + 1.65, 1.25, C["cut"], fill="#111", sw=1.5)
                s.text(yb + dx, PCB1 + 4.0, lab, size=10, anchor=anc)
            s.text(45, PCB1 + 5, "Ø2.5 pin-holes", size=10, anchor="middle")
        else:
            s.circ(SMA[0], SMA[1], 3.3, C["cut"], fill="white", sw=2)
            s.text(SMA[0], SMA[1] - 6.5, "SMA (D-hole)", size=10, anchor="middle")
        s.text(Y0, -3.5, "← cable end", size=10, color=C["note"])
        s.text(Y1, -3.5, "header end →", size=10, color=C["note"], anchor="end")

    title(
        svg,
        600,
        565,
        "C. Left side (static wall) — buttons",
        "blue tabs = lid wings; dashed = notch they hook into",
    )
    side_view(640, 610, "left")
    title(svg, 600, 815, "D. Right side (static wall) — SMA", "")
    side_view(640, 850, "right")

    title(
        svg,
        60,
        640,
        "G. Back plate (seen from behind)",
        "flat; two L-keyholes 32 mm apart",
    )
    b = View(svg, 120, 680, -X1 - 6, Y1 + 2)
    b.rect(-X1, Y0, -X0, Y1, "#374151", fill=BASE_C, sw=2, rx=8)
    for kx, ky in KEYS:
        u = -kx
        b.rect(u - 2.15, ky, u + 2.15, ky + 7, "#374151", fill="white", sw=1.5, rx=2)
        b.rect(u - 7, ky - 2.15, u, ky + 2.15, "#374151", fill="white", sw=1.5, rx=2)
        b.circ(u, ky, 4.0, "#374151", fill="white", sw=1.5)
        b.circ(u, ky + 7, 1.0, C["dim"], fill=C["dim"], sw=0)
        b.circ(u - 7, ky, 1.0, C["vent"], fill=C["vent"], sw=0)
    b.text(
        -27.5,
        28,
        "● hung cable end down (screws level)",
        size=10,
        anchor="middle",
        color=C["dim"],
    )
    b.text(
        -27.5,
        23,
        "● hung SMA side up (screws stacked)",
        size=10,
        anchor="middle",
        color=C["vent"],
    )
    svg.text(
        60,
        1120,
        "Sketch only — approximate (mm). Green dashed = board, blue dashed = parts inside, "
        "amber = openings.",
        size=11,
        color=C["note"],
    )
    svg.save(path)


def sheet_lid(path):
    svg = Svg(1180, 900)
    svg.text(
        60,
        32,
        "ESP32-P4 case — sketch v4 (lid options and how it attaches)",
        size=18,
        weight="bold",
    )
    svg.text(
        60,
        54,
        f"All three: 2 mm plate, {FRAME:.1f} mm solid frame, holes ≤ {CELL} mm (keeps fingers and "
        f"M3 screws out), {RIB:.0f} mm walls.",
        size=12,
        color=C["note"],
    )
    opts = (
        (
            "hex",
            "1. Honeycomb (recommended)",
            "stiffest for its open area; prints flat, no bridges",
        ),
        ("square", "2. Square grid", "same open area; a bit less stiff diagonally"),
        ("slots", "3. Slots + centre rib", "tidy look; slots flex along their length"),
    )
    for i, (kind, name, note) in enumerate(opts):
        ox = 60 + i * 370
        title(svg, ox, 100, name, note)
        v = View(svg, ox + 20, 140, X0 - 2, Y1 + 2)
        lid_plan(v, kind, show_board=False)
        svg.text(
            ox + 20,
            140 + (Y1 - Y0 + 6) * 5 + 10,
            f"open area ≈ {open_area(kind) * 100:.0f}% of the grille",
            size=12,
            weight="bold",
        )

    title(svg, 60, 590, "E. Section through a wing (across the case)")
    v = View(svg, 120, 620, X0 - 6, ZMAX + 3)
    v.rect(X0, 0, X1, FLOOR, "#374151", fill=BASE_C)
    v.rect(X0, FLOOR, X0 + W, ZMAX, "#374151", fill=BASE_C)
    v.rect(X1 - W, FLOOR, X1, ZMAX, "#374151", fill=BASE_C)
    v.rect(X0 + W / 2, TOP, X1 - W / 2, ZMAX, "#374151", fill=LID_C)
    for x, d in ((X0, -1), (X1, 1)):
        v.rect(x, ZMAX - WING_DROP, x + d * 1.6, ZMAX, "#374151", fill=LID_C)
        v.rect(
            x,
            ZMAX - WING_DROP + 0.6,
            x - d * 0.8,
            ZMAX - WING_DROP + 2.2,
            "#374151",
            fill=LID_C,
        )
    v.rect(0, PCB0, 55, PCB1, C["board"], fill=C["board"], op=0.5)
    v.rect(4.3, PCB1, 19.9, PCB1 + 16, C["part"], fill=C["part"], op=0.15, sw=1)
    v.text(12, PCB1 + 8, "USB stack", size=10, anchor="middle")
    for t, z in (
        ("lid sits in a 1 mm rebate on top of the walls (flush)", ZMAX + 0.5),
        (
            f"wing hangs {WING_DROP:.0f} mm down the outside; its hook clicks into a notch",
            ZMAX - 4,
        ),
        ("open: lift a wing with a fingernail", ZMAX - 8),
        ("0.5 mm between the lid and the USB stack", ZMAX - 12),
    ):
        v.text(X1 + 6, z, t, size=11)
    svg.text(
        60,
        880,
        f"Wing positions (Y = {WINGS_Y[0]:.0f} and {WINGS_Y[1]:.0f} mm, both sides) avoid the SMA "
        "on the right wall.",
        size=11,
        color=C["note"],
    )
    svg.save(path)


if __name__ == "__main__":
    out = sys.argv[1]
    sheet_views(f"{out}/v4-views.svg")
    sheet_lid(f"{out}/v4-lid.svg")
    _ = GAP
