"""Writes the documentation diagrams (board, install paths, radios, RCP proxy, split mode, routing, TREL) as SVG.

Usage: python3 tools/docs_diagrams.py docs/images
"""

import argparse
import os

_parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
_parser.add_argument("out", help="output directory, e.g. docs/images")
OUT = _parser.parse_args().out
os.makedirs(OUT, exist_ok=True)

STYLE = """  <style>
    :root { --bg: #eef2f4; --box: #ffffff; --line: #c3ced6; --ink: #17222c; --quiet: #5d6b77;
            --air: #12968f; --zig: #a8850f; --wire: #b8732e; --pcb: #0e3b43; --silk: #e9e4d4; }
    @media (prefers-color-scheme: dark) {
      :root { --bg: #0f1a24; --box: #16242f; --line: #2d3d4a; --ink: #e7ecef; --quiet: #91a0ab;
              --air: #3cc7bb; --zig: #e6c84f; --wire: #d9944a; --pcb: #0b2c33; --silk: #e9e4d4; }
    }
    text { font-family: -apple-system, "Segoe UI", "Helvetica Neue", Arial, sans-serif; fill: var(--ink); }
    .t1 { font-size: 16px; font-weight: 650; }
    .t2 { font-size: 13px; fill: var(--quiet); }
    .t3 { font-size: 12.5px; font-weight: 600; }
    .mono { font-family: ui-monospace, "SF Mono", Menlo, Consolas, monospace; font-size: 12px; }
    .silk { font-family: ui-monospace, "SF Mono", Menlo, Consolas, monospace; font-size: 11px; fill: var(--silk); }
    .box { fill: var(--box); stroke: var(--line); stroke-width: 1.5; }
    .box.b-air { stroke: var(--air); } .box.b-zig { stroke: var(--zig); } .box.b-wire { stroke: var(--wire); }
    .pcb { fill: var(--pcb); stroke: var(--air); stroke-width: 1.5; }
    .chip { fill: #c9ced2; stroke: #8e979f; stroke-width: 1; }
    .wire { fill: none; stroke: var(--wire); stroke-width: 3; stroke-linecap: round; }
    .wire.thin { stroke-width: 2; }
    .air { fill: none; stroke: var(--air); stroke-width: 1.8; stroke-dasharray: 5 6; stroke-linecap: round; }
    .airs { fill: none; stroke: var(--air); stroke-width: 1.8; stroke-linecap: round; }
    .zigl { fill: none; stroke: var(--zig); stroke-width: 1.8; stroke-dasharray: 5 6; stroke-linecap: round; }
    .lead { fill: none; stroke: var(--quiet); stroke-width: 1; }
    .arrow { fill: none; stroke: var(--ink); stroke-width: 1.6; marker-end: url(#ah); }
    .node { fill: var(--air); } .end { fill: var(--box); stroke: var(--air); stroke-width: 2; }
    .ca { fill: var(--air); } .cz { fill: var(--zig); } .cw { fill: var(--wire); } .cq { fill: var(--quiet); }
  </style>
  <defs><marker id="ah" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">
    <path d="M0 0L10 5L0 10z" fill="var(--ink)"/></marker></defs>
"""


def svg(name, w, h, label, body):
    s = (
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" role="img"\n'
        f'     aria-label="{label}">\n'
        "  <!-- Generated for the documentation. Teal is radio (Thread), yellow Zigbee, copper wire. -->\n"
        + STYLE
        + f'  <rect width="{w}" height="{h}" rx="18" fill="var(--bg)"/>\n'
        + body
        + "</svg>\n"
    )
    with open(os.path.join(OUT, name), "w", encoding="utf-8") as f:
        f.write(s)


def box(x, y, w, h, t1, t2=None, cls="", t2b=None, anchor="start"):
    tx = x + 16 if anchor == "start" else x + w / 2
    out = f'  <rect class="box {("b-" + cls) if cls else ""}" x="{x}" y="{y}" width="{w}" height="{h}" rx="12"/>\n'
    ty = y + (h / 2 + 5 if t2 is None else h / 2 - 4)
    out += f'  <text class="t1" x="{tx}" y="{ty}" text-anchor="{anchor}">{t1}</text>\n'
    if t2:
        out += f'  <text class="t2" x="{tx}" y="{ty + 20}" text-anchor="{anchor}">{t2}</text>\n'
    if t2b:
        out += f'  <text class="t2" x="{tx}" y="{ty + 38}" text-anchor="{anchor}">{t2b}</text>\n'
    return out


