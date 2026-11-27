"""p4_case.board (BDD: p4_case.board). Needs the vendor STEP: make case-board."""

import math
from dataclasses import replace

import pytest
from p4_case.board import FACTS, HOLES, BBox, load_board, measure
from p4_case.board import check_facts as run_check_facts


class TestLoadBoardVendorStep:
    """board.load_board: On the vendor STEP"""

    def test_returns_a_board_model_with_the_named_parts(self, board):
        for label in ("USB_TPYPE-000", "RJ45_HR911105A", "SD1_ASM", "LED1_ASM"):
            assert label in board.parts

    def test_puts_the_pcb_top_at_z0_and_the_outline_at_x0_55_y0_68(self, board):
        pcb = board.pcb
        assert pcb.z1 == pytest.approx(0.0, abs=0.05)
        assert pcb.z0 == pytest.approx(-1.6, abs=0.05)
        assert (pcb.x0, pcb.x1) == pytest.approx((0.0, 55.0), abs=0.05)
        assert (pcb.y0, pcb.y1) == pytest.approx((0.0, 68.0), abs=0.05)


class TestLoadBoardMissingFile:
    """board.load_board: On a missing file"""

    def test_raises_file_not_found_naming_make_case_board(self, tmp_path):
        with pytest.raises(FileNotFoundError, match="make case-board"):
            load_board(tmp_path / "nope.stp")


class TestMeasureVendorStep:
    """board.measure: On the vendor STEP"""

    NAMES = (
        "usb",
        "rj45",
        "sd",
        "button_a",
        "button_b",
        "led",
        "header",
        "transformer",
        "caps_top",
    )

    def test_returns_boxes_for_the_spec_parts(self, board):
        facts = measure(board)
        for name in self.NAMES:
            assert isinstance(facts[name], BBox), name

    def test_returns_the_four_mounting_hole_centres(self, board):
        holes = measure(board)["holes"]
        assert len(holes) == 4
        for got, want in zip(sorted(holes), sorted(HOLES), strict=True):
            assert math.dist(got, want) < 0.05


class TestCheckFactsMatching:
    """board.check_facts: When the constants match the model"""

    def test_returns_only_passing_results_one_per_fact(self, board):
        results = run_check_facts(board)
        assert len(results) == len(FACTS) + 2  # + capacitor height and mounting holes
        assert all(r.ok for r in results), [r for r in results if not r.ok]


class TestCheckFactsOff:
    """board.check_facts: When a constant is off by more than 0.05 mm"""

    def test_returns_a_failing_result_naming_that_fact_and_both_values(self, board):
        bad = dict(FACTS)
        bad["usb"] = replace(FACTS["usb"], x0=FACTS["usb"].x0 + 0.2)
        results = run_check_facts(board, expected=bad)
        failed = [r for r in results if not r.ok]
        assert [r.name for r in failed] == ["board fact usb"]
        assert f"{bad['usb'].x0:.2f}" in failed[0].detail
        assert f"{FACTS['usb'].x0:.2f}" in failed[0].detail


class TestColumnEnvelopeVendorStep:
    """board.ColumnEnvelope: On the vendor STEP"""

    def test_top_at_the_usb_stack_is_16(self, board):
        usb = (4.26, 19.93, -1.7, 15.76)
        assert board.envelope.region_max_top(usb) == pytest.approx(16.0, abs=0.1)

    def test_bottom_under_the_2_pin_socket_is_minus_4_9(self, board):
        assert board.envelope.region_min_bottom((26, 31, 55, 59)) == pytest.approx(
            -4.9, abs=0.1
        )

    def test_is_nan_outside_the_board_outline(self, board):
        assert math.isnan(board.envelope.top(30.0, 75.0))
        assert math.isnan(board.envelope.bottom(-3.0, 30.0))


class TestFreeHeightAbovePlate:
    """board.ColumnEnvelope.free_height_above_plate"""

    PLATE_TOP = -6.6  # case Z 2 in the board frame

    def test_is_at_least_4_5_at_keyhole_k1(self, board):
        region = (6.0, 20.75, 38.0, 52.75)
        assert board.envelope.free_height_above_plate(region, self.PLATE_TOP) >= 4.5

    def test_is_at_least_3_0_at_keyhole_k2(self, board):
        region = (38.0, 52.75, 38.0, 52.75)
        assert board.envelope.free_height_above_plate(region, self.PLATE_TOP) >= 3.0
