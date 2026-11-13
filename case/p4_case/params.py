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
# board edge to the header-end wall: room to drop the board in shifted back, then slide its connectors into the
# cable wall (outer length 75 mm)
HEADER_GAP = 2.5
INNER = (-GAP, PCB_X1 + GAP, -GAP, PCB_Y1 + HEADER_GAP)  # x0, x1, y0, y1
# the SMA side wall is twice as thick: the antenna levers on it (print feedback); it grows outward, the board is inside
SMA_WALL = 4.0
# the button side wall too: thin side walls flexed and let the lid's detents slip (print feedback)
BUTTON_WALL = 4.0
# v6: the header-end wall is 3 mm, so the floor groove that seats the 40-pin curtain leaves 0.9 mm walls (outer
# length 76 mm)
HEADER_WALL = 3.0
OUTER = (
    INNER[0] - BUTTON_WALL,
    INNER[1] + SMA_WALL,
    INNER[2] - WALL,
    INNER[3] + HEADER_WALL,
)
INNER_R = 2.0 + GAP  # board corner radius 2 + gap
OUTER_R = 2.5  # outer vertical edges
# 45° on the outer bottom edge, which prints on the bed: no elephant's-foot lip around the case. Small, so the first
# layer keeps nearly all its area and the 45° face prints without support
OUTER_BOTTOM_CHAMFER = 0.5
# the outer top edge gets a bevel too; it runs on over the lid's parts that reach the outer face. 0.3, not 0.5: the
# cable-end wall's rim beside the rebate is only 1 mm, and 0.5 left a 0.5 mm strand in its top layer
OUTER_TOP_CHAMFER = 0.3
PLATE_T = 2.0  # case Z 0-2
STANDOFF_TOP = 7.0  # case Z (board underside)
LID_UNDERSIDE = 25.6  # case Z of the lid body's underside
# v6: the case is 1 mm taller and the lid 3 mm thick (the 2 mm lid flexed); the underside stays, so nothing inside moves
REBATE_FLOOR = 27.6  # case Z; the lid flange rests here
TOP = 28.6  # case Z of the wall tops and the lid top (flush)
# 5.2: the rebate takes 1 mm of the wall (half the cable-end wall, a quarter of the side walls)
REBATE_W = WALL / 2

# --- board screws (4, 5.1a) ---
STANDOFF_D = 6.0
# 45° flare where each standoff meets the plate: no sharp root corner, a stronger joint
STANDOFF_ROOT_CHAMFER = 2.5  # 1.5 left a sub-0.8 mm strand; 2.0-3.0 all clear the board
# each standoff is joined to its side wall by a web as wide as the post (all four sit 1 mm from a wall; the board's
# underside is clear there), so post and wall are one piece. The web stops this far below the post top: the board
# bears only on the round pad around its mounting hole
STANDOFF_WEB_DROP = 0.5
# 45° chamfer along the floor's edge into all four walls: no sharp root corner where the walls bend
WALL_ROOT_CHAMFER = 2.5
SCREW_HOLE_D = 2.2  # pilot hole: an M2.5 screw cuts its own thread in the standoff
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
# snap neck in each arm: a Ø3.5 shank clicks past it, so pushing a plug in can't lift the case off its screws
KEY_NECK_W = 3.3
# entry centre to neck centre; the shank parks past it at the arm's end
KEY_NECK_AT = 4.5
KEY_NECK_LEN = 1.0
# 45° on the bed side: first-layer squish would otherwise narrow the keyholes
KEY_CHAMFER = 0.4

# --- cable wall openings (5.2) ---
OPENING_MARGIN = 0.5
OPENING_TOP_MARGIN = 1.0  # room between a connector's top and the lid's lintel above it
OPENING_BOTTOM = -0.5  # board Z of the USB/RJ45 opening bottoms
USB_BOX, RJ45_BOX, SD_BOX = FACTS["usb"], FACTS["rj45"], FACTS["sd"]
SD_SLOT = (39.2, 51.4, -0.6, 2.0)  # x0, x1, board z0, z1
# USB and RJ45 share one opening, cut up through the wall top; the lid's lintel fills it above the connectors
CABLE_OPENING_X = (FACTS["usb"].x0 - OPENING_MARGIN, FACTS["rj45"].x1 + OPENING_MARGIN)
LINTEL_SPLIT_X = (
    FACTS["usb"].x1 + FACTS["rj45"].x0
) / 2  # where the lintel steps from the USB to the RJ45 height
# above the slot, where the card's grip edge is (print feedback). It starts right at the slot's top edge (a lip there
# blocked the fingernail and printed as a stray layer) and is deepest there; its ceiling slopes back to the face
SCOOP_X = (40.5, 50.5)
SCOOP_DEPTH = 2.2
SCOOP_RISE = 3.0
# above the slot only, the cable wall is thickened inward so 0.8 mm (two printed lines) stays behind the scoop
SD_WALL_IN = 1.0
SD_WALL_X = (SCOOP_X[0] - 1.0, SCOOP_X[1] + 1.0)

