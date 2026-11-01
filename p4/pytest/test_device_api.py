"""device_api: /device/* endpoints (spec 5.7; BDD: Task 15). Hardware-gated: written with Task 15 as the contract,
deferred to V2 (never run in Task 15).

Firmware tests post `c6_rcp/build/c6_rcp.bin` (or $OTBR_RCP_IMAGE). The mid-transfer C6 reset uses the test-build console
command `c6_inject_reset_at <offset>`. OTA tests reuse the bundle server of test_ota.py.
"""

# ruff: noqa: F811  (fixtures re-exported from test_ota are used as parameters)
import os
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest
import requests
from conftest import GOT_IP, ROM_BANNER, start_capture, stop_capture, wait_reachable
from test_ota import bundle_server, host_address_towards  # noqa: F401  (fixture re-export)

REPO = Path(__file__).resolve().parents[2]
BOOT_TIMEOUT_S = 60
PUSH_TIMEOUT_S = 240
OTA_TIMEOUT_S = 240
BACK_TO_BR_S = 2
STATUS_KEYS = {"mode", "app", "rcp", "jumper", "eth", "pd", "sniffer", "fault_reason"}
CHANNEL = 15
RCP_OFFSET_MID = 100_000


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


def status(base_url):
    response = requests.get(f"{base_url}/device/status", timeout=10)
    assert response.status_code == requests.codes.ok
    return response.json()


def wait_for_mode(base_url, mode, timeout):
    deadline = time.monotonic() + timeout
    current = None
    while time.monotonic() < deadline:
        current = status(base_url)["mode"]
        if current == mode:
            return
        time.sleep(0.5)
    pytest.fail(f"mode is {current}, expected {mode}")


def reboot_into(dut, reboot):
    """Hard reset; return the base URL once the board has its address again."""
    reboot()
    address = dut.expect(GOT_IP, timeout=BOOT_TIMEOUT_S).group(1).decode()
    wait_reachable(address)
    return f"http://{address}"


def assert_error_body(response):
    body = response.json()
    assert response.headers["Content-Type"].startswith("application/json")
    assert isinstance(body["error"], str)
    assert body["error"]
    assert isinstance(body["detail"], str)


class StallingBody:
    """Request body of known length that stops after `stall_at` bytes until `release()` or `abort()`."""

    def __init__(self, data, stall_at):
        self.data = data
        self.stall_at = stall_at
        self.stalled = threading.Event()
        self._go = threading.Event()
        self._abort = False

    def __len__(self):
        return len(self.data)

    def __iter__(self):
        yield self.data[: self.stall_at]
        self.stalled.set()
        self._go.wait(OTA_TIMEOUT_S)
        if self._abort:
            raise ConnectionAbortedError("test aborted the upload")
        yield self.data[self.stall_at :]

    def abort(self):
        self._abort = True
        self._go.set()


def post_in_background(url, body, **kwargs):
    """POST `body` on a thread; return (thread, result dict with 'response' or 'error')."""
    result = {}

    def run():
        try:
            result["response"] = requests.post(
                url, data=body, timeout=OTA_TIMEOUT_S, **kwargs
            )
        except requests.RequestException as error:
            result["error"] = error

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    return thread, result


def post_headers_only(host, path, content_length):
    """Send the request head of a POST with a huge Content-Length and one body byte; return (status code, body text).

    The byte is needed: esp_http_server dispatches the handler only once the first body byte arrived."""
    with socket.create_connection((host, 80), timeout=30) as sock:
        sock.sendall(
            f"POST {path} HTTP/1.1\r\nHost: {host}\r\nContent-Length: {content_length}\r\n\r\nx".encode()
        )
        reply = b""
        while b"\r\n\r\n" not in reply or not reply.rstrip().endswith(b"}"):
            chunk = sock.recv(4096)
            if not chunk:
                break
            reply += chunk
    head, _, body = reply.partition(b"\r\n\r\n")
    return int(head.split()[1]), body.decode(errors="replace")


class ImageServer:
    """HTTP server for one RCP image at /rcp.bin."""

    def __init__(self, image, host):
        data = image

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                self.send_response(200)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        self.httpd = ThreadingHTTPServer((host, 0), Handler)
        self.url = f"http://{host}:{self.httpd.server_address[1]}/rcp.bin"
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


@pytest.fixture
def image_server(rcp_image, board_ip):
    server = ImageServer(rcp_image, host_address_towards(board_ip))
    yield server
    server.close()


