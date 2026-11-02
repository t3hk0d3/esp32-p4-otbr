"""Device pages served by the board (BDD: Pages, on-board; docs/superpowers/specs/2026-11-02-device-pages-design.md).
Hardware-gated: runs in the batched hardware pass. The page logic itself is covered on the host by tools/tests
(test_device_pages.py and one test_<page>_page.py per page).

Manual check list (browser, board in BR_RUNNING, then repeat in SNIFFER and RADIO_FAULT):
  1. Device menu: Ethernet, Firmware, Packet capture, Diagnostics; "Device" is highlighted on each of them and the menu
     opens from the keyboard (Tab onto "Device"). "Dashboard" is highlighted on /.
  2. Dashboard (/): the Thread network and Identifiers cards and the Thread address table fill in BR_RUNNING; the PSKc
     shows only after Show; in SNIFFER the cards say they show while running. The banner says "Border router running" (green); in SNIFFER "Capturing packets" with a link to Packet
     capture; in RADIO_FAULT "Radio fault" with a link to Firmware. Tiles show versions, addresses and load.
  3. Switch to another tab for 10 s: /device/* requests stop (browser network tab) and resume on return.
  4. Firmware: jumper removed: the recovery card explains the jumper; fitted: Recover and Show C6 console, the log fills
     the box. Fault "no rcp firmware" (c6_erase): the radio card is outlined with the hint. Drop a file on a drop zone:
     its name shows. Upload a garbage file as bundle: "...: <detail>" and the board keeps running.
  5. Packet capture: empty channel, 30 s: the browser saves sniffer.pcap; the page switches to the running card with
     time left and frames; Stop ends it. The Wireshark command streams with curl -N into wireshark -k -i -.
  6. Ethernet: pick Split, enter VLAN 10 and 20, Review changes: the table lists Setup and both interfaces, the VLAN
     checkbox gates Apply. Apply on the VLAN rig: the board reboots; at the management address every device page shows
     the trial banner with a countdown; Keep new settings clears it. Discard brings back the running settings.
  7. Diagnostics: per-core bars, memory, lock table, tasks busiest first, refreshing every 2 s.
  8. A task name or log text with markup characters appears literally (nothing is interpreted as HTML).
"""

import os
from pathlib import Path

import pytest
import requests

REPO = Path(__file__).resolve().parents[2]
FRONTEND = REPO / "p4" / "components" / "esp_ot_br_server" / "frontend"
ENDPOINTS = [
    "/device/status",
    "/device/diag",
    "/device/network",
    "/device/network/confirm",
    "/device/sniffer.pcap",
    "/device/sniffer",
    "/device/ota",
    "/device/ota/url",
    "/device/radio/firmware",
    "/device/radio/recover",
    "/device/radio/log",
    "/device/reboot",
]
REQUEST_TIMEOUT_S = 10
UPLOAD_TIMEOUT_S = 240
GARBAGE_BYTES = 8192


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


@pytest.fixture
def rcp_image():
    path = Path(
        os.environ.get("OTBR_RCP_IMAGE", REPO / "c6_rcp" / "build" / "c6_rcp.bin")
    )
    if not path.is_file():
        pytest.skip(f"no RCP image at {path}")
    return path.read_bytes()


PAGES = {
    "index": "status",
    "ethernet": "ethernet",
    "firmware": "firmware",
    "capture": "capture",
    "diagnostics": "diagnostics",
}


