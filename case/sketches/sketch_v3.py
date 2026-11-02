"""Throwaway sketch generator for the case design (v3: static end walls, snap-on lid).

Units are mm in the board frame: X 0..55 (button edge X=0, SMA side X=55), Y 0..68 (cable edge Y=0,
header edge Y=68), Z from the case's outer back face. Run: python sketch_v3.py <out_dir>
"""

import sys

S = 5.0
W = 2.0
GAP = 0.5
FLOOR = 2.0
STAND = 5.0
LIDT = 2.0
PCB0 = FLOOR + STAND
PCB1 = PCB0 + 1.6
TOP = PCB1 + 16.5
ZMAX = TOP + LIDT
X0, X1 = -GAP - W, 55 + GAP + W
Y0, Y1 = -GAP - W, 68 + GAP + W
BASE_C, LID_C = "#d1d5db", "#a5b4fc"
C = {
    "board": "#16a34a",
    "part": "#2563eb",
    "cut": "#d97706",
    "note": "#6b7280",
    "dim": "#db2777",
    "vent": "#0891b2",
    "rib": "#7c3aed",
}
KEYS = [(10, 42), (42, 42)]
SNAPS_Y = (Y0 + W + 2.0, Y1 - W - 2.0)
SMA = (50.0, PCB1 + 9.0)


class Svg:
    def __init__(self, w, h):
        self.w, self.h, self.el = w, h, []

    def rect(self, x, y, w, h, stroke, fill="none", dash=None, sw=1.5, rx=0, op=1):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        self.el.append(
            f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="{rx}" fill="{fill}" '
            f'fill-opacity="{op}" stroke="{stroke}" stroke-width="{sw}"{d}/>'
        )

    def circ(self, x, y, r, stroke, fill="none", sw=1.5, dash=None):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        self.el.append(
            f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{r:.1f}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"{d}/>'
        )

    def line(self, x1, y1, x2, y2, stroke, sw=1, dash=None, marker=False):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        m = ' marker-start="url(#a)" marker-end="url(#a)"' if marker else ""
        self.el.append(
            f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" stroke="{stroke}" stroke-width="{sw}"{d}{m}/>'
        )

    def text(
        self, x, y, t, size=12, color="#111", anchor="start", weight="normal", rot=0
    ):
        r = f' transform="rotate({rot} {x:.1f} {y:.1f})"' if rot else ""
        self.el.append(
            f'<text x="{x:.1f}" y="{y:.1f}" font-family="sans-serif" font-size="{size}" fill="{color}" '
            f'text-anchor="{anchor}" font-weight="{weight}"{r}>{t}</text>'
        )

    def save(self, path):
        head = (
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{self.h}" '
            f'viewBox="0 0 {self.w} {self.h}"><defs><marker id="a" viewBox="0 0 10 10" refX="5" refY="5" '
            f'markerWidth="6" markerHeight="6" orient="auto-start-reverse"><path d="M0,2 L5,5 L0,8" fill="none" '
            f'stroke="{C["dim"]}"/></marker></defs><rect width="100%" height="100%" fill="white"/>'
        )
        with open(path, "w") as f:
            f.write(head + "\n".join(self.el) + "</svg>")