# --- header-end slot and ribbon-plug room (5.2) ---
# the full inner width: no thin slivers at the corners
HEADER_SLOT_X = (INNER[0], INNER[1])
# the slot is open to the wall top; the lid fills it down to its underside and hangs a break-off curtain below that.
# With the curtain broken out the passage is HEADER_SLOT_CLEAR tall (case Z 20.6-25.6)
HEADER_SLOT_CLEAR = 10.0  # case Z 15.6-25.6; the curtain fills it
HEADER_SLOT_BOTTOM = LID_UNDERSIDE - HEADER_SLOT_CLEAR
# along the slot floor, centred in the wall: the curtain's tongues sit in it
SLOT_GROOVE_W = 1.2
SLOT_GROOVE_D = 0.8
# length (X), depth (Y), height; seated on the header body
RIBBON_PLUG = (55.9, 9.0, 9.0)
HEADER_BODY_TOP = 2.5  # board Z where a plug seats (top of the header's plastic body)

# --- buttons (5.2) ---
BUTTON_HOLE_D = 2.5
BUTTON_FUNNEL_D = 4.5  # 90-degree countersink at the outer face guides a pencil tip into the 4 mm-deep hole
BUTTON_RECESS_X = -1.0  # left wall's inner face pulled back to here around the buttons
BUTTON_RECESS_MARGIN = 0.5

# --- SMA (5.2) ---
SMA_Y = 52.0
SMA_CASE_Z = 17.6
SMA_D = 6.5
# across the flat; the flat is at the bottom, the top is round (print feedback: it needs no teardrop)
SMA_FLAT = 5.9
# two extrusion lines left over the hole at the outer face; cut it out to fit the SMA
SMA_KNOCKOUT_T = 0.8

# --- lid and tabs (5.3) ---
# 3.0: 2 mm body inside the walls + 1 mm flange in the rebate
LID_T = TOP - LID_UNDERSIDE
LID_CLEAR = 0.2  # per side, flange to rebate and body to inner wall
# 45° on the top face's edge, which prints on the bed: room for elephant's foot
LID_TOP_CHAMFER = 0.4
LAYER_H = 0.2  # the print layer height the detent is aligned to
FRAME_W = 5.5
CELL = 3.5  # honeycomb, across flats
CELL_WALL = 1.0
RIB_Y = (INNER[2] + INNER[3]) / 2  # the lid's centre line
RIB_W = 1.8
TAB_LEN = 10.0
TAB_DROP = 3.5
TAB_ROOT_T = 2.4
TAB_TIP_T = 1.9  # inner-face taper target, before the outer lead-in
TAB_FLAT = 2.0
TAB_LEAD = 0.5
TAB_CLEAR = 0.2
# 45° root gusset, legs along the plate and down the tab (a fillet mostly vanishes when sliced)
TAB_GUSSET = 1.2
# 0.4 is back: the 4 mm side walls no longer flex, the 45° hold face and the preload do the retaining
BUMP_H = 0.4  # from the tab face; reaches BUMP_H - TAB_CLEAR past the wall face
BUMP_LEAD_ANGLE = 30.0  # lower face, from the tab face: the lid closes over it
BUMP_HOLD_ANGLE = 45.0  # upper face: opening takes about 1.75x the force of closing
BUMP_LEN = 6.0  # along Y
# case Z of the bump's crest, on a layer centre of the top-down lid
BUMP_Z = LID_UNDERSIDE - 0.9
# into the wall: as far as the bump reaches past the wall face
DENT_DEPTH = BUMP_H - TAB_CLEAR
# dent sits this much lower: its hold face presses the bump, pulling the lid down; no rattle
DENT_PRELOAD = 0.1
DENT_LEN = BUMP_LEN + 1.0

# --- 40-pin curtain (v6) ---
CURTAIN_T = HEADER_WALL - 2 * LID_CLEAR  # 2.6, centred in the header-end wall
CURTAIN_NECK = 0.8  # left by the V-notches at the break line (the lid's underside, a printed layer boundary)
CURTAIN_SEGMENTS = 4
CURTAIN_SLIT = 0.4
CURTAIN_FLOOR_GAP = 0.2  # segment bottoms to the slot floor
CURTAIN_TONGUE_W = 0.8  # in the floor groove, 0.2 mm clear each side

# --- keying teeth (v7) ---
# two per side wall, straight above the corner snap tabs: the lid's tooth drops into a notch in the wall top, flush
# outside and on top, and keys the lid both ways (print feedback: the outside ears broke, but the block they left in
# the wall-top cut made the fit snug). They replace the screw ears; the snaps hold the lid
TOOTH_LEN = 6.0  # along the wall
TOOTH_DEPTH = 2.0  # down from the top
TOOTH_LEAD = 0.5  # 45° lead-in on the tooth's lower corners (the notch follows it)
TOOTH_GAP = 0.2  # notch to tooth, all round
