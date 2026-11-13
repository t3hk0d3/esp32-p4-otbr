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
        from p4_case.base import tab_y_centres

        for x, y, z in (
            (P.OUTER[0] + 2.0, tab_y_centres()[0], P.TOP - 1.0),  # a tooth
            (1.0, 50.0, 27.0),
        ):
            assert solid(lid, x, y, z) == solid(lid_open, x, y, z)


class TestLidTopBevel:
    """lid.build_lid: Where the lid reaches the case's outer face"""

    def test_continues_the_cases_top_bevel(self, lid):
        c = P.OUTER_TOP_CHAMFER
        for x, y in (
            (P.OUTER[0] + 0.1, 4.7),  # a tooth
            (12.0, P.OUTER[2] + 0.1),  # cable-end lintel
            (27.5, P.OUTER[3] - 0.1),  # 40-pin fill
        ):
            assert not solid(lid, x, y, P.TOP - 0.05), (x, y)
            assert solid(lid, x, y, P.TOP - c - 0.1), (x, y)


class TestLidHeaderCorners:
    """lid.build_lid: At the header-end corners, where the base is square above the slot floor"""

    def test_squares_the_body_into_the_corner(self, lid):
        c = P.LID_CLEAR
        for x in (IX0 + c + 0.1, IX1 - c - 0.1):
            assert solid(lid, x, IY1 - c - 0.1, (P.LID_UNDERSIDE + P.REBATE_FLOOR) / 2)

    def test_squares_the_header_end_tabs_into_the_corner(self, lid):
        c = P.TAB_CLEAR
        for x in (IX0 + c + 0.1, IX1 - c - 0.1):
            assert solid(lid, x, IY1 - c - 0.1, P.LID_UNDERSIDE - 1.0)

    def test_keeps_the_cable_end_corners_round(self, lid):
        c = P.LID_CLEAR
        assert not solid(
            lid, IX0 + c + 0.1, IY0 + c + 0.1, (P.LID_UNDERSIDE + P.REBATE_FLOOR) / 2
        )

    def test_clears_the_square_base_corner(self, base, lid):
        from p4_case.checks import lid_seats

        assert lid_seats(base, lid).ok


class TestLidTeeth:
    """lid.build_lid: The keying teeth"""

    def test_has_two_teeth_over_each_side_wall_above_the_snap_tabs(self, lid):
        from p4_case.base import tab_y_centres

        half = P.TOOTH_LEN / 2
        bottom = P.TOP - P.TOOTH_DEPTH
        for yc in tab_y_centres():
            for side in (-1, 1):
                face, inward = _wall(side)
                for u in (0.4, 2.0, 3.7):  # flush with the outer face, across the wall
                    x = face + inward * u
                    assert solid(lid, x, yc, bottom + 0.1)
                    assert not solid(lid, x, yc, bottom - 0.1)
                    assert solid(lid, x, yc + half - 0.1, P.TOP - 0.3)
                    if u < 3.0:  # the inner 1 mm is the lid's flange anyway
                        assert not solid(lid, x, yc + half + 0.1, P.TOP - 0.3)

    def test_gives_each_tooth_a_45_degree_lead_in(self, lid):
        from p4_case.base import tab_y_centres

        half = P.TOOTH_LEN / 2
        bottom = P.TOP - P.TOOTH_DEPTH
        x = P.OUTER[0] + 2.0
        yc = tab_y_centres()[0]
        assert P.TOOTH_LEAD == pytest.approx(0.5)
        assert not solid(lid, x, yc + half - 0.1, bottom + 0.1)  # corner cut away
        assert solid(lid, x, yc + half - 0.1, bottom + P.TOOTH_LEAD + 0.1)

    def test_has_no_screw_ears(self, lid):
        face, inward = _wall(-1)
        assert not solid(lid, face + inward * 0.8, 32.0, 22.3)
        assert not solid(lid, P.INNER[0] + 2.5, 32.0, 20.0)