class TestStatus:
    """device_api GET /device/status"""

    def test_status_on_any_mode_returns_200_with_versions_rcp_pd_and_sniffer(
        self, dut, stored_dataset, base_url, send
    ):
        # On any mode: returns 200 with mode, versions, rcp {installed, compatible}, PD state, sniffer stats.
        # (With a stored dataset: an idle radio without Thread traffic would not notice the killed RCP.)
        stream = None
        for mode in ("BR_RUNNING", "SNIFFER", "RADIO_FAULT"):
            if mode == "SNIFFER":
                stream = start_capture(base_url, CHANNEL)
            elif mode == "RADIO_FAULT":
                stop_capture(base_url, stream)
                send("rcp_kill", rb"rcp_kill: ESP_OK")
                # The recovery reset "works" first (the hold only keeps the C6 down); the fault follows.
                dut.expect(rb"br_core: radio fault: RCP did not answer", timeout=30)
            body = status(base_url)
            assert body["mode"] == mode
            assert set(body) == STATUS_KEYS
            assert body["app"]["version"]
            assert body["app"]["slot"] in (0, 1)
            assert set(body["rcp"]) == {"installed", "compatible"}
            assert {"state", "prefix"} <= set(body["pd"])
            assert {"active", "frames", "drops", "channel"} <= set(body["sniffer"])

    @pytest.mark.parametrize(
        ("setup", "reason"),
        [("empty", "no rcp firmware"), ("hold", "rcp incompatible")],
    )
    def test_status_on_radio_fault_gives_the_fault_reason(
        self, dut, send, reboot, probe_override, setup, reason
    ):
        # On RADIO_FAULT: fault_reason is "no rcp firmware" or "rcp incompatible" where applicable.
        if setup == "empty":
            probe_override("empty")
        else:
            send("c6_hold 1", rb"c6_hold: ok")
        url = reboot_into(dut, reboot)
        wait_for_mode(url, "RADIO_FAULT", BOOT_TIMEOUT_S)
        body = status(url)
        assert body["mode"] == "RADIO_FAULT"
        assert body["fault_reason"] == reason


class TestSnifferEndpoint:
    """device_api GET /device/sniffer.pcap, DELETE /device/sniffer"""

    def test_sniffer_pcap_on_br_running_streams_until_stopped(self, base_url):
        # On BR_RUNNING: streams pcap until stopped.
        stream = start_capture(base_url, CHANNEL)
        try:
            chunks = stream.iter_content(
                chunk_size=24
            )  # keep the iterator: dropping it closes the connection
            first = next(chunks)
            assert first[:4] == bytes.fromhex("d4c3b2a1")  # little-endian a1b2c3d4
            time.sleep(2)
            assert status(base_url)["mode"] == "SNIFFER"  # still streaming
        finally:
            stop_capture(base_url, stream)

    def test_sniffer_pcap_on_sniffer_returns_409(self, base_url):
        # On SNIFFER: returns 409.
        stream = start_capture(base_url, CHANNEL)
        try:
            response = requests.get(
                f"{base_url}/device/sniffer.pcap?channel={CHANNEL}", timeout=10
            )
            assert response.status_code == requests.codes.conflict
            assert_error_body(response)
        finally:
            stop_capture(base_url, stream)

    def test_sniffer_pcap_on_radio_fault_returns_409(self, dut, send, reboot):
        # On RADIO_FAULT: returns 409.
        send("c6_hold 1", rb"c6_hold: ok")
        url = reboot_into(dut, reboot)
        wait_for_mode(url, "RADIO_FAULT", BOOT_TIMEOUT_S)
        response = requests.get(
            f"{url}/device/sniffer.pcap?channel={CHANNEL}", timeout=10
        )
        assert response.status_code == requests.codes.conflict

    @pytest.mark.parametrize(
        "query", ["channel=10", "channel=27", "channel=15&duration=3601"]
    )
    def test_sniffer_pcap_with_channel_or_duration_out_of_range_returns_400(
        self, base_url, query
    ):
        # On channel outside 11..26 or duration > 3600: returns 400.
        response = requests.get(f"{base_url}/device/sniffer.pcap?{query}", timeout=10)
        assert response.status_code == requests.codes.bad_request
        assert_error_body(response)
        assert status(base_url)["mode"] == "BR_RUNNING"

    def test_sniffer_pcap_without_channel_and_dataset_returns_400(
        self, dut, fresh_nvs, base_url
    ):
        # On no channel and no dataset: returns 400.
        response = requests.get(f"{base_url}/device/sniffer.pcap", timeout=10)
        assert response.status_code == requests.codes.bad_request

    def test_sniffer_pcap_client_disconnect_returns_to_br_running_within_2_seconds(
        self, base_url
    ):
        # On client disconnect: returns to BR_RUNNING within 2 s.
        stream = start_capture(base_url, CHANNEL)
        assert status(base_url)["mode"] == "SNIFFER"
        stream.close()
        deadline = time.monotonic() + BACK_TO_BR_S
        mode = None
        while time.monotonic() < deadline:
            mode = status(base_url)["mode"]
            if mode == "BR_RUNNING":
                return
            time.sleep(0.1)
        pytest.fail(f"mode is {mode} two seconds after the disconnect")

    def test_delete_sniffer_on_active_capture_ends_the_stream_and_returns_200(
        self, base_url
    ):
        # On active capture: ends the stream and returns 200.
        stream = start_capture(base_url, CHANNEL)
        response = requests.delete(f"{base_url}/device/sniffer", timeout=30)
        assert response.status_code == requests.codes.ok
        assert len(stream.content) >= 24  # the chunked body ends
        wait_for_mode(base_url, "BR_RUNNING", BACK_TO_BR_S)

    def test_delete_sniffer_without_capture_returns_404(self, base_url):
        # On no capture: returns 404.
        response = requests.delete(f"{base_url}/device/sniffer", timeout=10)
        assert response.status_code == requests.codes.not_found
        assert_error_body(response)


