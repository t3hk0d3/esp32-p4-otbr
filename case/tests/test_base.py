"""p4_case.base.build_base (BDD: p4_case.base.build_base). Geometry is probed with points in the board frame."""

import math

import pytest
from build123d import Vector
from p4_case import params as P
from p4_case.params import board_z


def solid(part, x, y, case_z):
    return part.is_inside(Vector(x, y, board_z(case_z)))


def solid_bz(part, x, y, bz):
    return part.is_inside(Vector(x, y, bz))


class TestDefaultBase:
    """base.build_base: On default parameters"""

    def test_returns_one_valid_watertight_solid(self, base):
        assert len(base.solids()) == 1
        assert base.is_valid

    def test_has_the_outer_size_64_by_76_by_28_6(self, base):
        bb = base.bounding_box()
        assert bb.size.X == pytest.approx(64.0, abs=0.1)
        assert bb.size.Y == pytest.approx(76.0, abs=0.1)
        assert bb.size.Z == pytest.approx(28.6, abs=0.1)
        assert bb.min.Z == pytest.approx(board_z(0.0), abs=0.01)

    def test_has_the_rebate_1_mm_deep_and_1_mm_wide_on_all_walls(self, base):
        x0, x1, y0, y1 = P.INNER
        assert P.REBATE_FLOOR == pytest.approx(27.6)
        top, below = P.TOP - 0.5, P.REBATE_FLOOR - 0.5
        # side walls probed at Y 50, clear of the screw ears at Y 32
        for x, y in (
            (x0 - 0.5, 50),
            (x1 + 0.5, 50),
            (27.5, y0 - 0.5),
            (27.5, y1 + 0.5),
        ):
            assert not solid(base, x, y, top)  # inner 1 mm: rebate
        for x, y in ((x0 - 0.5, 50), (x1 + 0.5, 50), (45.0, y0 - 0.5)):
            # below the rebate floor: wall (the header end has the slot)
            assert solid(base, x, y, below)
        for x, y in ((x0 - 1.5, 50), (x1 + 1.5, 50), (45.0, y0 - 1.5)):
            assert solid(base, x, y, top)  # the rest reaches the top

    def test_has_the_usb_rj45_and_sd_openings_at_the_spec_positions(self, base):
        y = -1.5  # inside the cable wall
        assert not solid_bz(base, 12.0, y, 1.0)  # USB-C
        assert not solid_bz(base, 12.0, y, 12.0)  # USB-A
        assert not solid_bz(base, 3.9, y, 8.0)  # USB margin, left
        assert solid_bz(base, 3.6, y, 8.0)  # past the margin
        assert not solid_bz(base, 30.0, y, 7.0)  # RJ45
        assert not solid_bz(base, 38.3, y, 7.0)
        assert solid_bz(base, 38.6, y, 7.0)
        assert not solid_bz(base, 45.0, y, 1.0)  # microSD slot
        assert solid_bz(base, 45.0, y, 4.0)  # above the slot and its scoop

    def test_leaves_no_lip_between_the_microsd_slot_and_the_scoop(self, base):
        _, _, _, sz1 = P.SD_SLOT
        x = sum(P.SCOOP_X) / 2
        assert not solid_bz(base, x, P.OUTER[2] + 0.3, sz1 + 0.1)

    def test_cuts_the_scoop_2_2_deep_10_wide_and_3_tall(self, base):
        _, _, sz0, sz1 = P.SD_SLOT
        x0, x1 = P.SCOOP_X
        assert (P.SCOOP_DEPTH, x1 - x0, P.SCOOP_RISE) == pytest.approx((2.2, 10.0, 3.0))
        face = P.OUTER[2]
        x = (x0 + x1) / 2
        assert not solid_bz(base, x, face + P.SCOOP_DEPTH - 0.2, sz1 + 0.1)  # deep
        assert solid_bz(base, x, face + P.SCOOP_DEPTH + 0.2, sz1 + 0.1)  # no deeper
        assert not solid_bz(base, x, face + 0.3, sz1 + 2.3)  # tall
        assert solid_bz(base, x, face + 0.3, sz1 + 2.9)
        assert not solid_bz(base, x0 + 0.3, face + 0.5, sz1 + 0.5)  # wide
        assert solid_bz(base, x0 - 0.3, face + 0.5, sz1 + 0.5)
        assert solid_bz(base, x1 + 0.3, face + 0.5, sz1 + 0.5)
        assert solid_bz(base, x, face + 0.3, sz0 - 0.5)  # nothing below the slot

    def test_keeps_0_8_mm_of_wall_behind_the_scoops_deepest_point(self, base):
        _, _, _, sz1 = P.SD_SLOT
        x = sum(P.SCOOP_X) / 2
        inner = P.INNER[2] + P.SD_WALL_IN
        assert inner - (P.OUTER[2] + P.SCOOP_DEPTH) == pytest.approx(0.8)
        assert solid_bz(base, x, inner - 0.1, sz1 + 0.1)
        assert not solid_bz(base, x, inner + 0.1, sz1 + 0.1)

    def test_thickens_the_cable_wall_inward_above_the_microsd_slot_only(self, base):
        _, _, sz0, sz1 = P.SD_SLOT
        y = P.INNER[2] + P.SD_WALL_IN / 2
        assert solid_bz(base, 45.0, y, sz1 + 1.0)  # above the slot
        assert not solid_bz(base, 45.0, y, (sz0 + sz1) / 2)  # not beside it
        assert not solid_bz(base, 45.0, y, sz0 - 1.0)  # not below it
        assert not solid_bz(base, 30.0, y, sz1 + 1.0)  # not along the rest of the wall

    def test_has_one_usb_and_rj45_opening_open_to_the_wall_top(self, base):
        y = -1.5
        mid = P.LINTEL_SPLIT_X
        assert not solid_bz(base, mid, y, 8.0)  # no pillar between USB and RJ45
        assert not solid(base, 12.0, y, P.TOP - 0.1)  # open to the top: no bridge
        assert not solid(base, 30.0, y, P.TOP - 0.1)
        assert solid_bz(base, P.CABLE_OPENING_X[0] - 0.2, y, 8.0)
        assert solid_bz(
            base, P.CABLE_OPENING_X[1] + 0.3, y, 8.0
        )  # above the microSD slot

    def test_has_the_header_slot_open_to_the_top_with_5_mm_clear(self, base):
        assert P.LID_UNDERSIDE - P.HEADER_SLOT_BOTTOM == pytest.approx(5.0)
        y = P.INNER[3] + 0.4  # in the header-end wall, beside the floor groove
        assert not solid(base, 27.5, y, P.HEADER_SLOT_BOTTOM + 0.1)
        assert not solid(base, 27.5, y, P.TOP - 0.1)  # open to the top: no bridge
        assert solid(base, 27.5, y, P.HEADER_SLOT_BOTTOM - 0.1)

    def test_runs_the_header_slot_the_full_inner_width(self, base):
        y = P.INNER[3] + 1.5
        z = P.HEADER_SLOT_BOTTOM + 1.0
        assert not solid(base, P.INNER[0] + 0.1, y, z)  # no sliver at the corners
        assert not solid(base, P.INNER[1] - 0.1, y, z)
        # corner rounding gone
        assert not solid(base, P.INNER[0] + 0.3, P.INNER[3] - 0.3, z)
        assert solid(base, P.INNER[0] - 0.3, y, z)  # side walls stay
        assert solid(base, P.INNER[1] + 0.3, y, z)

    def test_cuts_a_groove_along_the_slot_floor_centred_in_the_wall(self, base):
        mid = (P.INNER[3] + P.OUTER[3]) / 2
        w, d = P.SLOT_GROOVE_W, P.SLOT_GROOVE_D
        assert (w, d) == pytest.approx((1.2, 0.8))
        assert (P.OUTER[3] - P.INNER[3] - w) / 2 == pytest.approx(
            0.9
        )  # walls beside it
        floor = P.HEADER_SLOT_BOTTOM
        for x in (5.0, 27.5, 50.0):
            assert not solid(base, x, mid, floor - d + 0.1)  # groove
            assert solid(base, x, mid, floor - d - 0.1)  # its bottom
            assert solid(base, x, mid + w / 2 + 0.1, floor - 0.1)  # floor beside it
            assert solid(base, x, mid - w / 2 - 0.1, floor - 0.1)

    def test_has_two_pin_holes_on_the_plunger_axes_and_the_button_recess(self, base):
        x = P.INNER[0] - 1.2  # inside the left wall
        for btn in ("button_a", "button_b"):
            b = P.FACTS[btn]
            yc, zc = (b.y0 + b.y1) / 2, (b.z0 + b.z1) / 2
            assert not solid_bz(base, x, yc, zc)
            assert solid_bz(base, x, yc + 1.4, zc)  # Ø2.5 only
            assert not solid_bz(base, -0.7, yc, zc)  # inner recess
        assert solid_bz(base, -0.7, 25.0, 1.4)  # no recess away from the buttons

    def test_chamfers_the_button_recess_ceiling_at_45_degrees(self, base):
        a, b = P.FACTS["button_a"], P.FACTS["button_b"]
        y = (a.y0 + b.y1) / 2
        ceiling = a.z1 + P.BUTTON_RECESS_MARGIN
        assert not solid_bz(base, -0.6, y, ceiling + 0.2)  # under the 45° slope
        assert solid_bz(base, -0.9, y, ceiling + 0.2)  # above it
        assert solid_bz(base, -0.6, y, ceiling + 0.6)  # the slope ends at the face

    def test_has_the_sma_d_hole_at_y_52_with_the_flat_at_the_bottom(self, base):
        x = P.INNER[1] + 1.0  # inside the wall, behind the knock-out
        zc = board_z(P.SMA_CASE_Z)
        assert not solid_bz(base, x, P.SMA_Y, zc)
        # round top: an arch, no support
        assert not solid_bz(base, x, P.SMA_Y, zc + 3.1)
        assert not solid_bz(base, x, P.SMA_Y, zc - 2.5)
        assert solid_bz(base, x, P.SMA_Y, zc - 2.8)  # the flat
        assert solid_bz(base, x, P.SMA_Y + 3.5, zc)

    def test_gives_the_sma_hole_a_round_top(self, base):
        x = P.INNER[1] + 1.0
        zc = board_z(P.SMA_CASE_Z)
        r = P.SMA_D / 2
        assert solid_bz(base, x, P.SMA_Y, zc + r + 0.2)  # no teardrop above the circle
        assert not solid_bz(base, x, P.SMA_Y + 2.2, zc + 2.2)  # round at 45°

    def test_makes_the_whole_button_side_wall_4_mm_thick(self, base):
        x = P.INNER[0] - P.BUTTON_WALL + 0.2
        for y in (25.0, 45.0, 65.0):
            assert solid_bz(base, x, y, 10.0)
        assert not solid_bz(base, P.INNER[0] - P.BUTTON_WALL - 0.1, 45.0, 10.0)

    def test_funnels_the_button_holes_at_the_outer_face(self, base):
        b = P.FACTS["button_a"]
        yc, zc = (b.y0 + b.y1) / 2, (b.z0 + b.z1) / 2
        r = P.BUTTON_FUNNEL_D / 2
        assert not solid_bz(base, P.OUTER[0] + 0.1, yc + r - 0.3, zc)  # funnel mouth
        assert solid_bz(base, P.OUTER[0] + 1.5, yc + r - 0.3, zc)  # narrows to the hole

    def test_makes_the_whole_sma_side_wall_4_mm_thick(self, base):
        x = P.INNER[1] + P.SMA_WALL - 0.2
        for y in (5.0, 45.0, 65.0):  # clear of the screw ear's pocket at Y 32
            assert solid_bz(base, x, y, 10.0)
        assert not solid_bz(base, P.INNER[1] + P.SMA_WALL + 0.1, 35.0, 10.0)

    def test_closes_the_sma_hole_with_a_two_line_knock_out(self, base):
        zc = board_z(P.SMA_CASE_Z)
        assert P.SMA_KNOCKOUT_T == pytest.approx(0.8)
        assert solid_bz(base, P.OUTER[1] - P.SMA_KNOCKOUT_T / 2, P.SMA_Y, zc)
        assert not solid_bz(base, P.OUTER[1] - P.SMA_KNOCKOUT_T - 0.1, P.SMA_Y, zc)

    def test_keeps_the_side_walls_full_thickness_at_the_header_end(self, base):
        for x in (P.INNER[0] - 0.5, P.INNER[1] + 0.5):
            for z in (2.0, 8.0, 12.0):
                assert solid_bz(base, x, 63.0, z)

    def test_keeps_the_header_end_outer_corners_closed(self, base):
        ox0, ox1, _, oy1 = P.OUTER
        r = P.OUTER_R
        d = r * (1 - 1 / 2**0.5)  # outer surface on the corner diagonal
        for cx, sx in ((ox0, 1), (ox1, -1)):
            x, y = cx + sx * (d + 0.5), oy1 - (d + 0.5)
            for z in (2.0, 8.0, 12.0):
                assert solid_bz(base, x, y, z), (x, y, z)

    def test_has_four_tab_dents_just_below_the_bumps(self, base):
        z = P.BUMP_Z - P.DENT_PRELOAD  # the dent's deepest line
        for y in (
            P.INNER[2] + P.TAB_CLEAR + P.TAB_LEN / 2,
            P.INNER[3] - P.TAB_CLEAR - P.TAB_LEN / 2,
        ):
            for x, d in ((P.INNER[0], -1), (P.INNER[1], 1)):
                assert not solid(base, x + d * 0.1, y, z)  # dent
                assert solid(base, x + d * 0.1, y, z - 1.5)  # wall below the dent
                assert solid(base, x + d * 0.1, y, z + 0.6)  # wall above the dent

    def test_makes_each_dent_as_deep_as_the_bump_reaches_past_the_wall(self, base):
        depth = P.BUMP_H - P.TAB_CLEAR
        z = P.BUMP_Z - P.DENT_PRELOAD
        y = P.INNER[2] + P.TAB_CLEAR + P.TAB_LEN / 2
        assert P.DENT_DEPTH == pytest.approx(depth)
        assert not solid(base, P.INNER[0] - (depth - 0.05), y, z)
        assert solid(base, P.INNER[0] - (depth + 0.05), y, z)

    def test_has_four_6_mm_standoffs_5_mm_tall_on_the_hole_axes(self, base):
        for hx, hy in P.SCREW_HOLES:
            d = 1 if hx < 27.5 else -1  # toward the middle; the wall side is a web
            assert solid(base, hx + d * 2.7, hy, 6.5)
            assert not solid(base, hx + d * 3.3, hy, 6.5)
            assert not solid(base, hx + d * 2.7, hy, 7.2)
            assert not solid(base, hx, hy + 3.3, 6.5)  # Ø6 across the case's length too

    def test_joins_each_standoff_to_its_side_wall(self, base):
        r = P.STANDOFF_D / 2
        web_top = P.STANDOFF_TOP - P.STANDOFF_WEB_DROP
        for hx, hy in P.SCREW_HOLES:
            wall = P.INNER[0] if hx < 27.5 else P.INNER[1]
            d = 1 if hx < 27.5 else -1
            x = wall + d * 0.3  # between the wall and the post
            for y in (hy, hy + r - 0.2, hy - r + 0.2):
                assert solid(base, x, y, web_top - 0.1)
            assert not solid(base, x, hy + r + 0.2, web_top - 0.1)  # web is Ø6 wide

    def test_lets_the_board_bear_only_on_the_round_pad(self, base):
        assert P.STANDOFF_WEB_DROP == pytest.approx(0.5)
        web_top = P.STANDOFF_TOP - P.STANDOFF_WEB_DROP
        for hx, hy in P.SCREW_HOLES:
            wall = P.INNER[0] if hx < 27.5 else P.INNER[1]
            d = 1 if hx < 27.5 else -1
            assert not solid(base, wall + d * 0.3, hy, web_top + 0.1)  # below the board
            assert solid(base, hx - d * 2.7, hy, P.STANDOFF_TOP - 0.1)  # pad, wall side

    def test_chamfers_the_outer_bottom_edge_against_elephants_foot(self, base):
        c = P.OUTER_BOTTOM_CHAMFER
        assert c == pytest.approx(0.5)
        x0, x1, y0, y1 = P.OUTER
        for x, y in (
            (x0 + 0.1, 40.0),
            (x1 - 0.1, 40.0),
            (27.5, y0 + 0.1),
            (27.5, y1 - 0.1),
        ):
            assert not solid(base, x, y, 0.1)  # cut back at the bed
            assert solid(base, x, y, c + 0.1)  # full outline above it

    def test_flares_the_standoff_webs_into_the_plate_too(self, base):
        c, r = P.STANDOFF_ROOT_CHAMFER, P.STANDOFF_D / 2
        for hx, hy in P.SCREW_HOLES:
            wall = P.INNER[0] if hx < 27.5 else P.INNER[1]
            x = (wall + hx) / 2
            y = hy + r + c / 2
            assert solid(base, x, y, P.PLATE_T + c / 2 - 0.2)
            assert not solid(base, x, y, P.PLATE_T + c / 2 + 0.2)

    def test_chamfers_the_floor_into_all_four_walls_at_45_degrees(self, base):
        c = P.WALL_ROOT_CHAMFER
        assert c == pytest.approx(2.5)
        ix0, ix1, iy0, iy1 = P.INNER
        z_in, z_out = P.PLATE_T + c / 2 - 0.2, P.PLATE_T + c / 2 + 0.2
        for x, y in (
            (ix0 + c / 2, 40.0),
            (ix1 - c / 2, 40.0),
            (27.5, iy0 + c / 2),
            (27.5, iy1 - c / 2),
        ):
            assert solid(base, x, y, z_in), (x, y)
            assert not solid(base, x, y, z_out), (x, y)
        assert not solid(base, ix0 + c + 0.1, 40.0, P.PLATE_T + 0.05)  # ends c out

    def test_flares_each_standoff_into_the_plate_at_45_degrees(self, base):
        c = P.STANDOFF_ROOT_CHAMFER
        r = P.STANDOFF_D / 2
        assert c == pytest.approx(2.5)
        for hx, hy in P.SCREW_HOLES:
            d = 1 if hx < 27.5 else -1  # probe toward the middle, away from the walls
            x = hx + d * (r + c / 2)
            assert solid(base, x, hy, P.PLATE_T + c / 2 - 0.2)  # in the flare
            assert not solid(base, x, hy, P.PLATE_T + c / 2 + 0.2)  # above its slope
            assert not solid(base, hx + d * (r + c + 0.1), hy, P.PLATE_T + 0.05)

    def test_has_a_countersunk_through_hole_under_each_standoff(self, base):
        for hx, hy in P.SCREW_HOLES:
            for cz in (0.05, 1.0, 4.0, 6.9):
                assert not solid(base, hx, hy, cz)
            assert not solid(base, hx + 2.3, hy, 0.25)  # countersink near the surface
            assert solid(base, hx + 1.6, hy, 1.8)  # plain bore deeper in

    def test_narrows_each_keyhole_arm_to_a_snap_neck(self, base):
        half = P.KEY_NECK_W / 2
        for kx, ky in P.KEYHOLES:
            at = P.KEY_NECK_AT
            # +Y arm, then +X arm
            assert solid(base, kx + half + 0.1, ky + at, 1.0)
            assert not solid(base, kx + half - 0.1, ky + at, 1.0)
            assert solid(base, kx + at, ky - half - 0.1, 1.0)
            assert not solid(base, kx + at, ky - half + 0.1, 1.0)
            # full width again where the shank rests
            assert not solid(base, kx + P.KEY_ARM_W / 2 - 0.1, ky + P.KEY_ARM_L, 1.0)

    def test_parks_the_shank_past_the_neck(self, base):
        shank_r = 3.5 / 2
        assert P.KEY_NECK_AT + P.KEY_NECK_LEN / 2 < P.KEY_ARM_L - shank_r
        assert P.KEY_NECK_AT - P.KEY_NECK_LEN / 2 > P.KEY_ENTRY_D / 2 - 0.5

    def test_chamfers_the_keyholes_on_the_bed_side(self, base):
        for kx, ky in P.KEYHOLES:
            edge = P.KEY_ARM_W / 2
            assert not solid(base, kx + edge + 0.2, ky + 6.5, 0.1)  # chamfer
            assert solid(base, kx + edge + 0.2, ky + 6.5, P.KEY_CHAMFER + 0.2)

    def test_has_two_l_keyholes_with_entry_and_both_arms(self, base):
        for kx, ky in P.KEYHOLES:
            assert not solid(base, kx + 3.8, ky, 1.0)  # Ø8 entry
            assert not solid(base, kx, ky + 7.0, 1.0)  # +Y arm end
            assert not solid(base, kx + 7.0, ky, 1.0)  # +X arm end
            assert solid(base, kx + 2.4, ky + 5.0, 1.0)  # arm is 4.3 wide
            assert solid(base, kx, ky + 9.5, 1.0)  # plate past the arm

    def test_keeps_the_keyhole_sweep_areas_free(self, base):
        r = P.KEY_HEAD_D / 2
        for kx, ky in P.KEYHOLES:
            for dx in range(-4, int(P.KEY_ARM_L + r) + 1):
                for dy in range(-4, int(P.KEY_ARM_L + r) + 1):
                    if math.hypot(min(dx, 0), min(dy, 0)) > r:
                        continue
                    assert not solid(base, kx + dx, ky + dy, P.PLATE_T + 1.0)