class View:
    """Maps view mm (u right, v up) to sheet px; (u0, v1) lands at (ox, oy)."""

    def __init__(self, svg, ox, oy, u0, v1):
        self.s, self.ox, self.oy, self.u0, self.v1 = svg, ox, oy, u0, v1

    def p(self, u, v):
        return self.ox + (u - self.u0) * S, self.oy + (self.v1 - v) * S

    def rect(self, ua, va, ub, vb, *a, **k):
        x, y = self.p(min(ua, ub), max(va, vb))
        self.s.rect(x, y, abs(ub - ua) * S, abs(vb - va) * S, *a, **k)

    def circ(self, u, v, r, *a, **k):
        x, y = self.p(u, v)
        self.s.circ(x, y, r * S, *a, **k)

    def line(self, ua, va, ub, vb, *a, **k):
        x1, y1 = self.p(ua, va)
        x2, y2 = self.p(ub, vb)
        self.s.line(x1, y1, x2, y2, *a, **k)

    def text(self, u, v, t, **k):
        x, y = self.p(u, v)
        self.s.text(x, y, t, **k)

    def hdim(self, ua, ub, v, label):
        self.line(ua, v, ub, v, C["dim"], marker=True)
        self.text((ua + ub) / 2, v + 1, label, size=11, color=C["dim"], anchor="middle")

    def vdim(self, u, va, vb, label):
        self.line(u, va, u, vb, C["dim"], marker=True)
        x, y = self.p(u - 1, (va + vb) / 2)
        self.s.text(x, y, label, size=11, color=C["dim"], anchor="middle", rot=-90)


def title(svg, x, y, t, sub=""):
    svg.text(x, y, t, size=15, weight="bold")
    if sub:
        svg.text(x, y + 16, sub, size=11, color=C["note"])


def legend(svg, y):
    rows = (
        (
            BASE_C,
            "BASE (static): back plate + cable-end wall + header-end wall + standoffs",
        ),
        (LID_C, "LID (snaps on from the front): front plate + left and right walls"),
    )
    for i, (c, t) in enumerate(rows):
        svg.rect(60, y + i * 20 - 11, 22, 13, "#374151", fill=c, sw=1)
        svg.text(90, y + i * 20, t, size=12)


def front_view(svg):
    title(svg, 60, 130, "A. Front (as seen on the wall)")
    v = View(svg, 120, 160, X0 - 6, Y1 + 4)
    v.rect(X0, Y0, X1, Y1, "#374151", fill=LID_C, sw=2, rx=8)
    v.rect(0, 0, 55, 68, C["board"], dash="5,4", sw=1, rx=8)
    for i in range(6):
        u = 34.5 + i * 2.4
        v.rect(u, 28, u + 1.2, 44, C["vent"], fill="white", sw=1, rx=3)
    v.text(41, 45.5, "vents", size=10, anchor="middle", color=C["vent"])
    v.circ(1.0, 18.2, 1.0, C["cut"], fill="white", sw=1.5)
    v.text(2.8, 17.6, "LED", size=10)
    v.rect(2.1, 61.5, 52.9, 66.5, C["part"], dash="3,3", sw=1)
    v.text(
        27.5, 63.3, "40-pin header (inside)", size=10, anchor="middle", color=C["part"]
    )
    for box in (
        (4.26, -1.7, 19.93, 15.76),
        (21.86, -1.99, 37.9, 19.41),
        (39.6, 1.2, 51.0, 12.5),
    ):
        v.rect(*box, C["part"], dash="3,3", sw=1)
    v.text(12, 7, "USB", size=10, anchor="middle", color=C["part"])
    v.text(30, 9, "RJ45", size=10, anchor="middle", color=C["part"])
    v.text(45.3, 6, "µSD", size=10, anchor="middle", color=C["part"])
    v.circ(30.3, 26.6, 1.0, C["part"])
    v.line(30.3, 26.6, 55.5, SMA[0], C["part"], dash="4,3")
    v.text(13, 25, "u.FL → pigtail", size=10, color=C["part"])
    v.rect(X1, SMA[0] - 3.5, X1 + 4, SMA[0] + 3.5, "#374151", fill="#fbbf24", sw=1.5)
    v.text(X1 + 5, SMA[0] - 1, "SMA", size=11, weight="bold")
    for y in SNAPS_Y:
        for x in (X0, X1):
            v.rect(x - 0.8, y - 2.5, x + 0.8, y + 2.5, "#111", fill="#111")
    v.text(X1 + 2, SNAPS_Y[0] - 1, "snap", size=10)
    v.text(X1 + 2, SNAPS_Y[1] - 1, "snap", size=10)
    v.hdim(X0, X1, Y1 + 2.0, f"{X1 - X0:.0f}")
    v.vdim(X0 - 3.5, Y0, Y1, f"{Y1 - Y0:.0f}")
    v.text(27.5, Y0 - 4, "▼ cable end", size=10, anchor="middle", color=C["note"])


