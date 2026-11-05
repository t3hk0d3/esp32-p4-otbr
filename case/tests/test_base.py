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

    def test_has_the_outer_size_62_by_75_by_27_6(self, base):
        bb = base.bounding_box()
        assert bb.size.X == pytest.approx(62.0, abs=0.1)
        assert bb.size.Y == pytest.approx(75.0, abs=0.1)
        assert bb.size.Z == pytest.approx(27.6, abs=0.1)
        assert bb.min.Z == pytest.approx(board_z(0.0), abs=0.01)

    def test_has_the_rebate_1_mm_deep_and_half_a_wall_wide_on_all_walls(self, base):
        x0, x1, y0, y1 = P.INNER
        for x, y in (
            (x0 - 0.5, 34),
            (x1 + 0.5, 34),
            (27.5, y0 - 0.5),
            (27.5, y1 + 0.5),
        ):
            assert not solid(base, x, y, 27.1)  # inner half: rebate
        for x, y in ((x0 - 0.5, 34), (x1 + 0.5, 34), (45.0, y0 - 0.5)):
            assert solid(
                base, x, y, 26.1
            )  # below the rebate floor: wall (header end has the slot)
        for x, y in ((x0 - 1.5, 34), (x1 + 1.5, 34), (45.0, y0 - 1.5)):
            assert solid(base, x, y, 27.1)  # outer half reaches the top

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
        assert solid_bz(base, 45.0, y, 2.3)  # above the slot

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

    def test_has_the_header_slot_open_to_the_top_with_3_5_clear(self, base):
        y = P.INNER[3] + 1.5  # outer half of the header-end wall
        assert not solid(base, 27.5, y, P.HEADER_SLOT_BOTTOM + 0.1)
        assert not solid(base, 27.5, y, P.TOP - 0.1)  # open to the top: no bridge
        assert solid(base, 27.5, y, P.HEADER_SLOT_BOTTOM - 0.1)
        assert solid(base, 0.3, y, P.HEADER_SLOT_BOTTOM + 1.0)
        assert not solid(base, 0.7, y, P.HEADER_SLOT_BOTTOM + 1.0)

    def test_has_two_pin_holes_on_the_plunger_axes_and_the_button_recess(self, base):
        x = P.INNER[0] - 1.2  # inside the left wall
        for btn in ("button_a", "button_b"):
            b = P.FACTS[btn]
            yc, zc = (b.y0 + b.y1) / 2, (b.z0 + b.z1) / 2
            assert not solid_bz(base, x, yc, zc)
            assert solid_bz(base, x, yc + 1.4, zc)  # Ø2.5 only
            assert not solid_bz(base, -0.7, yc, zc)  # inner recess
        assert solid_bz(base, -0.7, 25.0, 1.4)  # no recess away from the buttons

    def test_has_the_sma_d_hole_at_y_52(self, base):
        x = P.INNER[1] + 1.0  # inside the wall, behind the knock-out
        zc = board_z(P.SMA_CASE_Z)
        assert not solid_bz(base, x, P.SMA_Y, zc)
        assert not solid_bz(base, x, P.SMA_Y, zc - 3.1)
        assert not solid_bz(base, x, P.SMA_Y, zc + 2.5)
        assert solid_bz(base, x, P.SMA_Y, zc + 2.8)  # the flat
        assert solid_bz(base, x, P.SMA_Y + 3.5, zc)

    def test_makes_the_whole_sma_side_wall_4_mm_thick(self, base):
        x = P.INNER[1] + P.SMA_WALL - 0.2
        for y in (5.0, 35.0, 65.0):
            assert solid_bz(base, x, y, 10.0)
        assert not solid_bz(base, P.INNER[1] + P.SMA_WALL + 0.1, 35.0, 10.0)

    def test_closes_the_sma_hole_with_a_one_line_knock_out(self, base):
        zc = board_z(P.SMA_CASE_Z)
        assert solid_bz(base, P.OUTER[1] - P.SMA_KNOCKOUT_T / 2, P.SMA_Y, zc)
        assert not solid_bz(base, P.OUTER[1] - P.SMA_KNOCKOUT_T - 0.1, P.SMA_Y, zc)

    def test_has_1_mm_ribbon_plug_recesses_leaving_1_mm_of_wall(self, base):
        z = 8.0  # board Z, within the plug height
        assert not solid_bz(base, P.INNER[0] - 0.5, 63.0, z)
        assert solid_bz(base, P.INNER[0] - 1.5, 63.0, z)
        assert not solid_bz(base, P.INNER[1] + 0.5, 63.0, z)
        assert not solid_bz(base, 27.5, P.INNER[3] + 0.5, z)
        assert solid_bz(base, 27.5, P.INNER[3] + 1.5, z)
        # no recess away from the header
        assert solid_bz(base, P.INNER[0] - 0.5, 40.0, z)
        top = P.RIBBON_RECESS_TOP
        assert not solid_bz(base, 27.5, P.INNER[3] + 0.3, top + 0.5)  # sloped ceiling
        assert solid_bz(base, 27.5, P.INNER[3] + 0.9, top + 0.5)

    def test_keeps_the_header_end_outer_corners_closed(self, base):
        ox0, ox1, _, oy1 = P.OUTER
        r = P.OUTER_R
        d = r * (1 - 1 / 2**0.5)  # outer surface on the corner diagonal
        for cx, sx in ((ox0, 1), (ox1, -1)):
            x, y = cx + sx * (d + 0.5), oy1 - (d + 0.5)
            for z in (2.0, 8.0, 12.0):
                assert solid_bz(base, x, y, z), (x, y, z)

    def test_has_four_tab_dents_level_with_the_bumps(self, base):
        z = P.BUMP_Z
        for y in (
            P.INNER[2] + P.TAB_CLEAR + P.TAB_LEN / 2,
            P.INNER[3] - P.TAB_CLEAR - P.TAB_LEN / 2,
        ):
            for x, d in ((P.INNER[0], -1), (P.INNER[1], 1)):
                assert not solid(base, x + d * 0.3, y, z)  # dent
                assert solid(base, x + d * 0.3, y, z - 1.5)  # wall below the dent

    def test_has_four_6_mm_standoffs_5_mm_tall_on_the_hole_axes(self, base):
        for hx, hy in P.SCREW_HOLES:
            assert solid(base, hx + 2.7, hy, 6.5)
            assert not solid(base, hx + 3.3, hy, 6.5)
            assert not solid(base, hx + 2.7, hy, 7.2)

    def test_has_a_countersunk_through_hole_under_each_standoff(self, base):
        for hx, hy in P.SCREW_HOLES:
            for cz in (0.05, 1.0, 4.0, 6.9):
                assert not solid(base, hx, hy, cz)
            assert not solid(base, hx + 2.3, hy, 0.25)  # countersink near the surface
            assert solid(base, hx + 1.6, hy, 1.8)  # plain bore deeper in

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
