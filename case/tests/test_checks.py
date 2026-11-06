"""p4_case.checks (BDD: p4_case.checks). Passing cases use the real parts; failing cases use small fixtures."""

import pytest
from build123d import Align, Box, Pos
from p4_case import checks
from p4_case import params as P
from p4_case.board import FACTS
from p4_case.params import board_z


def box(x0, x1, y0, y1, z0, z1):
    return Pos(x0, y0, z0) * Box(
        x1 - x0, y1 - y0, z1 - z0, align=(Align.MIN, Align.MIN, Align.MIN)
    )


@pytest.fixture(scope="module")
def results(board, base, lid):
    return {r.name: r for r in checks.run_all(board, base, lid)}


class TestBoardClearanceDefault:
    """checks.board_clearance: With the board in place and the default base or lid"""

    def test_passes_with_0_3_mm_clearance_outside_allowed_contacts(self, results):
        for name in ("base clears the board", "lid clears the board"):
            assert results[name].ok, results[name].detail


class TestBoardClearanceIntruding:
    """checks.board_clearance: With a part pushed 1 mm into the USB stack"""

    def test_fails_and_reports_the_location_and_overlap(self, board):
        usb = FACTS["usb"]
        intruder = box(usb.x0 + 2, usb.x0 + 6, 5, 9, usb.z1 - 1, usb.z1 + 2)
        r = checks.board_clearance(board, intruder, "fixture")
        assert not r.ok
        assert "too close" in r.detail and "at (" in r.detail
        assert "nan" not in r.detail  # the reported point is a real violation


class TestOpeningPathsDefault:
    """checks.opening_paths: On the default base"""

    def test_passes_for_every_opening(self, results):
        r = results["every opening has a clear path"]
        assert r.ok, r.detail


class TestOpeningPathsBlocked:
    """checks.opening_paths: With an opening blocked"""

    def test_fails_naming_the_opening(self, base, lid):
        plug = box(
            39.0, 52.0, P.OUTER[2] - 0.5, P.INNER[2], -1.0, 2.5
        )  # fills the microSD slot
        r = checks.opening_paths(base + plug, lid)
        assert not r.ok
        assert "microSD" in r.detail


class TestFittingMotionDefault:
    """checks.fitting_motion: On the default base"""

    def test_passes_and_reports_the_drop_offset(self, results):
        r = results["board fits in"]
        assert r.ok, r.detail
        assert "drop in" in r.detail


class TestFittingMotionNoTopMargin:
    """checks.fitting_motion: With the USB opening's top cut 0.3 mm into the connector"""

    def test_fails_and_reports_where(self, base):
        usb = FACTS["usb"]
        lintel = box(
            usb.x0 - 1, usb.x1 + 1, P.OUTER[2], P.INNER[2], usb.z1 - 0.3, usb.z1 + 1.2
        )
        r = checks.fitting_motion(base + lintel)
        assert not r.ok
        assert "collides" in r.detail or "hits" in r.detail


class TestLidClearance:
    """checks.lid_clearance: On the default lid with board, SMA envelope, ribbon plug and ribbon"""

    def test_passes(self, results):
        r = results["lid clears SMA, ribbon plug and ribbon"]
        assert r.ok, r.detail


class TestRibbonPlugHeadroom:
    """checks.ribbon_plug_headroom: On default parameters"""

    def test_leaves_1_3_mm_under_the_lid(self, results):
        r = results["ribbon plug has headroom"]
        assert r.ok, r.detail


class TestKeyholeVolumes:
    """checks.keyhole_volumes: On the default base"""

    def test_passes(self, results):
        r = results["keyhole screw heads fit"]
        assert r.ok, r.detail


class TestNutCircles:
    """checks.nut_circles: On the vendor board"""

    def test_reports_each_hole_and_passes_only_if_all_are_free(self, board, results):
        r = results["nut circles are free"]
        assert r.ok, r.detail
        assert "four" in r.detail


class TestOpeningSizeDefault:
    """checks.opening_size: On the default lid and base"""

    def test_passes(self, results):
        r = results["no opening wider than 3.5 mm"]
        assert r.ok, r.detail


class TestOpeningSizeWideCell:
    """checks.opening_size: With a 4 mm cell"""

    def test_fails_naming_the_widest_opening(self, base, lid):
        hole = Pos(2.0, P.RIB_Y, board_z(P.LID_UNDERSIDE) - 1) * Box(
            4.0, 4.0, 5.0, align=(Align.CENTER, Align.CENTER, Align.MIN)
        )
        r = checks.opening_size(base, lid - hole)
        assert not r.ok
        assert "widest lid opening" in r.detail


class TestWatertight:
    """checks.watertight: On both parts"""

    def test_passes_for_valid_closed_solids(self, results):
        assert results["base is one closed solid"].ok
        assert results["lid is one closed solid"].ok


