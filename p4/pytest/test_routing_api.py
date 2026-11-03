"""Mesh routing: PUT /device/routing on the board (spec docs/superpowers/specs/2026-11-03-mesh-routing-design.md,
section 9). Not marked: runs with the normal on-board suite, no routers needed (the rig tests are test_routing.py).

Every test puts back the routing settings it found, through test_routing.py's original_routing fixture (which also
keeps them in p4/pytest/.routing-restore.json until they are back; recovery in test_routing.py's docstring).

Run: make test-target TEST=p4/pytest/test_routing_api.py
"""

# ruff: noqa: F811  (fixtures re-exported from test_routing are used as parameters)
import time

import pytest
import requests
from test_routing import (  # noqa: F401  (fixture re-export)
    BOARD_ASN,
    STATUS_TIMEOUT_S,
    UNREACHABLE_PEER,
    base_url,
    bgp_settings,
    configure,
    device_status,
    get_routing,
    original_routing,
    put_routing,
    settings_of,
)


class TestPut:
    """routing, PUT /device/routing on the board"""

    def test_bgp_with_no_peers_is_400(self, base_url, original_routing):
        # put: bgp with no peers is 400
        body = {
            "method": "bgp",
            "omr": {"mode": "auto"},
            "announce_ula": False,
            "bgp": {
                "local_asn": BOARD_ASN,
                "router_id": None,
                "hold_time": 90,
                "peers": [],
            },
        }
        response = put_routing(base_url, body)
        assert response.status_code == requests.codes.bad_request, response.text
        assert response.json() == {"error": "invalid_request", "detail": "bgp.peers"}
        assert settings_of(get_routing(base_url)) == original_routing

    @pytest.mark.parametrize("method", ["ripng", "bgp", "off"])
    def test_every_method_but_pd_shows_pd_state_disabled(
        self, base_url, original_routing, method
    ):
        # put: every method but pd shows pd.state disabled
        bgp = bgp_settings(UNREACHABLE_PEER, BOARD_ASN) if method == "bgp" else None
        configure(base_url, original_routing, method, bgp=bgp)
        deadline = time.monotonic() + STATUS_TIMEOUT_S
        pd = None
        while time.monotonic() < deadline:
            pd = device_status(base_url)["pd"]
            if pd["state"] == "disabled":
                return
            time.sleep(1)
        pytest.fail(f"pd is {pd} with method {method}")