def text(x, y, s, cls="t2", anchor="start"):
    return f'  <text class="{cls}" x="{x}" y="{y}" text-anchor="{anchor}">{s}</text>\n'


def mesh(cx, cy, scale=1.0):
    pts = [(-40, -30), (35, -38), (-10, 30), (45, 25)]
    ends = [(-75, -10, 0), (5, -70, 1), (75, -5, 3), (-45, 60, 2), (60, 60, 3)]
    o = ""
    P = [(cx + x * scale, cy + y * scale) for x, y in pts]
    for a, b in [(0, 1), (0, 2), (1, 3), (2, 3), (1, 2)]:
        o += f'  <line class="airs" x1="{P[a][0]}" y1="{P[a][1]}" x2="{P[b][0]}" y2="{P[b][1]}" opacity="0.7"/>\n'
    for x, y, p in ends:
        ex, ey = cx + x * scale, cy + y * scale
        o += f'  <line class="air" x1="{ex}" y1="{ey}" x2="{P[p][0]}" y2="{P[p][1]}" opacity="0.7"/>\n'
        o += f'  <circle class="end" cx="{ex}" cy="{ey}" r="5"/>\n'
    for x, y in P:
        o += f'  <circle class="node" cx="{x}" cy="{y}" r="8"/>\n'
    return o


def board(x, y, w=150, h=84, label="This board", sub=None):
    o = f'  <rect class="pcb" x="{x}" y="{y}" width="{w}" height="{h}" rx="12"/>\n'
    o += f'  <rect x="{x + 14}" y="{y - 34}" width="8" height="40" rx="4" fill="var(--quiet)"/>\n'
    o += f'  <text class="t1" x="{x + w / 2}" y="{y + h / 2 + (0 if sub else 5)}" text-anchor="middle" style="fill:#e7ecef">{label}</text>\n'
    if sub:
        o += f'  <text class="t2" x="{x + w / 2}" y="{y + h / 2 + 20}" text-anchor="middle" style="fill:#a9c3c6">{sub}</text>\n'
    return o


# 1. Board anatomy (top view, layout after the Waveshare photo)
b = ""
b += '  <rect class="pcb" x="200" y="40" width="400" height="460" rx="16"/>\n'
b += '  <rect x="214" y="54" width="372" height="40" rx="4" fill="#0a2c32" stroke="var(--silk)" stroke-opacity="0.3"/>\n'
for i in range(20):
    for r in range(2):
        b += f'  <rect x="{222 + i * 18}" y="{60 + r * 16}" width="10" height="10" fill="#c9a46a"/>\n'
b += '  <rect class="chip" x="300" y="120" width="150" height="170" rx="6"/>\n'
b += '  <text class="t3" x="375" y="205" text-anchor="middle" style="fill:#17222c">ESP32-P4</text>\n'
b += '  <rect class="chip" x="320" y="305" width="100" height="95" rx="5"/>\n'
b += '  <text class="t3" x="370" y="350" text-anchor="middle" style="fill:#17222c">ESP32-C6</text>\n'
b += '  <circle cx="412" cy="392" r="6" fill="#c9a46a" stroke="#8e979f"/>\n'
b += '  <rect x="230" y="120" width="22" height="160" rx="3" fill="#0a2c32" stroke="var(--silk)" stroke-opacity="0.3"/>\n'
b += '  <rect x="262" y="120" width="22" height="160" rx="3" fill="#0a2c32" stroke="var(--silk)" stroke-opacity="0.3"/>\n'
b += '  <rect x="214" y="400" width="22" height="22" rx="4" fill="#2b3742"/><rect x="214" y="440" width="22" height="22" rx="4" fill="#2b3742"/>\n'
b += '  <text class="silk" x="214" y="434">RST</text><text class="silk" x="214" y="474">BOOT</text>\n'
b += '  <rect x="250" y="410" width="86" height="100" rx="4" fill="#b8bfc5" stroke="#7d868d"/>\n'
b += '  <text class="t3" x="293" y="455" text-anchor="middle" style="fill:#17222c">USB-A</text><text class="t3" x="293" y="473" text-anchor="middle" style="fill:#17222c">USB-C</text>\n'
b += '  <rect x="350" y="420" width="110" height="90" rx="4" fill="#b8bfc5" stroke="#7d868d"/>\n'
b += '  <text class="t3" x="405" y="470" text-anchor="middle" style="fill:#17222c">RJ45</text>\n'
b += '  <rect x="480" y="420" width="80" height="70" rx="4" fill="#b8bfc5" stroke="#7d868d"/>\n'
b += '  <text class="t3" x="520" y="460" text-anchor="middle" style="fill:#17222c">TF card</text>\n'
for i in range(4):
    b += f'  <circle cx="578" cy="{428 + i * 18}" r="6" fill="#c9a46a" stroke="#0a2c32" stroke-width="2"/>\n'
