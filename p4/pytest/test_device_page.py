"""Device page served by the board (BDD: device page; spec 5.7). Hardware-gated: written with Task 20 as the contract,
deferred to V3 (never run in Task 20). The page logic itself is covered on the host by tools/tests/test_device_page.py.

Manual check list (browser, board in BR_RUNNING, then repeat in SNIFFER and RADIO_FAULT):
  1. Open http://<board>/device.html: the nav bar has a highlighted "Device" entry; Status shows mode, versions, RCP
     version, jumper and Ethernet addresses; Diagnostics shows per-core load and the task table, refreshing every 2 s.
  2. Switch to another tab for 10 s: /device/diag requests stop (check the board log or the browser network tab) and
     resume on return. Scroll Diagnostics out of view: polling stops as well.
  3. Jumper removed: Recover and "Show C6 log" are hidden. Jumper fitted: both are shown, the log fills a <pre>.
  4. Fault "no rcp firmware" (erase the RCP, c6_erase): the RCP upload control is outlined and the hint names c6_rcp.bin.
  5. Start capture with channel 15, 30 s: the browser saves sniffer.pcap, Status shows frames while it runs; Stop ends it.
  6. Upload a garbage file as bundle: the Update card shows "...: <detail>" and the board keeps running.
  7. Network: apply a change: the confirm dialog warns about the reboot and the 5 minutes; after the reboot the banner
     shows a countdown and Confirm; confirming from the management side clears it.
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


class TestServing:
    """GET /device.html and /static/device.js"""

    def test_page_is_served_from_web_as_html(self, base_url):
        # On GET /device.html: 200, text/html, and the file content of the frontend directory.
        response = requests.get(f"{base_url}/device.html", timeout=REQUEST_TIMEOUT_S)
        assert response.status_code == requests.codes.ok
        assert response.headers["Content-Type"].startswith("text/html")
        assert response.text == (FRONTEND / "device.html").read_text()

    def test_script_is_served_from_web_as_javascript(self, base_url):
        # On GET /static/device.js: 200, a JavaScript content type, and the file content.
        response = requests.get(
            f"{base_url}/static/device.js", timeout=REQUEST_TIMEOUT_S
        )
        assert response.status_code == requests.codes.ok
        assert "javascript" in response.headers["Content-Type"]
        assert response.text == (FRONTEND / "static" / "device.js").read_text()

    def test_script_references_each_device_endpoint(self, base_url):
        # On the served script: every endpoint of the API appears in it.
        text = requests.get(
            f"{base_url}/static/device.js", timeout=REQUEST_TIMEOUT_S
        ).text
        assert all(endpoint in text for endpoint in ENDPOINTS)

    def test_nav_script_links_the_page_from_every_page(self, base_url):
        # On the served api.js: the nav bar entry for /device.html is present.
        text = requests.get(f"{base_url}/static/api.js", timeout=REQUEST_TIMEOUT_S).text
        assert "/device.html" in text


class TestEndpointsTheSamePathsAsThePage:
    """The requests the page sends"""

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