def _wall(side):
    """(outer face X, inward direction) of the left (-1) or right (+1) side wall."""
    return (P.OUTER[0], 1) if side < 0 else (P.OUTER[1], -1)


class TestOutsideEarPockets:
    """base.build_base: On default parameters (outside ears)"""

    def test_defaults_to_outside_ears(self):
        assert P.EAR_STYLE == "outside"

    def test_drops_each_side_wall_top_to_the_lid_underside_over_the_ear(self, base):
        half = P.EAR_W / 2 + P.EAR_GAP
        for side in (-1, 1):
            face, inward = _wall(side)
            for u in (2.0, 3.7):  # behind the pocket, across the rest of the wall
                x = face + inward * u
                assert not solid(base, x, P.EAR_Y, P.LID_UNDERSIDE + 0.1)
                assert not solid(base, x, P.EAR_Y + half - 0.1, P.LID_UNDERSIDE + 0.1)
                assert solid(base, x, P.EAR_Y, P.LID_UNDERSIDE - 0.1)
            assert solid(base, face + inward * 2.0, P.EAR_Y + half + 0.1, P.TOP - 0.1)

    def test_cuts_a_1_6_mm_u_pocket_into_each_outer_face(self, base):
        r = P.EAR_W / 2 + P.EAR_GAP
        zc = P.EAR_SCREW_Z
        assert P.EAR_T == pytest.approx(1.6)
        for side in (-1, 1):
            face, inward = _wall(side)
            x_in = face + inward * (P.EAR_T - 0.1)
            x_past = face + inward * (P.EAR_T + 0.1)
            assert not solid(base, x_in, P.EAR_Y + r - 0.1, zc + 2.0)  # straight sides
            assert solid(base, x_in, P.EAR_Y + r + 0.1, zc + 2.0)
            assert not solid(base, x_in, P.EAR_Y, zc - r + 0.1)  # half-circle bottom
            assert solid(base, x_in, P.EAR_Y, zc - r - 0.1)
            assert solid(base, x_in, P.EAR_Y + r * 0.8, zc - r * 0.8)  # round corners
            assert solid(base, x_past, P.EAR_Y + 3.0, zc + 2.0)  # only EAR_T deep

    def test_drills_a_2_2_pilot_through_the_wall_behind_each_pocket(self, base):
        zc = P.EAR_SCREW_Z
        r = P.EAR_PILOT_D / 2
        assert P.EAR_PILOT_D == pytest.approx(2.2)
        for side in (-1, 1):
            face, inward = _wall(side)
            for u in (P.EAR_T + 0.2, 3.0, 3.8):
                x = face + inward * u
                assert not solid(base, x, P.EAR_Y, zc)
                assert solid(base, x, P.EAR_Y + r + 0.2, zc)


