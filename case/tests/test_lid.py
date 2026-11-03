"""p4_case.lid.build_lid (BDD: p4_case.lid.build_lid). Geometry is probed with points in the board frame."""

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
        assert solid(lid, edge + 0.05, 34, FLANGE_Z)
        assert not solid(lid, edge - 0.05, 34, FLANGE_Z)
        assert not solid(lid, edge + 0.05, 34, BODY_Z)  # body is narrower
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
        top = P.LID_UNDERSIDE - P.TAB_FILLET - 0.2
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

    def test_has_a_root_fillet_on_each_tab(self, lid):
        x = TAB_X[-1] + P.TAB_ROOT_T + 0.3
        y = TAB_Y0 + 5.0
        assert solid(lid, x, y, P.LID_UNDERSIDE - 0.1)
        assert not solid(lid, x + P.TAB_FILLET, y, P.LID_UNDERSIDE - 0.1)

    def test_has_a_detent_bump_on_each_side_wall_face(self, lid):
        for yc in tab_y_centres():
            for side in (-1, 1):
                x = TAB_X[side] + side * (P.BUMP_H - 0.1)
                assert solid(lid, x, yc, P.BUMP_Z)
                assert not solid(lid, x, yc, P.BUMP_Z + 1.0)


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
