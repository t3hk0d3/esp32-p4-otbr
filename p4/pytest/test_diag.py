"""diag: GET /device/diag (spec 5.12; BDD: Task 17). Hardware-gated: written with Task 17 as the contract,
deferred to V3 (never run in Task 17).

The load test uses the test-build console command `busy <core> <seconds>` (a task pinned to the core, 1..3 s).
"""

import time

import pytest
import requests

TOP_KEYS = {
    "uptime_s",
    "reset_reason",
    "chip_temperature_c",
    "cpu",
    "tasks",
    "tasks_untracked",
    "memory",
    "ot_lock",
}
WINDOWS = {"1s", "10s", "60s"}
HEAP_KEYS = {"free", "min_free", "largest_free_block"}
LOCK_KEYS = {"acquisitions", "max_hold_us", "max_wait_us", "longest_holder"}
BUSY_SECONDS = 3
LOAD_PCT_MIN = 80
POLL_S = 0.25


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


def diag(base_url):
    response = requests.get(f"{base_url}/device/diag", timeout=10)
    assert response.status_code == requests.codes.ok
    assert response.headers["Content-Type"].startswith("application/json")
    return response.json()


class TestDiag:
    """device_api GET /device/diag"""

    def test_diag_on_any_mode_returns_200_with_cpu_tasks_memory_and_ot_lock(
        self, base_url
    ):
        time.sleep(2)  # the first sample exists after one sampler tick
        body = diag(base_url)
        assert set(body) == TOP_KEYS
        assert body["uptime_s"] > 0
        assert body["reset_reason"]
        assert 20 <= body["chip_temperature_c"] <= 100
        assert [c["core"] for c in body["cpu"]] == [0, 1]
        for core in body["cpu"]:
            assert set(core["load_pct"]) == WINDOWS
            assert all(0 <= v <= 100 for v in core["load_pct"].values())
        names = {t["name"] for t in body["tasks"]}
        assert {"IDLE0", "IDLE1", "diag"} <= names
        for task in body["tasks"]:
            assert set(task) == {
                "name",
                "core",
                "priority",
                "state",
                "cpu_pct",
                "stack_free_min_bytes",
            }
            assert task["core"] in (0, 1, "any")
            assert set(task["cpu_pct"]) == WINDOWS
        for region in ("internal", "psram"):
            assert set(body["memory"][region]) == HEAP_KEYS
            assert body["memory"][region]["free"] > 0
        assert body["memory"]["psram"]["largest_free_block"] > 0
        assert set(body["ot_lock"]) == LOCK_KEYS | {"last_60s"}
        assert set(body["ot_lock"]["last_60s"]) == LOCK_KEYS

    def test_diag_sampler_task_runs_on_core_1_at_the_lowest_application_priority(
        self, base_url
    ):
        time.sleep(2)
        sampler = next(t for t in diag(base_url)["tasks"] if t["name"] == "diag")
        assert sampler["core"] == 1
        assert sampler["priority"] == 1

    def test_diag_busy_loop_on_core_1_raises_its_1s_load_above_80_percent(
        self, base_url, send
    ):
        time.sleep(2)
        send(f"busy 1 {BUSY_SECONDS}", rb"busy: started")
        peak = 0.0
        deadline = time.monotonic() + BUSY_SECONDS + 1
        while time.monotonic() < deadline:
            peak = max(peak, diag(base_url)["cpu"][1]["load_pct"]["1s"])
            time.sleep(POLL_S)
        assert peak > LOAD_PCT_MIN

    def test_diag_ot_lock_counts_acquisitions_after_rest_calls(
        self, stored_dataset, base_url
    ):
        before = diag(base_url)["ot_lock"]["acquisitions"]
        for _ in range(5):
            requests.get(f"{base_url}/node/state", timeout=10)
        lock = diag(base_url)["ot_lock"]
        assert lock["acquisitions"] > before
        assert lock["max_hold_us"] > 0
        assert lock["longest_holder"]
        assert lock["last_60s"]["acquisitions"] > 0