class TestServing:
    """GET each device page and each script"""

    @pytest.mark.parametrize("name", sorted(PAGES))
    def test_page_is_served_from_web_as_html(self, base_url, name):
        # On GET /<page>.html: 200, text/html, and the file content of the frontend directory.
        response = requests.get(f"{base_url}/{name}.html", timeout=REQUEST_TIMEOUT_S)
        assert response.status_code == requests.codes.ok
        assert response.headers["Content-Type"].startswith("text/html")
        # Bytes, not text: the board sends no charset, so requests would decode UTF-8 (the en dash) as Latin-1.
        assert response.content == (FRONTEND / f"{name}.html").read_bytes()

    @pytest.mark.parametrize("script", sorted(set(PAGES.values()) | {"device_common"}))
    def test_script_is_served_from_web_as_javascript(self, base_url, script):
        # On GET /static/<script>.js: 200, a JavaScript content type, and the file content.
        response = requests.get(
            f"{base_url}/static/{script}.js", timeout=REQUEST_TIMEOUT_S
        )
        assert response.status_code == requests.codes.ok
        assert "javascript" in response.headers["Content-Type"]
        assert response.content == (FRONTEND / "static" / f"{script}.js").read_bytes()

    def test_scripts_reference_each_device_endpoint(self, base_url):
        # On the served scripts together: every endpoint of the API appears in one of them.
        text = "".join(
            requests.get(f"{base_url}/static/{s}.js", timeout=REQUEST_TIMEOUT_S).text
            for s in set(PAGES.values()) | {"device_common"}
        )
        assert all(endpoint in text for endpoint in ENDPOINTS)

    def test_nav_script_links_every_device_page(self, base_url):
        # On the served api.js: an entry for each of the five pages.
        text = requests.get(f"{base_url}/static/api.js", timeout=REQUEST_TIMEOUT_S).text
        assert all(f"/{name}.html" in text for name in PAGES)


class TestEndpointsTheSamePathsAsThePage:
    """The requests the pages send"""

    def test_status_has_the_fields_the_page_renders(self, base_url):
        # On GET /device/status: the keys the status section reads exist.
        body = requests.get(
            f"{base_url}/device/status", timeout=REQUEST_TIMEOUT_S
        ).json()
        assert {
            "mode",
            "app",
            "rcp",
            "jumper",
            "eth",
            "pd",
            "sniffer",
            "fault_reason",
        } <= body.keys()
        assert {
            "present",
            "base_expected",
            "base_installed",
            "recovery_failed",
        } <= body["jumper"].keys()

    def test_diag_has_the_fields_the_page_renders(self, base_url):
        # On GET /device/diag: cpu, tasks, memory and ot_lock exist with the sub-keys the page reads.
        body = requests.get(f"{base_url}/device/diag", timeout=REQUEST_TIMEOUT_S).json()
        assert {
            "uptime_s",
            "reset_reason",
            "cpu",
            "tasks",
            "memory",
            "ot_lock",
        } <= body.keys()
        assert {"1s", "10s", "60s"} == body["cpu"][0]["load_pct"].keys()

    def test_network_has_the_fields_the_page_renders(self, base_url):
        # On GET /device/network: mode, active, trial, interfaces and fallback exist.
        body = requests.get(
            f"{base_url}/device/network", timeout=REQUEST_TIMEOUT_S
        ).json()
        assert {
            "mode",
            "active",
            "trial",
            "interfaces",
            "fallback",
            "vlan_rx_dropped",
        } <= body.keys()

    def test_bundle_upload_with_garbage_gives_400_and_a_detail_the_page_shows(
        self, base_url
    ):
        # On a garbage body with Content-Length posted to /device/ota: 400 and JSON {error, detail}.
        response = requests.post(
            f"{base_url}/device/ota",
            data=bytes(GARBAGE_BYTES),
            headers={"Content-Type": "application/octet-stream"},
            timeout=UPLOAD_TIMEOUT_S,
        )
        assert response.status_code == requests.codes.bad_request
        assert {"error", "detail"} <= response.json().keys()

    def test_rcp_upload_reports_the_installed_version(self, base_url, rcp_image):
        # On a valid RCP image posted to /device/radio/firmware: 200 and {"installed": version}.
        response = requests.post(
            f"{base_url}/device/radio/firmware",
            data=rcp_image,
            headers={"Content-Type": "application/octet-stream"},
            timeout=UPLOAD_TIMEOUT_S,
        )
        assert response.status_code == requests.codes.ok
        assert response.json()["installed"]

    def test_recover_without_jumper_gives_412(self, base_url):
        # On POST /device/radio/recover without the jumper: 412 (the page hides the control in that case).
        status = requests.get(
            f"{base_url}/device/status", timeout=REQUEST_TIMEOUT_S
        ).json()
        if status["jumper"]["present"]:
            pytest.skip("jumper fitted")
        response = requests.post(
            f"{base_url}/device/radio/recover", timeout=REQUEST_TIMEOUT_S
        )
        assert response.status_code == requests.codes.precondition_failed
