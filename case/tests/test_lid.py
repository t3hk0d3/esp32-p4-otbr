"""p4_case.lid.build_lid (BDD: p4_case.lid.build_lid). Geometry is probed with points in the board frame."""

import itertools

import pytest
from build123d import Vector
from p4_case import params as P
from p4_case.base import tab_y_centres
from p4_case.params import board_z


def solid(part, x, y, case_z):
    return part.is_inside(Vector(x, y, board_z(case_z)))


IX0, IX1, IY0, IY1 = P.INNER
FLANGE_Z = (P.REBATE_FLOOR + P.TOP) / 2
BODY_Z = (P.LID_UNDERSIDE + P.REBATE_FLOOR) / 2
TAB_X = {
    -1: IX0 + P.TAB_CLEAR,
    1: IX1 - P.TAB_CLEAR,
}  # wall-facing face of the tabs, by side
TAB_Y0 = IY0 + P.TAB_CLEAR
TAB_Y1 = IY1 - P.TAB_CLEAR


class TestDefaultLid:
    """lid.build_lid: On default parameters"""

    def test_returns_one_valid_watertight_solid(self, lid):
        assert len(lid.solids()) == 1
        assert lid.is_valid

    def test_has_a_1_mm_flange_fitting_the_rebate_with_0_2_clearance(self, lid):
        edge = IX0 - P.REBATE_W + P.LID_CLEAR
        y = 50.0  # clear of the screw ear at Y 32
        assert solid(lid, edge + 0.05, y, FLANGE_Z)
        assert not solid(lid, edge - 0.05, y, FLANGE_Z)
        assert not solid(lid, edge + 0.05, y, BODY_Z)  # body is narrower
        assert lid.bounding_box().max.Z == pytest.approx(board_z(P.TOP), abs=0.01)

    def test_has_its_underside_at_case_z_25_6(self, lid):
        assert solid(lid, 27.5, 6.0, P.LID_UNDERSIDE + 0.05)
        assert not solid(lid, 27.5, 6.0, P.LID_UNDERSIDE - 0.05)

    def test_has_a_solid_frame_all_round(self, lid):
        edge = IX0 - P.REBATE_W + P.LID_CLEAR
        for y in (10.0, 30.0, 50.0):
            assert solid(lid, edge + P.FRAME_W - 0.3, y, BODY_Z)
        assert solid(
            lid, 1.0, 18.2, BODY_Z
        )  # no cells cut into the frame (over the LED)

    def test_has_only_openings_of_3_5_mm_or_less(self, lid):
        from p4_case.lid import opening_widths

        widths = opening_widths(lid)
        assert widths
        assert max(widths) <= P.CELL + 1e-6

    def test_has_the_cross_rib_near_y_34(self, lid):
        for x in (10.0, 20.0, 30.0, 40.0):
            assert solid(lid, x, P.RIB_Y, BODY_Z)

    def test_has_four_identical_corner_tabs_10_long_3_5_deep(self, lid):
        z_in = P.LID_UNDERSIDE - P.TAB_DROP + 0.3
        for side in (-1, 1):
            x = TAB_X[side] - side * 0.6
            for y in (TAB_Y0 + 3.5, TAB_Y1 - 3.5):
                assert solid(lid, x, y, z_in)
                assert not solid(lid, x, y, P.LID_UNDERSIDE - P.TAB_DROP - 0.1)
            assert not solid(lid, x, TAB_Y0 + P.TAB_LEN + 0.2, P.LID_UNDERSIDE - 1.0)
            assert not solid(lid, x, TAB_Y1 - P.TAB_LEN - 0.2, P.LID_UNDERSIDE - 1.0)

    def test_tapers_each_tab_inner_face_from_2_4_to_1_9(self, lid):
        y = TAB_Y0 + 5.0
        x = TAB_X[-1]
        top = P.LID_UNDERSIDE - P.TAB_GUSSET - 0.2
        assert solid(lid, x + 2.3, y, P.LID_UNDERSIDE - 0.05)  # near the root
        assert not solid(lid, x + 2.0, y, P.LID_UNDERSIDE - P.TAB_DROP + 0.05)
        assert solid(lid, x + 1.7, y, P.LID_UNDERSIDE - P.TAB_DROP + 0.05)
        assert top > P.LID_UNDERSIDE - P.TAB_DROP

    def test_keeps_wall_faces_flat_for_2_mm_then_leads_in_0_5(self, lid):
        x = TAB_X[-1]
        y = TAB_Y0 + 5.0
        assert solid(lid, x + 0.05, y, P.LID_UNDERSIDE - 1.9)
        assert not solid(lid, x + 0.3, y, P.LID_UNDERSIDE - P.TAB_DROP + 0.1)
        assert solid(lid, x + 0.6, y, P.LID_UNDERSIDE - P.TAB_DROP + 0.1)
        # end face toward the cable wall (probed clear of the rounded corner)
        assert solid(lid, x + 1.8, TAB_Y0 + 0.4, P.LID_UNDERSIDE - 1.9)
        assert not solid(lid, x + 1.8, TAB_Y0 + 0.3, P.LID_UNDERSIDE - P.TAB_DROP + 0.1)

    def test_has_a_45_degree_root_gusset_on_each_tab(self, lid):
        x = TAB_X[-1] + P.TAB_ROOT_T + 0.3
        y = TAB_Y0 + 5.0
        g = P.TAB_GUSSET
        assert g == pytest.approx(1.2)
        assert solid(lid, x, y, P.LID_UNDERSIDE - 0.1)
        assert solid(lid, x, y, P.LID_UNDERSIDE - (g - 0.3) + 0.1)  # under the slope
        assert not solid(lid, x, y, P.LID_UNDERSIDE - (g - 0.3) - 0.1)  # past it
        assert not solid(lid, x + g, y, P.LID_UNDERSIDE - 0.1)

    def test_has_a_detent_bump_on_each_side_wall_face(self, lid):
        for yc in tab_y_centres():
            for side in (-1, 1):
                x = TAB_X[side] + side * (P.BUMP_H - 0.1)
                assert solid(lid, x, yc, P.BUMP_Z)
                assert not solid(lid, x, yc, P.BUMP_Z + 1.0)

    def test_gives_each_bump_a_30_degree_lead_in_and_a_45_degree_hold(self, lid):
        assert P.BUMP_LEAD_ANGLE == pytest.approx(30.0)
        assert P.BUMP_HOLD_ANGLE == pytest.approx(45.0)
        yc = tab_y_centres()[0]
        x = TAB_X[-1] - 0.2  # 0.2 out from the tab face
        # 0.25 below the crest the 30° lead-in still reaches 0.26; 0.25 above, the 45° hold only 0.15
        assert solid(lid, x, yc, P.BUMP_Z - 0.25)
        assert not solid(lid, x, yc, P.BUMP_Z + 0.25)

    def test_puts_the_bump_crest_on_a_printed_layer_centre(self):
        layers = (P.TOP - P.BUMP_Z) / P.LAYER_H
        assert layers % 1 == pytest.approx(0.5)

    def test_chamfers_the_top_face_edge_against_elephants_foot(self, lid):
        edge = IX0 - P.REBATE_W + P.LID_CLEAR
        assert not solid(lid, edge + 0.1, 50, P.TOP - 0.1)
        assert solid(lid, edge + 0.1, 50, P.TOP - P.LID_TOP_CHAMFER - 0.1)