b += '  <text class="silk" x="560" y="418">H7</text>\n'
# labels
L = [
    (214, 74, 190, 74, "end", "40-pin header", "maintenance jumper: pins 12, 16, 18"),
    (
        412,
        392,
        640,
        300,
        "start",
        "C6 antenna connector (IPEX)",
        "fit a 2.4 GHz antenna here",
    ),
    (370, 340, 640, 230, "start", "ESP32-C6: the built-in radio", "Thread or Zigbee"),
    (
        375,
        160,
        640,
        140,
        "start",
        "ESP32-P4: runs the firmware",
        "border router, web UI",
    ),
    (
        584,
        455,
        640,
        430,
        "start",
        "H7: C6 UART pads",
        "TXD, RXD, GND, IO9 (pin 1 at top)",
    ),
    (405, 510, 640, 520, "start", "Ethernet (RJ45), PoE", "network and power"),
    (
        293,
        510,
        190,
        545,
        "end",
        "USB-A above USB-C",
        "A: USB radio dongle · C: flashing, console",
    ),
    (
        214,
        450,
        190,
        420,
        "end",
        "RESET and BOOT",
        "hold BOOT, press RESET: download mode",
    ),
]
for px, py, lx, ly, anc, t1, t2 in L:
    b += f'  <path class="lead" d="M{px} {py} L{lx + (6 if anc == "end" else -6)} {ly - 5}"/>\n'
    b += f'  <circle cx="{px}" cy="{py}" r="3" fill="var(--quiet)"/>\n'
    b += text(lx, ly, t1, "t3", anc) + text(lx, ly + 17, t2, "t2", anc)
b = '  <g transform="translate(90 0)">\n' + b + "  </g>\n"
svg(
    "board.svg",
    1050,
    580,
    "Top view of the Waveshare ESP32-P4-WIFI6-POE-ETH: the 40-pin header along the top edge, the ESP32-P4 module, the ESP32-C6 module with its IPEX antenna connector, and along the bottom edge RESET and BOOT buttons, the stacked USB-A and USB-C ports, the RJ45 Ethernet port, the TF card slot and the four H7 pads",
    b,
)

# 2. Install paths
b = ""
b += box(30, 40, 230, 70, "1. Flash the P4", "browser flasher, over USB-C", "air")
b += '  <path class="arrow" d="M260 75 H300"/>\n'
b += box(
    300, 40, 260, 70, "Open the board's web page", "the Dashboard says what's missing"
)
b += '  <path class="arrow" d="M430 110 V150"/>\n'
b += text(430, 172, "2. Put the radio firmware on the C6: pick one way", "t3", "middle")
cols = [
    (
        40,
        "A: no wires",
        "most new boards",
        "upload c6_installer.bin",
        "on the Firmware page, wait ~1 min",
    ),
    (
        330,
        "B: maintenance jumper",
        "three wires fitted",
        "the board rewrites",
        "the C6 by itself at boot",
    ),
    (
        620,
        "C: USB-UART adapter",
        "if A and B failed",
        "esptool writes the",
        "C6 base image over H7",
    ),
]
for x, t1, t2, s1, s2 in cols:
    b += f'  <path class="arrow" d="M430 182 C 430 200, {x + 125} 190, {x + 125} 210"/>\n'
    b += f'  <rect class="box" x="{x}" y="210" width="250" height="110" rx="12"/>\n'
    b += text(x + 16, 238, t1, "t1") + text(x + 16, 258, t2, "t3 cq")
    b += text(x + 16, 286, s1, "t2") + text(x + 16, 303, s2, "t2")
    b += f'  <path class="arrow" d="M{x + 125} 320 C {x + 125} 345, 430 340, 430 360"/>\n'
