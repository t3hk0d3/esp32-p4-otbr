"""GET /metrics body: the host test's full sample render parses with prometheus_client and matches the catalogue.

The sample is written by p4/components/metrics/test_host (make test-host); make test runs that first.
"""

from pathlib import Path

import pytest

parser = pytest.importorskip("prometheus_client.parser")

ROOT = Path(__file__).resolve().parent.parent.parent
SAMPLE = ROOT / "p4" / "components" / "metrics" / "test_host" / "build" / "sample.prom"

# Spec section 4 (br_jumper_present and the NAT64 families dropped). prometheus_client names a counter family
# without its _total suffix.
CATALOGUE = {
    "br_info": "gauge",
    "br_uptime_seconds": "gauge",
    "br_reset_reason": "gauge",
    "br_mode": "gauge",
    "br_cpu_seconds": "counter",
    "br_task_cpu_seconds": "counter",
    "br_task_stack_free_min_bytes": "gauge",
    "br_tasks_untracked": "gauge",
    "br_heap_free_bytes": "gauge",
    "br_heap_min_free_bytes": "gauge",
    "br_heap_largest_free_block_bytes": "gauge",
    "br_ot_lock_acquisitions": "counter",
    "br_ot_lock_hold_seconds": "counter",
    "br_ot_lock_wait_seconds": "counter",
    "br_ot_lock_hold_max_seconds": "gauge",
    "br_ot_lock_wait_max_seconds": "gauge",
    "br_storage_ok": "gauge",
    "br_sniffer_active": "gauge",
    "br_sniffer_frames": "counter",
    "br_sniffer_drops": "counter",
    "br_eth_up": "gauge",
    "br_network_trial_pending": "gauge",
    "br_scrape_partial": "gauge",
    "br_thread_role": "gauge",
    "br_thread_role_entered": "counter",
    "br_thread_partition_changes": "counter",
    "br_thread_attach_attempts": "counter",
    "br_thread_routers": "gauge",
    "br_thread_children": "gauge",
    "br_thread_neighbors": "gauge",
    "br_mac_tx_frames": "counter",
    "br_mac_tx_retries": "counter",
    "br_mac_tx_errors": "counter",
    "br_mac_rx_frames": "counter",
    "br_mac_rx_duplicated": "counter",
    "br_mac_rx_errors": "counter",
    "br_thread_ip6_packets": "counter",
    "br_border_routing_state": "gauge",
    "br_dhcp6_pd_state": "gauge",
    "br_border_routing_packets": "counter",
    "br_border_routing_bytes": "counter",
    "br_border_routing_ra": "counter",
    "br_border_routing_rs": "counter",
    "br_srp_server_state": "gauge",
    "br_srp_hosts": "gauge",
    "br_srp_services": "gauge",
    "br_trel_enabled": "gauge",
    "br_trel_active": "gauge",
    "br_trel_peers": "gauge",
    "br_trel_packets": "counter",
    "br_trel_bytes": "counter",
    "br_trel_tx_failures": "counter",
    "br_thread_collect_age_seconds": "gauge",
    "br_thread_collect_failures": "counter",
}


@pytest.fixture(scope="module")
def families():
    if not SAMPLE.exists():
        pytest.fail(f"{SAMPLE} is missing: run make test-host first")
    return list(parser.text_string_to_metric_families(SAMPLE.read_text()))


class TestMetricsFormat:
    """metrics_render output, read by the reference parser"""

    def test_full_render_has_exactly_the_catalogue_families_and_types(self, families):
        assert {f.name: f.type for f in families} == CATALOGUE

    def test_full_render_has_no_two_samples_with_the_same_name_and_labels(
        self, families
    ):
        keys = [
            (s.name, tuple(sorted(s.labels.items())))
            for f in families
            for s in f.samples
        ]
        assert len(keys) == len(set(keys))

    def test_state_sets_have_exactly_one_series_at_1(self, families):
        by_name = {f.name: f for f in families}
        for name in (
            "br_mode",
            "br_thread_role",
            "br_border_routing_state",
            "br_dhcp6_pd_state",
            "br_srp_server_state",
        ):
            assert [s.value for s in by_name[name].samples].count(1.0) == 1, name