class TestLidOnBase:
    """lid.build_lid: Fitted on the default base"""

    def test_does_not_overlap_the_base(self, base, lid):
        from p4_case.checks import lid_seats

        r = lid_seats(base, lid)
        assert r.ok, r.detail


class TestLidLintel:
    """lid.build_lid: The lintel over the cable-wall opening"""

    def test_fills_the_cut_above_the_usb_flush_with_the_outer_face(self, lid):
        assert solid(lid, 12.0, P.OUTER[2] + 0.1, P.TOP - 0.5)
        assert solid(lid, 12.0, -1.5, P.LID_UNDERSIDE + 0.5)

    def test_reaches_down_to_1_mm_above_the_rj45(self, lid):
        bottom = P.RJ45_BOX.z1 + P.OPENING_TOP_MARGIN
        assert solid(lid, 30.0, -1.5, P.BOARD_Z_IN_CASE + bottom + 0.1)
        assert not solid(lid, 30.0, -1.5, P.BOARD_Z_IN_CASE + bottom - 0.1)

    def test_keeps_lid_clearance_to_the_opening_sides(self, lid):
        x0, x1 = P.CABLE_OPENING_X
        assert not solid(lid, x0 + 0.1, -1.5, P.TOP - 0.5)
        assert solid(lid, x0 + 0.3, -1.5, P.TOP - 0.5)
        assert not solid(lid, x1 - 0.1, -1.5, P.TOP - 0.5)


