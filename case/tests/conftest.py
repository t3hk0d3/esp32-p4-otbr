"""Shared fixtures: the vendor board model and the two printed parts, built once per session."""

from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
BOARD_STEP = REPO / ".artifacts" / "board" / "board.stp"


@pytest.fixture(scope="session")
def board():
    from p4_case.board import load_board

    return load_board(BOARD_STEP)


@pytest.fixture(scope="session")
def base():
    from p4_case.base import build_base

    return build_base()


@pytest.fixture(scope="session")
def lid():
    from p4_case.lid import build_lid

    return build_lid()


@pytest.fixture(scope="session")
def lid_open():
    """The default lid with all four curtain segments broken out."""
    from p4_case.lid import build_lid

    return build_lid(curtain=False)