def end_view(svg, ox, oy, which):
    v = View(svg, ox, oy, X0 - 4, ZMAX + 3)
    v.rect(X0, FLOOR, X1, ZMAX, "#374151", fill=LID_C, sw=2)
    v.rect(X0, 0, X1, FLOOR, "#374151", fill=BASE_C, sw=1)
    v.rect(X0 + W, FLOOR, X1 - W, TOP, "#374151", fill=BASE_C, sw=1.5)
    v.rect(0, PCB0, 55, PCB1, C["board"], dash="3,3", sw=1)
    if which == "cable":
        v.rect(3.8, PCB1 - 1, 20.4, PCB1 + 17.0, C["cut"], fill="white", sw=2)
        v.text(12.1, PCB1 + 9, "USB-A", size=10, anchor="middle")
        v.text(12.1, PCB1 + 2, "USB-C", size=10, anchor="middle")
        v.rect(21.4, PCB1 - 1, 38.4, PCB1 + 14.3, C["cut"], fill="white", sw=2)
        v.text(29.9, PCB1 + 6, "RJ45 (PoE)", size=10, anchor="middle")
        v.rect(39.2, PCB1 - 0.6, 51.4, PCB1 + 2.0, C["cut"], fill="white", sw=2)
        v.text(45.3, PCB1 + 4.5, "µSD", size=10, anchor="middle")
        v.rect(41.5, PCB1 - 3.0, 49.0, PCB1 - 0.6, C["cut"], dash="2,2", sw=1)
        v.text(
            45.3, PCB0 - 4.2, "thumb scoop", size=9, anchor="middle", color=C["note"]
        )
    else:
        v.rect(0.5, TOP - 4.0, 54.5, TOP, C["cut"], fill="white", sw=2)
        v.text(27.5, TOP - 2.8, "40-pin cable slot 54 × 4", size=10, anchor="middle")
        v.rect(2.1, PCB1, 52.9, PCB1 + 8.3, C["part"], dash="3,3", sw=1)
        v.text(
            27.5,
            PCB1 + 4,
            "header behind this wall",
            size=10,
            anchor="middle",
            color=C["part"],
        )
    for x, d in ((X0 + W, 1), (X1 - W, -1)):
        v.rect(x, FLOOR + 6, x + d * 1.3, FLOOR + 8.4, "#111", fill="white", sw=1)
    v.vdim(X0 - 2, 0, ZMAX, f"{ZMAX:.0f}")


def side_view(svg, ox, oy, which):
    v = View(svg, ox, oy, Y0 - 4, ZMAX + 3)
    v.rect(Y0, 0, Y1, FLOOR, "#374151", fill=BASE_C, sw=1)
    v.rect(Y0, FLOOR, Y1, ZMAX, "#374151", fill=LID_C, sw=2)
    v.rect(0, PCB0, 68, PCB1, C["board"], dash="3,3", sw=1)
    for y in SNAPS_Y:
        v.rect(y - 2.5, FLOOR + 6, y + 2.5, FLOOR + 8.4, "#111", dash="2,2", sw=1)
    if which == "left":
        for yb, lab in ((5.65, "BOOT?"), (12.35, "RST?")):
            v.circ(yb, PCB1 + 1.65, 1.25, C["cut"], fill="#111", sw=1.5)
            v.text(yb, PCB1 + 5.5, lab, size=10, anchor="middle")
        v.text(
            40,
            PCB1 + 9,
            "Ø2.5 pin-holes: pencil / small screwdriver only",
            size=10,
            anchor="middle",
        )
    else:
        v.circ(SMA[0], SMA[1], 3.3, C["cut"], fill="white", sw=2)
        v.text(SMA[0], SMA[1] + 5.5, "SMA (D-hole)", size=10, anchor="middle")
    v.text(Y0, -3.5, "← cable end", size=10, color=C["note"])
    v.text(Y1, -3.5, "header end →", size=10, color=C["note"], anchor="end")