class TestLidGrilleCentred:
    """lid.cell_centres: On default parameters"""

    def test_is_symmetric_about_the_lid_centre(self):
        from p4_case.lid import cell_centres

        cells = cell_centres()
        cx = (P.INNER[0] + P.INNER[1]) / 2
        cy = (P.INNER[2] + P.INNER[3]) / 2
        keys = {(round(x, 3), round(y, 3)) for x, y in cells}
        for x, y in cells:
            assert (round(2 * cx - x, 3), round(y, 3)) in keys
            assert (round(x, 3), round(2 * cy - y, 3)) in keys

    def test_keeps_every_cell_out_of_the_frame(self):
        from p4_case.lid import CELL_R, cell_centres

        edge = P.INNER[0] - P.REBATE_W + P.LID_CLEAR + P.FRAME_W
        assert min(x for x, _ in cell_centres()) - CELL_R >= edge - 1e-6


def _wall(side):
    """(outer face X, inward direction) of the left (-1) or right (+1) side wall."""
    return (P.OUTER[0], 1) if side < 0 else (P.OUTER[1], -1)


SLOT_MID = (P.INNER[3] + P.OUTER[3]) / 2  # the header-end wall's centre line
FLOOR = P.HEADER_SLOT_BOTTOM


def segment_centres():
    x0, x1 = P.INNER[0] + P.LID_CLEAR, P.INNER[1] - P.LID_CLEAR
    n, slit = P.CURTAIN_SEGMENTS, P.CURTAIN_SLIT
    w = (x1 - x0 - (n - 1) * slit) / n
    return [x0 + w / 2 + i * (w + slit) for i in range(n)], w


class TestLidV6Thickness:
    """lid.build_lid: On default parameters (v6)"""

    def test_is_3_mm_thick_inside_the_walls_with_its_top_at_28_6(self, lid):
        assert P.TOP == pytest.approx(28.6)
        assert lid.bounding_box().max.Z == pytest.approx(board_z(P.TOP), abs=0.01)
        assert solid(lid, 1.0, 50.0, P.LID_UNDERSIDE + 0.1)  # frame, bottom
        assert solid(lid, 1.0, 50.0, P.TOP - 0.1)  # frame, top
        assert P.TOP - P.LID_UNDERSIDE == pytest.approx(3.0)


