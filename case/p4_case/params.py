"""Case dimensions (spec sections 4-5). XY in the board frame; heights given in case Z and converted with board_z().

Case Z 0 is the outer back face; the board's top face (board Z 0) sits at case Z 8.6. Geometry is built in the board
frame, so a feature at case height h is placed at board_z(h).
"""

from p4_case.board import FACTS, HOLES

BOARD_Z_IN_CASE = 8.6  # 5.1: case Z of the PCB's top face


def case_z(board_z_value: float) -> float:
    """Case height of a board-frame Z."""
    return board_z_value + BOARD_Z_IN_CASE


def board_z(case_z_value: float) -> float:
    """Board-frame Z of a case height."""
    return case_z_value - BOARD_Z_IN_CASE


# --- outline and layers (4, 5) ---
WALL = 2.0
GAP = 0.5  # board edge to inner wall face
PCB_X1, PCB_Y1 = 55.0, 68.0
INNER = (-GAP, PCB_X1 + GAP, -GAP, PCB_Y1 + GAP)  # x0, x1, y0, y1
OUTER = (INNER[0] - WALL, INNER[1] + WALL, INNER[2] - WALL, INNER[3] + WALL)
INNER_R = 2.0 + GAP  # board corner radius 2 + gap
OUTER_R = INNER_R + WALL
PLATE_T = 2.0  # case Z 0-2
STANDOFF_TOP = 7.0  # case Z (board underside)
LID_UNDERSIDE = 25.6  # case Z of the lid body's underside
REBATE_FLOOR = 26.6  # case Z; the lid flange rests here
TOP = 27.6  # case Z of the wall tops and the lid top (flush)
REBATE_W = WALL / 2  # 5.2: the rebate takes the inner half of the wall

# --- board screws (4, 5.1a) ---
STANDOFF_D = 6.0
SCREW_HOLE_D = 2.8  # M2.5 clearance
CSK_D = 5.4  # countersink diameter at the back face: M2.5 flat head (Ø5.0) sunk 0.2 below flush
CSK_ANGLE = 90.0
SCREW_HOLES = HOLES

# --- keyholes (5.1) ---
KEYHOLES = ((10.0, 42.0), (42.0, 42.0))
KEY_ENTRY_D = 8.0
KEY_ARM_W = 4.3
KEY_ARM_L = 7.0  # entry centre to arm-end centre, toward +Y and +X
KEY_HEAD_D = 7.5
KEY_HEAD_H = 2.5

# --- cable wall openings (5.2) ---
OPENING_MARGIN = 0.5
OPENING_TOP_MARGIN = 1.0  # extra room at the top so the board can tilt in
OPENING_BOTTOM = -0.5  # board Z of the USB/RJ45 opening bottoms
USB_BOX, RJ45_BOX, SD_BOX = FACTS["usb"], FACTS["rj45"], FACTS["sd"]
SD_SLOT = (39.2, 51.4, -0.6, 2.0)  # x0, x1, board z0, z1
SCOOP_X = (41.5, 49.0)
SCOOP_R = 2.4
SCOOP_DEPTH = 1.5  # into the cable wall's outer face, at the slot's bottom edge

# --- header-end slot and ribbon-plug room (5.2) ---
HEADER_SLOT_X = (0.5, 54.5)
HEADER_SLOT_CLEAR = 3.5  # clear height under the lid flange
# case Z 23.1; the slot is cut open to the wall top (no bridge)
HEADER_SLOT_BOTTOM = REBATE_FLOOR - HEADER_SLOT_CLEAR
# length (X), depth (Y), height; seated on the header body
RIBBON_PLUG = (55.9, 9.0, 9.0)
HEADER_BODY_TOP = 2.5  # board Z where a plug seats (top of the header's plastic body)
RIBBON_RECESS = 1.0  # depth into the walls around the header end
RIBBON_RECESS_Y0 = 58.0
RIBBON_RECESS_TOP = HEADER_BODY_TOP + RIBBON_PLUG[2] + 1.0  # board Z

# --- buttons (5.2) ---
BUTTON_HOLE_D = 2.5
BUTTON_RECESS_X = -1.0  # left wall's inner face pulled back to here around the buttons
BUTTON_RECESS_MARGIN = 0.5

# --- SMA (5.2) ---
SMA_Y = 52.0
SMA_CASE_Z = 17.6
SMA_D = 6.5
SMA_FLAT = 5.9  # across the flat; the flat is on top

# --- lid and tabs (5.3) ---
# 2.0: 1 mm body inside the walls + 1 mm flange in the rebate
LID_T = TOP - LID_UNDERSIDE
LID_CLEAR = 0.2  # per side, flange to rebate and body to inner wall
FRAME_W = 5.5
CELL = 3.5  # honeycomb, across flats
CELL_WALL = 1.0
RIB_Y = 34.0
RIB_W = 1.8
LED_XY = (0.96, 18.2)  # LED centre
TAB_LEN = 10.0
TAB_DROP = 3.5
TAB_ROOT_T = 2.4
TAB_TIP_T = 1.9  # inner-face taper target, before the outer lead-in
TAB_FLAT = 2.0
TAB_LEAD = 0.5
TAB_CLEAR = 0.2
TAB_FILLET = 1.5
BUMP_H = 0.4
BUMP_ANGLE = 30.0  # ramp angle from the tab face
BUMP_LEN = 6.0  # along Y
BUMP_Z = LID_UNDERSIDE - 1.0  # case Z of the bump's crest
DENT_DEPTH = BUMP_H + 0.1
DENT_LEN = BUMP_LEN + 1.0
