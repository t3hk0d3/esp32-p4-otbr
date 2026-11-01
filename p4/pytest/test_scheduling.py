"""scheduling (spec 5.11; BDD: Task 18). Hardware-gated: written with Task 18 as the contract, deferred to V3.

Reads /device/diag tasks[].core (a number for pinned tasks, "any" for unpinned ones) and compares it with the
placement table in core_map.py. Tasks missing from the table are ignored.
"""

import time

import pytest
import requests
from core_map import ALWAYS_PRESENT, MANAGEMENT_TASKS, NETWORK_TASKS

SETTLE_TIMEOUT_S = (
    240  # ot_main appears only after the radio sync (up to 180 s in br_core)
)
POLL_S = 2


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


@pytest.fixture
def task_cores(base_url):
    """{task name: core} from /device/diag, polled until every always-present task exists.

    GET /node/state creates the lazily started thread_rest worker once OpenThread is ready (503 before). c6_log needs
    the maintenance jumper: when /device/status says it is absent the task is not awaited. Tasks that live only as long
    as a job (pcap_stream, ota_http, radio_fw, rom_recover) are covered by the code constant WORKER_CORE only."""
    status = requests.get(f"{base_url}/device/status", timeout=10).json()
    wanted = (ALWAYS_PRESENT | {"thread_rest"}) - (
        set() if status["jumper"]["present"] else {"c6_log"}
    )
    deadline = time.monotonic() + SETTLE_TIMEOUT_S
    cores = {}
    while time.monotonic() < deadline:
        requests.get(f"{base_url}/node/state", timeout=10)
        response = requests.get(f"{base_url}/device/diag", timeout=10)
        assert response.status_code == requests.codes.ok
        cores = {task["name"]: task["core"] for task in response.json()["tasks"]}
        if wanted <= set(cores):
            return cores
        time.sleep(POLL_S)
    pytest.fail(
        f"tasks still missing after {SETTLE_TIMEOUT_S} s: {sorted(wanted - set(cores))}"
    )


def mismatches(expected, task_cores):
    """Entries of `expected` that /device/diag lists on a different core (absent tasks are covered by task_cores)."""
    return {
        name: (core, task_cores[name])
        for name, core in expected.items()
        if name in task_cores and task_cores[name] != core
    }


@pytest.mark.jumper
class TestScheduling:
    """scheduling.startup"""

    def test_network_tasks_report_core_0_on_system_start(self, task_cores):
        # On system started: tcpip, EMAC RX, OpenThread, event loop and mDNS tasks report core 0.
        assert mismatches(NETWORK_TASKS, task_cores) == {}

    def test_management_tasks_report_core_1_on_system_start(self, task_cores):
        # On system started: HTTP server, workers, rcp_sync, log receiver and sampler report core 1.
        assert mismatches(MANAGEMENT_TASKS, task_cores) == {}
