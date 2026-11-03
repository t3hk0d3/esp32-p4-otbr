"""Network names: mDNS hostname, Home Assistant (meshcop) name, web UI advert, DHCP name (spec 2026-11-03-network-names).

Hardware: the usual single-mode rig (DHCP on the untagged LAN, `board_ip`) and the Docker macvlan network `otbr-p0` on
the board's segment (see test_br_boot.TestBootMdns). The split-mode class needs the VLAN rig of test_vlan.py and is
selected with `-m vlan_switch`. The DHCP client name is checked by hand on the router (no rig DHCP server).

Every test leaves the board with the default names (fixture `default_names`).
"""

import ipaddress
import json
import subprocess
import textwrap
import threading
import time
from pathlib import Path

import pytest
import requests
from conftest import wait_ot_state
from test_br_boot import MDNS_IMAGE, MDNS_NETWORK
from test_vlan import (  # noqa: F401  (rig, split_board, to_split_trial: fixtures of the VLAN rig)
    http_from,
    link_local,
    mdns_records,
    rig,
    split_board,
    to_split_trial,
)

HERE = Path(__file__).resolve().parent
DEFAULTS = {
    "hostname": "",
    "ha_name_follows_hostname": True,
    "ha_name": "",
    "http_advert": False,
    "dhcp_name": "",
}
SETTLE_S = 3  # probing and announcing of a new name
LEADER_TIMEOUT_S = 90
TYPE_PTR = 12

# Runs in a container: one multicast DNS query, prints one JSON line per answer record [type, name, value]
# (value: address for A/AAAA, target for PTR/SRV, hex otherwise). Usage: name type_number seconds
QUERY_SNIPPET = textwrap.dedent("""
    import json, socket, sys, time
    sys.path.insert(0, "/t")
    import mdns_query
    name, qtype, seconds = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
    sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    sock.bind(("::", 0))
    sock.settimeout(0.5)
    interface = socket.if_nametoindex("eth0")
    end, last, seen = time.monotonic() + seconds, 0.0, set()
    while time.monotonic() < end:
        if time.monotonic() - last >= 1:
            sock.sendto(mdns_query.build_query(name, qtype), ("ff02::fb", 5353, 0, interface))
            last = time.monotonic()
        try:
            packet, _ = sock.recvfrom(1500)
        except socket.timeout:
            continue
        for section, rname, rtype, value in mdns_query.records(packet):
            row = json.dumps([rtype, rname, value if isinstance(value, str) else value.hex()])
            if section == "an" and row not in seen:
                seen.add(row)
                print(row, flush=True)
""")


def lan(argv, timeout=90, raw=False):
    """Run `argv` in a python container on the board's LAN with the mDNS helpers in /t."""
    caps = ["--cap-add", "NET_RAW"] if raw else []
    return subprocess.run(
        ["docker", "run", "--rm", "--network", MDNS_NETWORK, *caps, "-v", f"{HERE}:/t:ro", MDNS_IMAGE, *argv],
        capture_output=True, text=True, timeout=timeout, check=False,
    )  # fmt: skip


def mdns(name, qtype, seconds=6):
    """Answer records for a query: [(type, name, value)]."""
    result = lan(["python", "-c", QUERY_SNIPPET, name, str(qtype), str(seconds)])
    return [
        tuple(json.loads(line))
        for line in result.stdout.splitlines()
        if line.startswith("[")
    ]


def addresses(hostname):
    result = lan(["python", "/t/mdns_query.py", f"{hostname}.local", "6"])
    return [ipaddress.ip_address(line) for line in result.stdout.split()]


def instances(service):
    """Instance names announced for `service` (e.g. "_meshcop._udp"), without the service suffix."""
    suffix = f".{service}.local"
    return [
        value[: -len(suffix)]
        for rtype, _, value in mdns(f"{service}.local", TYPE_PTR)
        if value.endswith(suffix)
    ]


def put_names(http, **changes):
    response = http.put("/device/names", json=DEFAULTS | changes)
    assert response.status_code == 200, response.text
    time.sleep(SETTLE_S)
    return response.json()


@pytest.fixture
def default_names(http):
    yield
    http.put("/device/names", json=DEFAULTS)


