"""metrics: GET /metrics (spec docs/superpowers/specs/2026-11-02-prometheus-metrics-design.md). Hardware-gated: runs in
the next batched on-board pass, not as part of the task that added it.
"""

import time

import pytest
import requests
from prometheus_client.parser import text_string_to_metric_families

CONTENT_TYPE = "text/plain; version=0.0.4; charset=utf-8"
COLLECT_WAIT_S = 12  # one collector period (10 s) plus margin


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


def scrape(base_url):
    response = requests.get(f"{base_url}/metrics", timeout=10)
    assert response.status_code == requests.codes.ok
    assert response.headers["Content-Type"] == CONTENT_TYPE
    return {f.name: f for f in text_string_to_metric_families(response.text)}


class TestMetrics:
    """metrics GET /metrics"""

    def test_metrics_in_any_mode_parses_and_br_mode_has_one_series_at_1(self, base_url):
        families = scrape(base_url)
        assert [s.value for s in families["br_mode"].samples].count(1.0) == 1
        assert families["br_chip_temperature_celsius"].samples

    def test_metrics_in_br_running_with_a_network_has_fresh_thread_series(
        self, base_url
    ):
        time.sleep(COLLECT_WAIT_S)
        families = scrape(base_url)
        mode = {s.labels["mode"]: s.value for s in families["br_mode"].samples}
        if mode["BR_RUNNING"] != 1.0:
            pytest.skip("board is not in BR_RUNNING")
        assert "br_thread_role" in families
        assert "br_mac_tx_frames" in families
        assert families["br_thread_collect_age_seconds"].samples[0].value < 30