class TestPrintabilityDefault:
    """checks.printability: On the base (plate down) and the lid (top down)"""

    def test_passes(self, results):
        for name in ("base prints without supports", "lid prints without supports"):
            assert results[name].ok, results[name].detail


class TestPrintabilityLedge:
    """checks.printability: With a 20 mm unsupported ledge"""

    def test_fails_with_the_ledge_location_and_span(self):
        tower = box(0, 5, 0, 5, 0, 10)
        ledge = box(5, 25, 0, 5, 8, 10)
        r = checks.printability(tower + ledge, "fixture", top_down=False)
        assert not r.ok
        assert "20.0" in r.detail


class TestRunAll:
    """checks.run_all"""

    def test_returns_one_result_per_check_plus_the_board_facts(self, results):
        from p4_case.board import FACTS as F

        board_facts = [n for n in results if n.startswith("board fact")]
        assert len(board_facts) == len(F) + 2
        assert len(results) - len(board_facts) == 18


class TestOpeningSizeTallSlot:
    """checks.opening_size: With the header slot floor 1 mm lower"""

    def test_fails_on_the_header_slot_passage(self, base, lid):
        x0, x1 = P.HEADER_SLOT_X
        floor = board_z(P.HEADER_SLOT_BOTTOM)
        deeper = box(
            x0, x1, P.INNER[3] - 0.01, P.OUTER[3] + 1, floor - 1.0, floor + 0.01
        )
        r = checks.opening_size(base - deeper, lid)
        assert not r.ok
        assert "header-slot passage" in r.detail


class TestLidSeats:
    """checks.lid_seats"""

    def test_passes_for_the_default_parts(self, results):
        r = results["lid seats on the base"]
        assert r.ok, r.detail

    def test_fails_when_the_lid_is_shifted_into_a_wall(self, base, lid):
        r = checks.lid_seats(base, Pos(0.5, 0, 0) * lid)
        assert not r.ok
        assert "overlap" in r.detail


class TestPrintabilityCantilever:
    """checks.printability: With a 10 x 10 mm cantilevered roof"""

    def test_fails(self):
        tower = box(0, 5, 0, 10, 0, 10)
        roof = box(5, 15, 0, 10, 8, 10)
        r = checks.printability(tower + roof, "fixture", top_down=False)
        assert not r.ok


class TestDetentEngagementDefault:
    """checks.detent_engagement: On the default lid and base"""

    def test_passes_and_reports_reach_and_preload(self, results):
        r = results["lid snaps engage the walls"]
        assert r.ok, r.detail
        assert "past the wall" in r.detail and "preload" in r.detail


class TestDetentEngagementNoBumps:
    """checks.detent_engagement: With the bumps trimmed back to the tab faces"""

    def test_fails_reporting_the_reach(self, base, lid):
        x0, x1, _, _ = P.INNER
        z0, z1 = board_z(P.BUMP_Z) - 2, board_z(P.BUMP_Z) + 2
        trim = box(x0 - 1, x0 + P.TAB_CLEAR, -5, 80, z0, z1) + box(
            x1 - P.TAB_CLEAR, x1 + 1, -5, 80, z0, z1
        )
        r = checks.detent_engagement(base, lid - trim)
        assert not r.ok
        assert "past the wall" in r.detail


class TestLidSeatsPreload:
    """checks.lid_seats: With the bumps preloaded into their dents"""

    def test_ignores_the_designed_bump_overlap(self, base, lid):
        assert checks._intersection(base, lid) > checks.BOOL_EPS  # the preload
        assert checks.lid_seats(base, lid).ok


class TestSmaRoom:
    """checks.sma_room: On the vendor board"""

    def test_passes_for_the_default_envelope(self, results):
        r = results["SMA nut clears the board"]
        assert r.ok, r.detail

    def test_fails_when_the_envelope_reaches_the_board(self, board):
        r = checks.sma_room(board, radius=12.0)
        assert not r.ok
        assert "board column" in r.detail


class TestMinPrintWidthDefault:
    """checks.min_print_width: On the base (plate down) and the lid (top down)"""

    def test_passes(self, results):
        for name in (
            "base has no strand under 0.8 mm",
            "lid has no strand under 0.8 mm",
        ):
            assert results[name].ok, results[name].detail


class TestMinPrintWidthStrand:
    """checks.min_print_width: With two blocks joined by a 0.45 mm wall"""

    def test_fails_with_the_strand_location(self):
        part = (
            box(0, 5, 0, 5, 0, 5) + box(10, 15, 0, 5, 0, 5) + box(5, 10, 2, 2.45, 0, 5)
        )
        r = checks.min_print_width(part, "fixture", top_down=False)
        assert not r.ok
        assert "at (" in r.detail


class TestMinPrintWidthBump:
    """checks.min_print_width: With a 0.4 mm bump on a thick wall"""

    def test_passes(self):
        part = box(0, 10, 0, 5, 0, 5) + box(3, 7, 5, 5.4, 0, 5)
        r = checks.min_print_width(part, "fixture", top_down=False)
        assert r.ok, r.detail