class TestNamesApi:
    """GET/PUT /device/names"""

    def test_fresh_board_has_the_default_names(self, http, default_names):
        # On a board with nothing stored: hostname esp-otbr-xxxxxx answering mDNS, HA name following, DHCP esp-otbr.
        http.put("/device/names", json=DEFAULTS)
        body = http.get("/device/names").json()
        assert body["hostname"] == ""
        assert body["hostname_live"] == body["hostname_default"]
        assert body["hostname_default"].startswith("esp-otbr-")
        assert body["ha_name_effective"] == body["hostname_default"]
        assert body["dhcp_name_default"] == "esp-otbr"
        assert addresses(body["hostname_default"])

    def test_invalid_names_are_refused_with_the_field(self, http, default_names):
        # On a bad hostname, a missing key, a 64-byte HA name: 400 invalid_config naming the field.
        cases = [
            (DEFAULTS | {"hostname": "bad.name"}, "hostname"),
            ({k: v for k, v in DEFAULTS.items() if k != "dhcp_name"}, "dhcp_name"),
            (
                DEFAULTS | {"ha_name_follows_hostname": False, "ha_name": "Ж" * 32},
                "ha_name",
            ),
        ]
        for body, field in cases:
            response = http.put("/device/names", json=body)
            assert response.status_code == 400, body
            assert response.json()["error"] == "invalid_config"
            assert response.json()["field"] == field

    def test_values_survive_a_reboot(self, http, board_ip, reboot, default_names):
        # On a saved set of names and a reboot: GET returns them.
        put_names(
            http, hostname="names-reboot", dhcp_name="names-router", http_advert=True
        )
        reboot()
        body = http.get("/device/names").json()
        assert (body["hostname"], body["dhcp_name"], body["http_advert"]) == (
            "names-reboot",
            "names-router",
            True,
        )


class TestNamesLive:
    """netnames live apply, single mode"""

    def test_new_hostname_answers_and_the_old_one_stops(
        self, http, board_ip, default_names
    ):
        # On a new hostname: mDNS answers for it with the board's address, the old name falls silent, IPv4 unchanged.
        old = http.get("/device/names").json()["hostname_live"]
        body = put_names(http, hostname="names-test")
        assert body["applied"] is True
        assert ipaddress.ip_address(board_ip) in addresses("names-test")
        assert addresses(old) == []
        assert (
            requests.get(f"http://{board_ip}/device/names", timeout=10).json()[
                "hostname_live"
            ]
            == "names-test"
        )

    def test_ha_name_is_the_meshcop_instance(
        self, dut, stored_dataset, http, default_names
    ):
        # On a set HA name with Thread up: the _meshcop._udp browse lists it.
        wait_ot_state(dut, {"leader"}, LEADER_TIMEOUT_S)
        put_names(http, ha_name_follows_hostname=False, ha_name="Names Test OTBR")
        assert any(
            name.startswith("Names Test OTBR") for name in instances("_meshcop._udp")
        )

    def test_web_ui_advert_only_while_on(self, http, default_names):
        # On advert on: _http._tcp lists the HA name; off again: gone.
        put_names(http, hostname="names-http", http_advert=True)
        assert "names-http" in instances("_http._tcp")
        put_names(http, hostname="names-http", http_advert=False)
        assert "names-http" not in instances("_http._tcp")

    def test_dhcp_name_change_keeps_the_address(self, http, board_ip, default_names):
        # On a new DHCP name: the same IPv4 answers before, during and right after the save.
        failures = []
        stop = threading.Event()

        def probe():
            while not stop.is_set():
                try:
                    requests.get(f"http://{board_ip}/device/status", timeout=3)
                except requests.RequestException as e:
                    failures.append(repr(e))
                time.sleep(0.3)

        prober = threading.Thread(target=probe)
        prober.start()
        try:
            body = put_names(http, dhcp_name="names-router")
        finally:
            stop.set()
            prober.join()
        assert body["applied"] is True, body
        assert failures == []

    def test_conflict_renames_and_is_reported(self, http, default_names):
        # On another host announcing the same name with bigger addresses: the board answers as name-2, GET says so.
        put_names(http, hostname="names-clash")
        result = lan(
            ["python", "/t/mdns_spoof.py", "fe80::ffff:ffff:ffff:fffe", "names-clash.local", "255.255.255.254",
             "fe80::ffff:ffff:ffff:ffff", "12"],
            timeout=60, raw=True,
        )  # fmt: skip
        assert result.returncode == 0, result.stderr
        time.sleep(4)
        assert http.get("/device/names").json()["hostname_live"] == "names-clash-2"


@pytest.mark.vlan_switch
class TestNamesSplit:
    """netnames in split mode (VLAN rig of test_vlan.py)"""

    def test_no_web_ui_advert_and_nothing_on_mgmt(self, split_board):  # noqa: F811
        # On split mode with the advert on: no _http._tcp on either side, the hostname never answered on mgmt.
        body = json.dumps(DEFAULTS | {"hostname": "names-split", "http_advert": True})
        status, _, reply = http_from(
            "mgmt", link_local(split_board, "mgmt"), "PUT", "/device/names", body
        )
        assert status == requests.codes.ok, reply
        assert json.loads(reply)["http_advert_available"] is False
        time.sleep(SETTLE_S)
        for side in ("mgmt", "thread"):
            assert not mdns_records(
                side, "_http._tcp.local", TYPE_PTR, full_window=True
            ), side
        assert not mdns_records("mgmt", "names-split.local", 28, full_window=True)
        assert mdns_records("thread", "names-split.local", 28)