b += box(
    290, 360, 280, 70, "Upload c6_rcp.bin", "the radio firmware, a few seconds", "air"
)
b += '  <path class="arrow" d="M430 430 V460"/>\n'
b += box(
    250,
    460,
    360,
    70,
    "3. Give the board a Thread network",
    "Home Assistant, or the Network page",
    "air",
)
svg(
    "install-paths.svg",
    900,
    560,
    "First install: flash the P4 over USB-C, open the web page, then put the radio firmware on the C6 one of three ways (A: upload c6_installer.bin with no wires, B: the maintenance jumper rewrites the C6 by itself, C: a USB-UART adapter), upload c6_rcp.bin, and give the board a Thread network",
    b,
)

# 3. Radios
b = ""
b += box(30, 70, 250, 84, "ESP32-C6", "built in, on SPI", "air")
b += box(30, 226, 250, 84, "USB radio dongle", "firmware detected at plug-in", "zig")
b += '  <line class="air" x1="280" y1="112" x2="410" y2="160"/>\n  <line class="zigl" x1="280" y1="268" x2="410" y2="220"/>\n'
b += '  <rect class="pcb" x="410" y="90" width="260" height="200" rx="22"/>\n'
b += '  <text class="t1" x="540" y="148" text-anchor="middle" style="fill:#e7ecef;font-size:19px">ESP32-P4</text>\n'
b += '  <text class="t2" x="540" y="172" text-anchor="middle" style="fill:#a9c3c6">a mode per radio</text>\n'
for x, w, c, s in [
    (432, 72, "air", "Thread"),
    (510, 72, "zig", "Zigbee"),
    (588, 62, "wire", "Proxy"),
]:
    b += f'  <rect x="{x}" y="196" width="{w}" height="28" rx="14" fill="var(--pcb)" stroke="var(--{c})"/>\n'
    b += f'  <text class="t3 c{c[0]}" x="{x + w / 2}" y="215" text-anchor="middle">{s}</text>\n'
b += '  <text class="t2" x="540" y="256" text-anchor="middle" style="fill:#a9c3c6">or off</text>\n'
for y in (70, 190, 310):
    b += f'  <path class="wire" d="M670 190 C 740 190, 740 {y}, 800 {y}"/>\n'
b += box(
    800, 40, 290, 62, "Home Assistant", "OpenThread Border Router integration", "air"
)
b += box(
    800, 159, 290, 62, "Matter controllers", "reach Thread devices through it", "air"
)
b += box(
    800,
    279,
    290,
    62,
    "ZHA or Zigbee2MQTT",
    "ZHA on board · Z2M through the proxy",
    "zig",
)
b += text(
    550,
    372,
    "Thread and the on-board Zigbee coordinator run side by side, one on each radio; a proxy runs beside either.",
    "t2",
    "middle",
)
svg(
    "radios.svg",
    1120,
    392,
    "The built-in ESP32-C6 and a USB radio dongle connect to the ESP32-P4, which runs each radio as Thread, Zigbee, an RCP proxy or off. Thread serves Home Assistant and Matter controllers; the on-board Zigbee coordinator serves ZHA, and Zigbee2MQTT reaches a dongle through the proxy. Thread and Zigbee run side by side, one on each radio; a proxy runs beside either.",
    b,
)

# 4. RCP proxy
b = ""
b += box(30, 80, 230, 84, "Radio", "the C6, or a USB dongle", "zig")
b += '  <line class="zigl" x1="260" y1="122" x2="330" y2="122"/>\n'
b += board(330, 80, 190, 84, "This board", "radio in RCP proxy mode")
b += '  <path class="wire" d="M520 122 H800"/>\n'
b += text(660, 104, "TCP or UDP over Ethernet", "t3", "middle")
b += text(660, 148, "Listen: it connects to board:6638", "t2", "middle")
b += text(660, 166, "Connect: the board dials it", "t2", "middle")
b += box(
    800,
    70,
    290,
    104,
    "Another computer",
    "Zigbee2MQTT or ZHA (Z-Stack, EZSP)",
    "wire",
    "otbr-agent (OpenThread RCP)",
)
b += text(
    560,
    225,
    "The computer drives the radio as if it were plugged into it. One computer at a time; no login.",
    "t2",
    "middle",
)
svg(
    "rcp-proxy.svg",
    1120,
    250,
    "RCP proxy: a radio on the board, the C6 or a USB dongle, is served over TCP or UDP on Ethernet to another computer running Zigbee2MQTT, ZHA or otbr-agent. The computer connects to the board on port 6638, or the board dials the computer.",
    b,
)

