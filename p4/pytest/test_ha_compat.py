"""Home Assistant compatibility (BDD: Task 21, HA compatibility): tools/otbr_compat.py against the board.

The driver runs every python-otbr-api call of HA's `otbr` integration, ends with the destructive `factory_reset`
and re-provisions the dataset it started with. Single network mode (REST writes allowed on the only address).
Optional split-mode checks run when the environment names the addresses (see TestSplitModePolicy).
"""

import os
import subprocess
import sys
from pathlib import Path

import pytest
import requests
from conftest import wait_ot_state

REPO = Path(__file__).resolve().parents[2]
DRIVER = REPO / "tools" / "otbr_compat.py"
DRIVER_TIMEOUT_S = 540
LEADER_TIMEOUT_S = 90


def run_driver(url, *extra):
    return subprocess.run(
        [sys.executable, str(DRIVER), "--url", url, *extra],
        capture_output=True,
        text=True,
        timeout=DRIVER_TIMEOUT_S,
        check=False,
    )


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


class TestEveryPythonOtbrApiCall:
    """tools/otbr_compat.py, single network mode"""

    def test_succeeds_against_the_board(self, dut, stored_dataset, base_url):
        # On every python-otbr-api call used by the HA otbr integration: succeeds against the board.
        wait_ot_state(dut, {"leader"}, LEADER_TIMEOUT_S)
        result = run_driver(base_url)
        assert result.returncode == 0, result.stdout + result.stderr
        assert result.stdout.rstrip().endswith("ALL OK")

    def test_leaves_the_stored_dataset_in_place(self, dut, stored_dataset, base_url):
        # The run restores the board: the dataset of the start is the active dataset afterwards and Thread runs.
        wait_ot_state(dut, {"leader"}, LEADER_TIMEOUT_S)
        result = run_driver(base_url)
        assert result.returncode == 0, result.stdout + result.stderr
        response = requests.get(
            f"{base_url}/node/dataset/active",
            headers={"Accept": "text/plain"},
            timeout=10,
        )
        assert response.text.strip().lower() == stored_dataset.lower()
        assert requests.get(f"{base_url}/node/state", timeout=10).json() in {
            "leader",
            "router",
            "child",
        }

    def test_unreachable_board_exits_non_zero_naming_the_call(self):
        # On the first call failing: exits non-zero and prints the failing call.
        result = run_driver("http://127.0.0.1:1")
        assert result.returncode != 0
        assert "FAIL get_api_version" in result.stdout


@pytest.mark.skipif(
    not os.environ.get("COMPAT_MGMT_URL"),
    reason="split-mode check needs COMPAT_MGMT_URL (management address of a board in split mode)",
)
class TestSplitModePolicy:
    """tools/otbr_compat.py --expect-writes-refused against the management address of a split-mode board"""

    def test_first_write_on_the_management_address_is_refused_with_403(self):
        result = run_driver(os.environ["COMPAT_MGMT_URL"], "--expect-writes-refused")
        assert result.returncode == 0, result.stdout + result.stderr
        assert "first write is refused with 403" in result.stdout


@pytest.mark.skipif(
    not os.environ.get("COMPAT_THREAD_BB_URL"),
    reason="split-mode check needs COMPAT_THREAD_BB_URL (thread_bb address of a board in split mode, reachable from the host)",
)
class TestSplitModeThreadBb:
    """tools/otbr_compat.py against the thread_bb address of a split-mode board"""

    def test_every_call_succeeds_on_thread_bb(self):
        result = run_driver(os.environ["COMPAT_THREAD_BB_URL"])
        assert result.returncode == 0, result.stdout + result.stderr