class TestInsideEarWallHoles:
    """base.build_base: With ears inside"""

    def test_drills_a_3_0_clearance_hole_through_each_side_wall(self, base_inside):
        zc, r = P.IN_EAR_SCREW_Z, P.IN_EAR_CLEAR_D / 2
        for side in (-1, 1):
            face, inward = _wall(side)
            for u in (1.5, 2.5, 3.8):
                x = face + inward * u
                assert not solid(base_inside, x, P.EAR_Y, zc)
                assert not solid(base_inside, x, P.EAR_Y + r - 0.1, zc)
                assert solid(base_inside, x, P.EAR_Y + r + 0.2, zc)

    def test_countersinks_each_hole_to_5_0_at_the_outer_face(self, base_inside):
        zc, r = P.IN_EAR_SCREW_Z, P.IN_EAR_CSK_D / 2
        for side in (-1, 1):
            face, inward = _wall(side)
            assert not solid(base_inside, face + inward * 0.1, P.EAR_Y + r - 0.3, zc)
            assert solid(base_inside, face + inward * 1.2, P.EAR_Y + r - 0.3, zc)

    def test_has_no_wall_top_cut_and_no_pocket(self, base_inside):
        for side in (-1, 1):
            face, inward = _wall(side)
            assert solid(base_inside, face + inward * 1.5, P.EAR_Y, P.TOP - 0.1)
            assert solid(base_inside, face + inward * 0.5, P.EAR_Y, P.EAR_SCREW_Z - 4.0)

    def test_is_otherwise_the_default_base(self, base, base_inside):
        for x, y, z in ((27.5, 71.5, 21.0), (45.0, -1.5, 12.0), (-2.5, 50.0, 28.0)):
            assert solid(base, x, y, z) == solid(base_inside, x, y, z)