class TestLidOutsideEars:
    """lid.build_lid: On default parameters (outside ears)"""

    def test_fills_each_wall_top_cut_flush_with_the_outer_face(self, lid):
        half = P.EAR_W / 2
        for side in (-1, 1):
            face, inward = _wall(side)
            assert solid(lid, face + inward * 0.2, P.EAR_Y, P.TOP - 0.5)
            assert solid(lid, face + inward * 2.0, P.EAR_Y, P.LID_UNDERSIDE + 0.2)
            assert solid(lid, face + inward * 2.0, P.EAR_Y + half - 0.1, P.TOP - 0.5)
            # 0.2 mm clear of the cut's ends
            assert not solid(
                lid, face + inward * 2.0, P.EAR_Y + half + 0.1, P.TOP - 0.5
            )

    def test_hangs_a_1_6_mm_u_shaped_ear_flush_with_each_outer_face(self, lid):
        r = P.EAR_W / 2
        zc = P.EAR_SCREW_Z
        for side in (-1, 1):
            face, inward = _wall(side)
            x = face + inward * 0.8
            assert solid(lid, x, P.EAR_Y + 3.5, zc + 1.0)
            assert not solid(
                lid, face + inward * (P.EAR_T + 0.1), P.EAR_Y + 3.5, zc + 1.0
            )
            assert solid(lid, x, P.EAR_Y, zc - r + 0.2)  # half-circle bottom, R5
            assert not solid(lid, x, P.EAR_Y, zc - r - 0.2)
            assert not solid(lid, x, P.EAR_Y + r * 0.8, zc - r * 0.8)  # round
            assert not solid(lid, x, P.EAR_Y + r + 0.1, zc + 1.0)

    def test_gives_each_ear_a_2_9_hole_and_a_90_degree_countersink_to_5_0(self, lid):
        zc = P.EAR_SCREW_Z
        assert (P.EAR_CLEAR_D, P.EAR_CSK_D) == pytest.approx((2.9, 5.0))
        for side in (-1, 1):
            face, inward = _wall(side)
            x = face + inward * 1.4  # past the countersink
            assert not solid(lid, x, P.EAR_Y + P.EAR_CLEAR_D / 2 - 0.15, zc)
            assert solid(lid, x, P.EAR_Y + P.EAR_CLEAR_D / 2 + 0.2, zc)
            assert not solid(
                lid, face + inward * 0.1, P.EAR_Y + P.EAR_CSK_D / 2 - 0.2, zc
            )

    def test_puts_the_ear_hole_on_the_wall_pilots_axis(self, base, lid):
        for side in (-1, 1):
            face, inward = _wall(side)
            for u in (0.5, 1.2, 2.5, 3.5):
                x = face + inward * u
                assert not solid(lid, x, P.EAR_Y, P.EAR_SCREW_Z)
                assert not solid(base, x, P.EAR_Y, P.EAR_SCREW_Z)


class TestLidCurtain:
    """lid.build_lid: On default parameters (curtain on)"""

    def test_fills_the_header_slot_above_the_underside_flush_with_the_outer_face(
        self, lid
    ):
        assert solid(lid, 27.5, P.OUTER[3] - 0.1, P.TOP - 0.5)
        assert solid(lid, 27.5, SLOT_MID, P.LID_UNDERSIDE + 0.2)
        assert not solid(lid, P.INNER[0] + 0.1, SLOT_MID, P.LID_UNDERSIDE + 1.0)
        assert solid(lid, P.INNER[0] + 0.3, SLOT_MID, P.LID_UNDERSIDE + 1.0)

    def test_hangs_four_2_6_mm_segments_with_0_4_mm_slits(self, lid):
        centres, _w = segment_centres()
        z = (FLOOR + P.LID_UNDERSIDE) / 2
        assert P.CURTAIN_SEGMENTS == 4
        assert P.CURTAIN_T == pytest.approx(2.6)
        half = P.CURTAIN_T / 2
        for x in centres:
            assert solid(lid, x, SLOT_MID, z)
            assert solid(lid, x, SLOT_MID + half - 0.1, z)
            assert not solid(lid, x, SLOT_MID + half + 0.1, z)
        for a, b in itertools.pairwise(centres):
            assert not solid(lid, (a + b) / 2, SLOT_MID, z)  # slit

    def test_notches_each_segment_to_0_8_mm_at_the_underside(self, lid):
        centres, _ = segment_centres()
        assert P.CURTAIN_NECK == pytest.approx(0.8)
        for x in centres:
            assert solid(lid, x, SLOT_MID + 0.3, P.LID_UNDERSIDE - 0.05)
            assert not solid(lid, x, SLOT_MID + 0.5, P.LID_UNDERSIDE - 0.05)
            full = P.LID_UNDERSIDE - (P.CURTAIN_T - P.CURTAIN_NECK) / 2 - 0.1
            assert solid(lid, x, SLOT_MID + P.CURTAIN_T / 2 - 0.1, full)

    def test_puts_a_tongue_in_the_floor_groove_0_2_clear_of_the_floor(self, lid):
        centres, _ = segment_centres()
        tongue = P.CURTAIN_TONGUE_W / 2
        for x in centres:
            assert solid(lid, x, SLOT_MID, FLOOR - 0.3)  # tongue in the groove
            assert not solid(lid, x, SLOT_MID, FLOOR - P.SLOT_GROOVE_D + 0.1)
            assert not solid(lid, x, SLOT_MID + tongue + 0.1, FLOOR - 0.3)
            assert not solid(lid, x, SLOT_MID + 0.6, FLOOR + 0.1)  # 0.2 off the floor
            assert solid(lid, x, SLOT_MID + 0.6, FLOOR + 0.3)