def back_view(svg):
    title(
        svg,
        60,
        640,
        "G. Back plate (seen from behind)",
        "flat; two L-keyholes 32 mm apart",
    )
    v = View(svg, 120, 680, -X1 - 6, Y1 + 2)
    v.rect(-X1, Y0, -X0, Y1, "#374151", fill=BASE_C, sw=2, rx=8)
    for kx, ky in KEYS:
        u = -kx
        v.rect(u - 2.15, ky, u + 2.15, ky + 7, "#374151", fill="white", sw=1.5, rx=2)
        v.rect(u - 7, ky - 2.15, u, ky + 2.15, "#374151", fill="white", sw=1.5, rx=2)
        v.circ(u, ky, 4.0, "#374151", fill="white", sw=1.5)
        v.circ(u, ky + 7, 1.0, C["dim"], fill=C["dim"], sw=0)
        v.circ(u - 7, ky, 1.0, C["vent"], fill=C["vent"], sw=0)
    v.text(
        -27.5,
        28,
        "● hung cable end down (screws level)",
        size=10,
        anchor="middle",
        color=C["dim"],
    )
    v.text(
        -27.5,
        23,
        "● hung SMA side up (screws stacked)",
        size=10,
        anchor="middle",
        color=C["vent"],
    )


def sheet_views(path):
    svg = Svg(1180, 1120)
    svg.text(
        60, 32, "ESP32-P4 case — sketch v3 (outside views)", size=18, weight="bold"
    )
    legend(svg, 62)
    front_view(svg)
    title(
        svg,
        600,
        130,
        "B. Cable end (static wall; bottom on the wall)",
        "board tilts in: connectors first",
    )
    end_view(svg, 640, 175, "cable")
    title(
        svg,
        600,
        345,
        "H. Header end (static wall; top on the wall)",
        "ribbon / jumper wires leave through the slot",
    )
    end_view(svg, 640, 390, "header")
    title(
        svg, 600, 565, "C. Left side (lid wall) — buttons", "dashed = snap hooks inside"
    )
    side_view(svg, 640, 610, "left")
    title(svg, 600, 815, "D. Right side (lid wall) — SMA", "")
    side_view(svg, 640, 850, "right")
    back_view(svg)
    svg.text(
        60,
        1100,
        "Sketch only — approximate (mm). Green dashed = board, blue dashed = parts inside, "
        "amber = openings.",
        size=11,
        color=C["note"],
    )
    svg.save(path)