class TestOtaEndpoint:
    """device_api POST /device/ota, POST /device/ota/url"""

    def test_ota_with_valid_bundle_returns_200_and_reboots_into_the_new_slot(
        self,
        dut,
        base_url,
        bundle_server,
    ):
        # On valid bundle: returns 200 and reboots into the new slot.
        running = status(base_url)["app"]["slot"]
        response = requests.post(
            f"{base_url}/device/ota", data=bundle_server.bundle, timeout=OTA_TIMEOUT_S
        )
        assert response.status_code == requests.codes.ok
        dut.expect(ROM_BANNER, timeout=BOOT_TIMEOUT_S)
        url = "http://" + dut.expect(GOT_IP, timeout=BOOT_TIMEOUT_S).group(1).decode()
        assert status(url)["app"]["slot"] == 1 - running

    def test_ota_with_invalid_bundle_returns_400_and_keeps_running(
        self,
        base_url,
        bundle_server,
    ):
        # On invalid bundle: returns 400 and keeps running.
        before = status(base_url)
        response = requests.post(
            f"{base_url}/device/ota",
            data=bytes(bundle_server.corrupt_web),
            timeout=OTA_TIMEOUT_S,
        )
        assert response.status_code == requests.codes.bad_request
        assert_error_body(response)
        after = status(base_url)
        assert after["app"]["slot"] == before["app"]["slot"]
        assert after["mode"] == before["mode"]

    def test_ota_while_an_update_is_running_returns_409(
        self,
        base_url,
        bundle_server,
    ):
        # On update already running: returns 409.
        body = StallingBody(bundle_server.bundle, len(bundle_server.bundle) // 3)
        thread, _ = post_in_background(f"{base_url}/device/ota", body)
        try:
            assert body.stalled.wait(60)
            time.sleep(1)
            second = requests.post(f"{base_url}/device/ota", data=b"x" * 16, timeout=10)
            assert second.status_code == requests.codes.conflict
            assert_error_body(second)
        finally:
            body.abort()
            thread.join(timeout=60)

    def test_ota_url_with_reachable_valid_bundle_returns_200_and_reboots_into_the_new_slot(
        self,
        dut,
        base_url,
        bundle_server,
    ):
        # On reachable URL with valid bundle: returns 200 and reboots into the new slot.
        running = status(base_url)["app"]["slot"]
        response = requests.post(
            f"{base_url}/device/ota/url",
            json={"url": bundle_server.url("ok")},
            timeout=OTA_TIMEOUT_S,
        )
        assert response.status_code == requests.codes.ok
        dut.expect(ROM_BANNER, timeout=BOOT_TIMEOUT_S)
        url = "http://" + dut.expect(GOT_IP, timeout=BOOT_TIMEOUT_S).group(1).decode()
        assert status(url)["app"]["slot"] == 1 - running


class TestOtaEdgeCases:
    """device_api POST /device/ota, committed update and request validation"""

    def test_requests_after_a_successful_ota_during_the_reboot_window_return_409(
        self, dut, base_url, bundle_server
    ):
        # R20: a committed update waits for its reboot: another OTA, a radio push and a reboot request are refused.
        requests.post(
            f"{base_url}/device/ota", data=bundle_server.bundle, timeout=OTA_TIMEOUT_S
        ).raise_for_status()
        second = requests.post(f"{base_url}/device/ota", data=b"x" * 16, timeout=10)
        push = requests.post(
            f"{base_url}/device/radio/firmware", data=b"x" * 16, timeout=10
        )
        reboot = requests.post(f"{base_url}/device/reboot", timeout=10)
        assert second.status_code == requests.codes.conflict
        assert push.status_code == requests.codes.conflict
        assert reboot.status_code == requests.codes.conflict
        dut.expect(ROM_BANNER, timeout=BOOT_TIMEOUT_S)

    def test_ota_url_with_a_malformed_body_returns_400(self, base_url):
        # ota/url body must be {"url": "http://..."}.
        for body in (
            b"not json",
            b"{}",
            b'{"url": 5}',
            b'{"url": ""}',
            b'{"url": "ftp://x/y"}',
        ):
            response = requests.post(
                f"{base_url}/device/ota/url", data=body, timeout=10
            )
            assert response.status_code == requests.codes.bad_request, body
            assert_error_body(response)

    def test_ota_url_with_an_https_url_returns_400_https_not_supported(self, base_url):
        response = requests.post(
            f"{base_url}/device/ota/url",
            json={"url": "https://example.org/a.bin"},
            timeout=10,
        )
        assert response.status_code == requests.codes.bad_request
        assert response.json()["error"] == "https_not_supported"

    def test_ota_with_an_absurd_content_length_returns_413_without_starting(
        self, board_ip, base_url
    ):
        status_code, body = post_headers_only(board_ip, "/device/ota", 64 * 1024 * 1024)
        assert status_code == requests.codes.request_entity_too_large
        assert "payload_too_large" in body
        assert status(base_url)["mode"] == "BR_RUNNING"

    def test_reboot_while_an_update_runs_returns_409(self, base_url, bundle_server):
        body = StallingBody(bundle_server.bundle, len(bundle_server.bundle) // 3)
        thread, _ = post_in_background(f"{base_url}/device/ota", body)
        try:
            assert body.stalled.wait(60)
            time.sleep(1)
            response = requests.post(f"{base_url}/device/reboot", timeout=10)
            assert response.status_code == requests.codes.conflict
            assert_error_body(response)
        finally:
            body.abort()
            thread.join(timeout=60)


class TestRadioFirmwareEndpoint:
    """device_api POST /device/radio/firmware"""

    @pytest.mark.parametrize("start", ["BR_RUNNING", "RADIO_FAULT"])
    def test_upload_of_a_valid_image_streams_it_to_the_c6_restarts_openthread_and_reports_the_version(
        self, dut, send, reboot, probe_override, rcp_image, board_ip, start
    ):
        # On BR_RUNNING or RADIO_FAULT and a valid c6_rcp.bin body: streams it to the C6 without storing it on the
        # P4; restarts OpenThread and returns 200 with the installed version; /device/status rcp.installed shows it.
        url = f"http://{board_ip}"
        if start == "RADIO_FAULT":
            probe_override("empty")
            url = reboot_into(dut, reboot)
            wait_for_mode(url, "RADIO_FAULT", BOOT_TIMEOUT_S)
        slot = status(url)["app"]["slot"]
        response = requests.post(
            f"{url}/device/radio/firmware", data=rcp_image, timeout=PUSH_TIMEOUT_S
        )
        assert response.status_code == requests.codes.ok, response.text
        installed = response.json()["installed"]
        assert installed
        dut.expect(rb"rcp_sync: BEGIN", timeout=5)  # the image went to the C6 ...
        wait_for_mode(url, "BR_RUNNING", PUSH_TIMEOUT_S)
        body = status(url)
        assert body["rcp"]["installed"] == installed
        assert body["rcp"]["compatible"]
        assert (
            body["app"]["slot"] == slot
        )  # ... and nothing was written to the P4's app slots

    def test_upload_without_a_known_size_returns_400(self, base_url, rcp_image):
        # On request without a known body size: returns 400 and the previous firmware still boots.
        before = status(base_url)["rcp"]["installed"]
        response = requests.post(
            f"{base_url}/device/radio/firmware",
            data=iter([rcp_image[:1024]]),
            timeout=30,
        )  # chunked: no Content-Length
        assert response.status_code == requests.codes.bad_request
        assert_error_body(response)
        assert status(base_url)["rcp"]["installed"] == before

    def test_upload_of_an_invalid_image_returns_400(self, base_url, rcp_image):
        # On request with an invalid image: returns 400 and the previous firmware still boots.
        before = status(base_url)["rcp"]["installed"]
        response = requests.post(
            f"{base_url}/device/radio/firmware",
            data=os.urandom(len(rcp_image)),
            timeout=PUSH_TIMEOUT_S,
        )
        assert response.status_code == requests.codes.bad_request, response.text
        assert_error_body(response)
        wait_for_mode(base_url, "BR_RUNNING", PUSH_TIMEOUT_S)
        assert status(base_url)["rcp"]["installed"] == before

    def test_c6_reset_mid_transfer_returns_a_json_error_and_the_previous_firmware_boots(
        self, dut, base_url, send, rcp_image
    ):
        # On C6 reset injected mid-transfer: returns a JSON error {error, detail}; the BR restarts with the previous
        # firmware.
        before = status(base_url)["rcp"]["installed"]
        send(f"c6_inject_reset_at {RCP_OFFSET_MID}", rb"c6_inject_reset_at: ok")
        response = requests.post(
            f"{base_url}/device/radio/firmware", data=rcp_image, timeout=PUSH_TIMEOUT_S
        )
        assert response.status_code >= requests.codes.bad_request
        assert_error_body(response)
        wait_for_mode(base_url, "BR_RUNNING", PUSH_TIMEOUT_S)
        assert status(base_url)["rcp"]["installed"] == before

    @pytest.mark.parametrize("busy_with", ["sniffer", "ota", "push"])
    def test_upload_during_sniffer_ota_or_another_push_returns_409(
        self,
        base_url,
        rcp_image,
        bundle_server,
        busy_with,
    ):
        # On SNIFFER, OTA or another firmware push in progress: returns 409.
        stream = body = thread = None
        if busy_with == "sniffer":
            stream = start_capture(base_url, CHANNEL)
        elif busy_with == "ota":
            body = StallingBody(bundle_server.bundle, len(bundle_server.bundle) // 3)
            thread, _ = post_in_background(f"{base_url}/device/ota", body)
        else:
            body = StallingBody(rcp_image, len(rcp_image) // 3)
            thread, _ = post_in_background(f"{base_url}/device/radio/firmware", body)
        try:
            if body:
                assert body.stalled.wait(60)
                time.sleep(1)
            response = requests.post(
                f"{base_url}/device/radio/firmware", data=rcp_image[:4096], timeout=10
            )
            assert response.status_code == requests.codes.conflict
            assert_error_body(response)
        finally:
            if stream:
                stop_capture(base_url, stream)
            if body:
                body.abort()
                thread.join(timeout=60)

    def test_from_url_with_reachable_valid_image_behaves_like_the_upload(
        self, dut, base_url, image_server
    ):
        # On reachable URL with a valid image: behaves like POST /device/radio/firmware.
        response = requests.post(
            f"{base_url}/device/radio/firmware",
            params={"from_url": image_server.url},
            timeout=PUSH_TIMEOUT_S,
        )
        assert response.status_code == requests.codes.ok, response.text
        installed = response.json()["installed"]
        assert installed
        wait_for_mode(base_url, "BR_RUNNING", PUSH_TIMEOUT_S)
        assert status(base_url)["rcp"]["installed"] == installed

    def test_from_url_with_unreachable_url_returns_a_json_error_and_keeps_the_firmware(
        self, base_url, board_ip
    ):
        # On unreachable URL: returns a JSON error and the previous firmware still boots.
        before = status(base_url)["rcp"]["installed"]
        dead = f"http://{host_address_towards(board_ip)}:1/rcp.bin"
        response = requests.post(
            f"{base_url}/device/radio/firmware",
            params={"from_url": dead},
            timeout=PUSH_TIMEOUT_S,
        )
        assert response.status_code >= requests.codes.bad_request
        assert_error_body(response)
        assert status(base_url)["mode"] == "BR_RUNNING"
        assert status(base_url)["rcp"]["installed"] == before

    def test_upload_with_a_content_length_above_the_c6_slot_returns_413_before_anything_is_torn_down(
        self, board_ip, base_url
    ):
        # The size bound (C6 slot, 1.5 MB) is checked in the handler: OpenThread keeps running, nothing is pushed.
        status_code, body = post_headers_only(
            board_ip, "/device/radio/firmware", 4 * 1024 * 1024
        )
        assert status_code == requests.codes.request_entity_too_large
        assert "payload_too_large" in body
        assert status(base_url)["mode"] == "BR_RUNNING"

    def test_from_url_with_an_https_url_returns_400_https_not_supported(self, base_url):
        response = requests.post(
            f"{base_url}/device/radio/firmware",
            params={"from_url": "https://example.org/a.bin"},
            timeout=10,
        )
        assert response.status_code == requests.codes.bad_request
        assert response.json()["error"] == "https_not_supported"

    def test_from_url_response_above_the_c6_slot_returns_413_and_keeps_openthread(
        self, base_url, board_ip
    ):
        big = ImageServer(b"\0" * (2 * 1024 * 1024), host_address_towards(board_ip))
        try:
            response = requests.post(
                f"{base_url}/device/radio/firmware",
                params={"from_url": big.url},
                timeout=PUSH_TIMEOUT_S,
            )
        finally:
            big.close()
        assert response.status_code == requests.codes.request_entity_too_large
        assert status(base_url)["mode"] == "BR_RUNNING"

    def test_from_url_together_with_a_body_returns_400(self, base_url, image_server):
        # from_url together with a non-empty body: returns 400.
        response = requests.post(
            f"{base_url}/device/radio/firmware",
            params={"from_url": image_server.url},
            data=b"not empty",
            timeout=30,
        )
        assert response.status_code == requests.codes.bad_request
        assert_error_body(response)

    @pytest.mark.parametrize("busy_with", ["sniffer", "ota", "push"])
    def test_from_url_during_sniffer_ota_or_another_push_returns_409(
        self,
        base_url,
        rcp_image,
        image_server,
        bundle_server,
        busy_with,
    ):
        # On SNIFFER, OTA or another firmware push in progress: returns 409.
        stream = body = thread = None
        if busy_with == "sniffer":
            stream = start_capture(base_url, CHANNEL)
        elif busy_with == "ota":
            body = StallingBody(bundle_server.bundle, len(bundle_server.bundle) // 3)
            thread, _ = post_in_background(f"{base_url}/device/ota", body)
        else:
            body = StallingBody(rcp_image, len(rcp_image) // 3)
            thread, _ = post_in_background(f"{base_url}/device/radio/firmware", body)
        try:
            if body:
                assert body.stalled.wait(60)
                time.sleep(1)
            response = requests.post(
                f"{base_url}/device/radio/firmware",
                params={"from_url": image_server.url},
                timeout=10,
            )
            assert response.status_code == requests.codes.conflict
        finally:
            if stream:
                stop_capture(base_url, stream)
            if body:
                body.abort()
                thread.join(timeout=60)


class TestRebootEndpoint:
    """device_api POST /device/reboot"""

    @pytest.mark.parametrize("mode", ["BR_RUNNING", "SNIFFER", "RADIO_FAULT"])
    def test_reboot_on_any_mode_returns_200_then_reboots(
        self, dut, send, reboot, board_ip, mode
    ):
        # On any mode: returns 200 then reboots.
        url = f"http://{board_ip}"
        if mode == "RADIO_FAULT":
            send("c6_hold 1", rb"c6_hold: ok")
            url = reboot_into(dut, reboot)
            wait_for_mode(url, "RADIO_FAULT", BOOT_TIMEOUT_S)
        stream = start_capture(url, CHANNEL) if mode == "SNIFFER" else None
        response = requests.post(f"{url}/device/reboot", timeout=10)
        assert response.status_code == requests.codes.ok
        dut.expect(ROM_BANNER, timeout=BOOT_TIMEOUT_S)
        if stream:
            stream.close()


class TestErrors:
    """device_api errors"""

    def test_every_device_error_body_is_json_with_error_and_detail(self, base_url):
        # On any /device/* error: body is JSON with error and detail.
        failing = [
            requests.delete(f"{base_url}/device/sniffer", timeout=10),
            requests.get(f"{base_url}/device/sniffer.pcap?channel=99", timeout=10),
            requests.post(f"{base_url}/device/ota", data=b"", timeout=10),
            requests.post(f"{base_url}/device/ota/url", json={"nourl": 1}, timeout=10),
            requests.post(f"{base_url}/device/radio/firmware", data=b"", timeout=10),
        ]
        for response in failing:
            assert response.status_code >= requests.codes.bad_request, response.url
            assert_error_body(response)
