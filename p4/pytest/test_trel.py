"""TREL switch on the board (docs/superpowers/specs/2026-11-03-trel-design.md). Hardware-gated: written with the feature,
run in the batched hardware pass. The JSON, config and isolation logic is host-tested (netcfg, device_api, metrics).

Manual check list (needs a second TREL border router of the same Thread network on the backbone link, e.g. an
ot-br-posix built with OTBR_TREL or another ESP Thread BR):
  1. `avahi-browse -rt _trel._udp` lists this board (TXT xa, xp) while Thread runs.
  2. Ethernet page, "Thread over Ethernet" card: Active, peers 1 or more, the peer table shows the other BR.
     `ot trel peers` on the console agrees; `ot neighbor table` lists the other BR.
  3. Off: the card shows Off within a poll; the other BR loses the TREL peer; Thread devices stay reachable. On again.
  4. Split mode: from the mgmt VLAN, `nc -u <mgmt address> 12390` traffic never reaches OpenThread (the rx counter
     does not move).
  5. Sniffer start and stop: TREL is active again afterwards.
"""

import pytest
import requests
from conftest import BOOT_TIMEOUT_S

REQUEST_TIMEOUT_S = 10
THREAD_UP = rb"br_core: Thread started with the stored dataset"


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


def status(base_url):
    reply = requests.get(f"{base_url}/device/status", timeout=REQUEST_TIMEOUT_S)
    reply.raise_for_status()
    return reply.json()["trel"]


def active_config(base_url):
    reply = requests.get(f"{base_url}/device/network", timeout=REQUEST_TIMEOUT_S)
    reply.raise_for_status()
    return reply.json()["active"]


def put_trel(base_url, enabled):
    config = active_config(base_url)
    config["trel"] = enabled
    return requests.put(
        f"{base_url}/device/network", json=config, timeout=REQUEST_TIMEOUT_S
    )


def test_status_carries_trel(base_url):
    trel = status(base_url)
    assert trel is not None
    assert set(trel) >= {
        "enabled",
        "active",
        "peers",
        "tx_packets",
        "tx_failures",
        "rx_packets",
        "peer_list",
    }


def test_network_config_carries_trel(base_url):
    assert isinstance(active_config(base_url)["trel"], bool)


def test_trel_switches_live_without_a_reboot(dut, stored_dataset, base_url):
    dut.expect(THREAD_UP, timeout=BOOT_TIMEOUT_S)
    try:
        reply = put_trel(base_url, False)
        assert reply.status_code == 200, reply.text
        assert reply.json()["status"] in ("applied", "saved")
        assert active_config(base_url)["trel"] is False
        trel = status(base_url)
        assert trel["enabled"] is False
        assert trel["active"] in (False, None)
        assert requests.get(f"{base_url}/device/network", timeout=REQUEST_TIMEOUT_S).json()["trial"] is None
    finally:
        reply = put_trel(base_url, True)
    assert reply.status_code == 200, reply.text
    trel = status(base_url)
    assert trel["enabled"] is True
    assert trel["active"] in (True, None)


def test_trel_change_without_other_changes_is_not_a_trial(base_url):
    config = active_config(base_url)
    reply = put_trel(base_url, config["trel"] is not True)
    try:
        assert reply.status_code == 200, reply.text
        assert reply.json()["status"] != "trial"
    finally:
        put_trel(base_url, config["trel"])


def test_trel_that_is_not_a_boolean_is_refused(base_url):
    config = active_config(base_url)
    config["trel"] = "on"
    reply = requests.put(
        f"{base_url}/device/network", json=config, timeout=REQUEST_TIMEOUT_S
    )
    assert reply.status_code == 400
    assert "trel" in reply.text