def sheet_sections(path):
    s = Svg(1180, 900)
    s.text(
        60,
        32,
        "ESP32-P4 case — sketch v3 (sections and assembly)",
        size=18,
        weight="bold",
    )
    legend(s, 62)
    title(s, 60, 130, "E. Long section (cable end left, header end right)")
    v = View(s, 140, 160, Y0 - 6, ZMAX + 4)
    v.rect(Y0, 0, Y1, FLOOR, "#374151", fill=BASE_C)
    v.rect(Y0, FLOOR, Y0 + W, TOP, "#374151", fill=BASE_C)
    v.rect(Y1 - W, FLOOR, Y1, TOP - 4, "#374151", fill=BASE_C)
    v.rect(Y0, TOP, Y1, ZMAX, "#374151", fill=LID_C)
    for y in (24.3, 57.3):
        v.rect(y - 3, FLOOR, y + 3, PCB0, "#374151", fill=BASE_C)
    v.rect(0, PCB0, 68, PCB1, C["board"], fill=C["board"], op=0.5)
    v.rect(-1.7, PCB1, 15.8, PCB1 + 16, C["part"], fill=C["part"], op=0.15, sw=1)
    v.text(7, PCB1 + 8, "USB", size=10, anchor="middle")
    v.rect(27.6, PCB1, 44.4, PCB1 + 12, C["part"], fill=C["part"], op=0.15, sw=1)
    v.text(36, PCB1 + 6, "PoE xfmr", size=10, anchor="middle")
    v.rect(61.5, PCB1, 66.5, PCB1 + 8.3, C["part"], fill=C["part"], op=0.25, sw=1)
    v.rect(
        59.5,
        PCB1 + 2.5,
        68.0,
        PCB1 + 11.5,
        C["rib"],
        fill=C["rib"],
        op=0.15,
        sw=1,
        dash="3,2",
    )
    v.text(58.5, PCB1 + 7, "IDC plug", size=10, anchor="end", color=C["rib"])
    v.line(64, PCB1 + 11.5, 64, TOP - 2, C["rib"], sw=2, dash="4,2")
    v.line(64, TOP - 2, Y1 + 8, TOP - 2, C["rib"], sw=2, dash="4,2")
    v.text(Y1 + 2, TOP + 0.5, "ribbon out", size=10, color=C["rib"])
    v.text(Y1 + 2, FLOOR + 6, "header-end wall", size=10)
    v.text(Y0 - 2, FLOOR + 14, "cable wall", size=10, anchor="end")
    v.vdim(Y0 - 4, 0, ZMAX, f"{ZMAX:.0f}")
    v.text(
        34,
        TOP - 3,
        "lid inner face 16.5 above the board",
        size=10,
        anchor="middle",
        color=C["note"],
    )

    title(s, 60, 400, "F. Exploded cross-section at an end wall — how the lid snaps on")
    f = View(s, 140, 430, X0 - 6, ZMAX + 34)
    off = 30
    f.rect(X0, TOP + off, X1, ZMAX + off, "#374151", fill=LID_C)
    f.rect(X0, FLOOR + off, X0 + W, ZMAX + off, "#374151", fill=LID_C)
    f.rect(X1 - W, FLOOR + off, X1, ZMAX + off, "#374151", fill=LID_C)
    for x, d in ((X0 + W, 1), (X1 - W, -1)):
        f.rect(
            x, FLOOR + off + 6.2, x + d * 1.2, FLOOR + off + 8.2, "#111", fill="#111"
        )
    f.line(27.5, ZMAX + 1, 27.5, ZMAX + off - 3, C["dim"], marker=True)
    f.rect(X0, 0, X1, FLOOR, "#374151", fill=BASE_C)
    f.rect(X0 + W, FLOOR, X1 - W, TOP, "#374151", fill=BASE_C, op=0.7)
    for x, d in ((X0 + W, 1), (X1 - W, -1)):
        f.rect(x, FLOOR + 6, x + d * 1.3, FLOOR + 8.4, "#374151", fill="white")
    f.text(X1 + 3, TOP + off, "lid: front plate + side walls", size=11)
    f.text(
        X1 + 3,
        FLOOR + off + 7,
        "hook inside each side wall, at both ends (4 total)",
        size=11,
    )
    f.text(X1 + 3, TOP - 3, "end wall (static, part of the base)", size=11)
    f.text(
        X1 + 3, FLOOR + 7, "notch in the end wall's side face catches the hook", size=11
    )
    f.text(
        X1 + 3,
        0,
        "release: lever the side wall out at a small slot",
        size=11,
        color=C["note"],
    )

    title(s, 60, 760, "Assembly")
    steps = (
        "1. SMA bulkhead into the right lid wall; pigtail (8 cm) onto the board's u.FL.",
        (
            "2. Board in, tilted: USB/RJ45 into the cable-wall holes first, then lower onto the standoffs; 4× M2.5 "
            "self-tapping."
        ),
        "3. Optional: ribbon / jumpers onto the 40-pin header, out through the header-end slot.",
        "4. Lid on from the front until the four corner snaps click.",
    )
    for i, t in enumerate(steps):
        s.text(60, 790 + i * 22, t, size=12)
    s.save(path)


if __name__ == "__main__":
    out = sys.argv[1]
    sheet_views(f"{out}/v3-views.svg")
    sheet_sections(f"{out}/v3-sections.svg")