class TestLidCurtainBrokenOut:
    """lid.build_lid: With curtain=False"""

    def test_has_no_material_in_the_slot_below_the_underside(self, lid_open):
        centres, _ = segment_centres()
        for x in centres:
            assert not solid(lid_open, x, SLOT_MID, (FLOOR + P.LID_UNDERSIDE) / 2)
            assert not solid(lid_open, x, SLOT_MID, FLOOR - 0.3)
        assert solid(lid_open, 27.5, SLOT_MID, P.LID_UNDERSIDE + 0.2)

    def test_is_otherwise_the_default_lid(self, lid, lid_open):
        for x, y, z in (
            (P.OUTER[0] + 0.8, P.EAR_Y + 3.5, P.EAR_SCREW_Z + 1),
            (1.0, 50.0, 27.0),
        ):
            assert solid(lid, x, y, z) == solid(lid_open, x, y, z)


class TestLidInsideEars:
    """lid.build_lid: With ears inside"""

    EX = P.INNER[0] + P.TAB_CLEAR  # the left block's wall-facing face

    def test_hangs_a_12_by_5_block_0_2_off_each_wall_down_to_18_5(self, lid_inside):
        half = P.IN_EAR_LEN / 2
        assert (P.IN_EAR_LEN, P.IN_EAR_T, P.IN_EAR_BOTTOM) == pytest.approx(
            (12.0, 5.0, 18.5)
        )
        for side in (-1, 1):
            face = P.INNER[0] if side < 0 else P.INNER[1]
            inward = -side
            x = face + inward * (P.TAB_CLEAR + 2.5)
            assert solid(lid_inside, x, P.EAR_Y, 20.0)
            assert solid(lid_inside, x, P.EAR_Y + half - 0.2, 20.0)
            assert not solid(lid_inside, x, P.EAR_Y + half + 0.2, 20.0)
            assert solid(lid_inside, x, P.EAR_Y + 3.0, P.IN_EAR_BOTTOM + 0.1)
            assert not solid(lid_inside, x, P.EAR_Y + 3.0, P.IN_EAR_BOTTOM - 0.1)
            assert not solid(lid_inside, face + inward * 0.1, P.EAR_Y + 3.0, 20.0)
            assert solid(lid_inside, face + inward * 0.3, P.EAR_Y + 3.0, 20.0)

    def test_buttresses_each_block_at_45_degrees_up_to_the_underside(self, lid_inside):
        for side in (-1, 1):
            face = P.INNER[0] if side < 0 else P.INNER[1]
            inward = -side
            back = face + inward * (P.TAB_CLEAR + P.IN_EAR_T)
            rise = 22.0 - P.IN_EAR_BOTTOM  # 3.5 up, so 3.5 in
            assert solid(lid_inside, back + inward * (rise - 0.3), P.EAR_Y + 3.0, 22.0)
            assert not solid(
                lid_inside, back + inward * (rise + 0.3), P.EAR_Y + 3.0, 22.0
            )

    def test_has_3_mm_45_degree_gussets_at_both_ends(self, lid_inside):
        half, g = P.IN_EAR_LEN / 2, P.IN_EAR_GUSSET
        assert g == pytest.approx(3.0)
        x = self.EX + 2.5
        for out in (-1, 1):
            y = P.EAR_Y + out * (half + g / 2)
            assert solid(lid_inside, x, y, P.LID_UNDERSIDE - g / 2 + 0.2)
            assert not solid(lid_inside, x, y, P.LID_UNDERSIDE - g / 2 - 0.2)

    def test_drills_a_2_4_blind_pilot_on_the_wall_holes_axis(
        self, base_inside, lid_inside
    ):
        zc, r = P.IN_EAR_SCREW_Z, P.IN_EAR_PILOT_D / 2
        stop = P.TAB_CLEAR + P.IN_EAR_T - P.IN_EAR_PILOT_STOP
        for side in (-1, 1):
            face = P.INNER[0] if side < 0 else P.INNER[1]
            inward = -side
            for u in (P.TAB_CLEAR + 0.3, stop - 0.2):
                assert not solid(lid_inside, face + inward * u, P.EAR_Y, zc)
                assert solid(lid_inside, face + inward * u, P.EAR_Y + r + 0.2, zc)
            assert solid(lid_inside, face + inward * (stop + 0.2), P.EAR_Y, zc)  # blind
            assert not solid(base_inside, face - inward * 1.0, P.EAR_Y, zc)  # wall hole

    def test_leaves_out_every_grille_cell_over_a_block(self, lid_inside):
        from p4_case.lid import CELL_R, cell_centres

        half = P.IN_EAR_LEN / 2 + P.IN_EAR_GUSSET
        reach = P.TAB_CLEAR + P.IN_EAR_T + (P.LID_UNDERSIDE - P.IN_EAR_BOTTOM)
        for cx, cy in cell_centres(ears="inside"):
            near_y = abs(cy - P.EAR_Y) < half + CELL_R
            near_x = (
                cx < P.INNER[0] + reach + CELL_R or cx > P.INNER[1] - reach - CELL_R
            )
            assert not (near_y and near_x), (cx, cy)
        assert solid(lid_inside, self.EX + P.IN_EAR_T + 2.0, P.EAR_Y, P.TOP - 0.1)

    def test_has_no_wall_top_fill_and_no_outside_ear(self, lid_inside):
        face, inward = _wall(-1)
        assert not solid(lid_inside, face + inward * 0.3, P.EAR_Y, P.TOP - 0.5)
        assert not solid(lid_inside, face + inward * 0.8, P.EAR_Y + 3.5, P.EAR_SCREW_Z)