# 5. Split mode
b = ""
b += board(40, 150, 170, 90, "This board", "one cable, two VLANs")
b += '  <path class="wire" d="M210 195 H330"/>\n'
b += text(270, 180, "tagged", "t2", "middle")
b += box(330, 160, 150, 70, "Switch", "trunk port", "wire")
b += '  <path class="wire thin" d="M480 180 C 540 180, 540 95, 600 95"/>\n  <path class="wire thin" d="M480 210 C 540 210, 540 295, 600 295"/>\n'
b += '  <rect class="box" x="600" y="30" width="420" height="130" rx="14"/>\n'
b += text(616, 56, "Management VLAN (mgmt)", "t1") + text(
    616, 76, "web UI, /device API, metrics, firmware updates", "t2"
)
b += box(616, 92, 180, 52, "Your browser") + box(812, 92, 192, 52, "Prometheus")
b += '  <rect class="box b-air" x="600" y="220" width="420" height="160" rx="14"/>\n'
b += text(616, 246, "Thread backbone VLAN (thread_bb)", "t1") + text(
    616, 266, "Thread REST API, mDNS, TREL, routing", "t2"
)
b += box(616, 282, 180, 52, "Home Assistant") + box(
    812, 282, 192, 52, "Other border routers"
)
b += text(
    810, 362, "Home Assistant must be here: mDNS stays on this VLAN", "t2", "middle"
)
b += text(270, 290, "Nothing passes between", "t2", "middle") + text(
    270, 307, "the two VLANs on the board", "t2", "middle"
)
svg(
    "split-mode.svg",
    1060,
    400,
    "Split mode: the board on one tagged cable to a switch trunk port. The management VLAN carries the web UI, device API and metrics for your browser and Prometheus; the Thread backbone VLAN carries the Thread REST API, mDNS, TREL and routing for Home Assistant and other border routers. Nothing passes between the two on the board.",
    b,
)

# 6. Routing
b = ""
b += mesh(130, 170, 1.0)
b += text(130, 290, "Thread devices", "t3", "middle") + text(
    130, 308, "2001:db8:42:100::/64", "mono", "middle"
)
b += '  <line class="air" x1="210" y1="170" x2="280" y2="170"/>\n'
b += board(280, 128, 170, 84, "This board", "border router")
b += '  <path class="wire" d="M450 170 H640"/>\n'
b += (
    text(545, 150, "BGP or RIPng:", "t3", "middle")
    + text(545, 198, '"2001:db8:42:100::/64', "mono", "middle")
    + text(545, 214, 'is behind me"', "mono", "middle")
)
b += box(640, 130, 200, 80, "Your router", "learns the route", "wire")
b += '  <path class="wire thin" d="M840 170 H900"/>\n'
b += box(900, 130, 190, 80, "Other subnets", "VLANs, VPN, servers")
b += text(
    610,
    300,
    "With DHCPv6 prefix delegation the router already knows the prefix: nothing to announce.",
    "t2",
    "middle",
)
svg(
    "routing.svg",
    1110,
    330,
    "Routing to the mesh: Thread devices use the OMR prefix 2001:db8:42:100::/64. The board announces it to your router over BGP or RIPng, so hosts on other subnets reach the Thread devices through the board. With DHCPv6 prefix delegation the router knows the prefix already.",
    b,
)

# 7. TREL
b = ""
b += mesh(150, 150, 0.9) + mesh(810, 150, 0.9)
b += text(40, 262, "Upstairs devices", "t3") + text(
    920, 262, "Downstairs devices", "t3", "end"
)
b += '  <line class="air" x1="178" y1="234" x2="190" y2="184"/>\n  <line class="air" x1="668" y1="234" x2="800" y2="184"/>\n'
b += board(160, 270, 150, 74, "Board 1") + board(650, 270, 150, 74, "Board 2")
b += '  <path class="wire" d="M310 320 H420"/>\n  <path class="wire" d="M650 320 H540"/>\n'
b += box(420, 290, 120, 60, "Switch", None, "wire", anchor="middle")
b += '  <path d="M235 344 C 300 420, 660 420, 725 344" fill="none" stroke="#8a5cd0" stroke-width="2" stroke-dasharray="7 6"/>\n'
b += text(480, 425, "TREL: Thread links over Ethernet", "t3", "middle")
b += text(
    480,
    444,
    "one Thread network, though the radios can't hear each other",
    "t2",
    "middle",
)
svg(
    "trel.svg",
    960,
    470,
    "TREL: two boards of the same Thread network, each with its own radio area of devices, connected over an Ethernet switch. TREL carries Thread links between them, so the two radio areas form one mesh.",
    b,
)