class TestLidInsideOnBase:
    """lid.build_lid: With ears inside, fitted on the inside-ear base"""

    def test_does_not_overlap_the_base(self, base_inside, lid_inside):
        from p4_case.checks import lid_seats

        r = lid_seats(base_inside, lid_inside)
        assert r.ok, r.detail


class TestLidNoEars:
    """lid.build_lid: With no ears (snaps only)"""

    def test_has_neither_ear_nor_wall_top_fill(self, lid_none):
        face, inward = _wall(-1)
        assert not solid(lid_none, face + inward * 0.3, P.EAR_Y, P.TOP - 0.5)
        assert not solid(lid_none, face + inward * 0.8, P.EAR_Y + 3.5, P.EAR_SCREW_Z)
        assert not solid(lid_none, P.INNER[0] + 2.5, P.EAR_Y, 20.0)  # no inside block

    def test_keeps_the_corner_tabs_bumps_and_curtain(self, lid, lid_none):
        segment = segment_centres()[0][0]
        for x, y, z in (
            (TAB_X[-1] + 0.6, TAB_Y0 + 3.5, P.LID_UNDERSIDE - 1.0),
            (segment, SLOT_MID, (FLOOR + P.LID_UNDERSIDE) / 2),
        ):
            assert solid(lid, x, y, z) and solid(lid_none, x, y, z)

    def test_keeps_the_default_grille(self):
        from p4_case.lid import cell_centres

        assert cell_centres(ears="none") == cell_centres()

    def test_fits_the_no_ear_base(self, base_none, lid_none):
        from p4_case.checks import lid_seats

        r = lid_seats(base_none, lid_none)
        assert r.ok, r.detail


class TestLidTopBevel:
    """lid.build_lid: Where the lid reaches the case's outer face"""

    def test_continues_the_cases_top_bevel(self, lid):
        c = P.OUTER_TOP_CHAMFER
        for x, y in (
            (P.OUTER[0] + 0.1, P.EAR_Y),  # outside ear's fill
            (12.0, P.OUTER[2] + 0.1),  # cable-end lintel
            (27.5, P.OUTER[3] - 0.1),  # 40-pin fill
        ):
            assert not solid(lid, x, y, P.TOP - 0.05), (x, y)
            assert solid(lid, x, y, P.TOP - c - 0.1), (x, y)
