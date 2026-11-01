"""Paired bundle OTA by URL (BDD: esp_br_http_ota patched core; spec 5.6). Deferred to V2: written with Task 13.

The test build's console command `ota_url <url>` calls esp_br_http_ota against an in-process HTTP server on the host.
The bundle holds a marker web image (`slot.txt`) and the app image of the running build, so a successful update flips
the running slot and serves the marker from /web. Failing updates must leave the boot partition and /web untouched.
"""

import importlib.util
import socket
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest
from conftest import CONSOLE_READY, GOT_IP, send_until_ack, wait_reachable

REPO = Path(__file__).resolve().parents[2]
MARKER_PREFIX = "ota-bundle-marker"
CONSOLE_TIMEOUT_S = 10
OTA_TIMEOUT_S = 240
BOOT_TIMEOUT_S = 60
OTA_BOOT = rb"ota_boot: boot=(-?\d) running=(\d)"
OTA_RESULT = rb"ota_url: (\w+)"
HEADER_LEN = 44
SHA_OFFSET = 12


def load_make_bundle():
    spec = importlib.util.spec_from_file_location(
        "make_bundle", REPO / "tools" / "make_bundle.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class BundleServer:
    """HTTP server for the bundles. `/ok`, `/bad_sha`, `/truncated_app` are served whole; `/disconnect` closes the
    connection halfway through the body; `/stalled` sends a third of the body and then waits until `release()`."""

    def __init__(self, bundle, host):
        self.bundle = bundle
        self.stalled = threading.Event()  # set when /stalled has sent its first third
        self._release = threading.Event()
        self.corrupt_web = bytearray(bundle)
        self.corrupt_web[SHA_OFFSET] ^= 0x01
        self.truncated_app = bundle[: len(bundle) - 4096]
        server = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                body, cut = server.route(self.path)
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                try:
                    self.wfile.write(body[:cut])
                    self.wfile.flush()
                    if cut < len(body):
                        if self.path == "/stalled":
                            server.stalled.set()
                            server._release.wait(OTA_TIMEOUT_S)
                        self.close_connection = True
                        self.connection.shutdown(socket.SHUT_RDWR)
                        return
                except OSError:
                    pass

        self.httpd = ThreadingHTTPServer((host, 0), Handler)
        self.port = self.httpd.server_address[1]
        self.host = host
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def route(self, path):
        full = len(self.bundle)
        if path == "/bad_sha":
            return bytes(self.corrupt_web), len(self.corrupt_web)
        if path == "/truncated_app":
            return self.truncated_app, len(self.truncated_app)
        if path == "/disconnect":
            return self.bundle, full // 2
        if path == "/stalled":
            return self.bundle, full // 3
        return self.bundle, full

    def release(self):
        self._release.set()

    def url(self, name):
        return f"http://{self.host}:{self.port}/{name}"

    def close(self):
        self.release()
        self.httpd.shutdown()
        self.httpd.server_close()


def host_address_towards(board_ip):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.connect((board_ip, 9))
        return probe.getsockname()[0]


@pytest.fixture
def bundle_server(dut, board_ip, spiffs_image_for, tmp_path):
    # A fresh marker per test: the previous test's successful update left its own marker in the slot now running.
    marker = f"{MARKER_PREFIX}-{uuid.uuid4().hex}"
    web_image = spiffs_image_for("web_0", {"slot.txt": marker.encode()})
    out = tmp_path / "otbr.bundle"
    make_bundle = load_make_bundle()
    assert (
        make_bundle.main(
            [
                "--web",
                str(web_image),
                "--app",
                str(Path(dut.app.binary_path) / "p4.bin"),
                "--out",
                str(out),
            ]
        )
        == 0
    )
    server = BundleServer(out.read_bytes(), host_address_towards(board_ip))
    server.marker = marker
    yield server
    server.close()


def boot_and_running(dut):
    send_until_ack(dut, "ota_boot", OTA_BOOT)
    match = dut.pexpect_proc.match
    return int(match.group(1)), int(match.group(2))


def ota_result(dut, timeout=OTA_TIMEOUT_S):
    return dut.expect(OTA_RESULT, timeout=timeout).group(1).decode()


def serves_marker(dut, marker):
    dut.write("cat /web/slot.txt")
    match = dut.expect(
        rb"cat: /web/slot.txt: (not found|[^\r\n]*)", timeout=CONSOLE_TIMEOUT_S
    )
    return match.group(1).decode().startswith(marker)


def assert_pair_unchanged(dut, running, marker):
    """Boot partition is still the running slot, /web still serves the old image, and no update is left running."""
    assert boot_and_running(dut) == (running, running)
    assert not serves_marker(dut, marker)
    dut.write("ota_probe")
    dut.expect(rb"ota_probe: ESP_OK", timeout=CONSOLE_TIMEOUT_S)


class TestOtaBundle:
    """esp_br_http_ota (patched core): bundle"""

    def test_ota_valid_bundle_flips_slot_and_serves_the_bundle_web(
        self, dut, bundle_server
    ):
        # On valid bundle: writes web_M and ota_M, sets boot partition to ota_M.
        _, running = boot_and_running(dut)
        dut.write(f"ota_url {bundle_server.url('ok')}")
        assert ota_result(dut) == "ESP_OK"
        dut.expect(CONSOLE_READY, timeout=BOOT_TIMEOUT_S)
        assert boot_and_running(dut) == (1 - running, 1 - running)
        assert serves_marker(dut, bundle_server.marker)

    def test_ota_web_sha_mismatch_aborts_without_changing_the_boot_partition(
        self, dut, bundle_server
    ):
        # On web SHA-256 mismatch: aborts without changing the boot partition.
        _, running = boot_and_running(dut)
        dut.write(f"ota_url {bundle_server.url('bad_sha')}")
        assert ota_result(dut) == "ESP_ERR_INVALID_CRC"
        assert_pair_unchanged(dut, running, bundle_server.marker)

    def test_ota_truncated_p4_image_aborts_without_changing_the_boot_partition(
        self, dut, bundle_server
    ):
        # On invalid P4 image: aborts without changing the boot partition. The body is complete as HTTP but the
        # app image is cut short, so only esp_ota_end's image validation can catch it.
        _, running = boot_and_running(dut)
        dut.write(f"ota_url {bundle_server.url('truncated_app')}")
        assert ota_result(dut) != "ESP_OK"
        assert_pair_unchanged(dut, running, bundle_server.marker)

    def test_ota_second_concurrent_update_is_rejected_with_invalid_state(
        self, dut, bundle_server
    ):
        # On second concurrent update: rejects with ESP_ERR_INVALID_STATE; the first one is not disturbed until the
        # server drops it.
        _, running = boot_and_running(dut)
        dut.write(f"ota_url_bg {bundle_server.url('stalled')}")
        assert bundle_server.stalled.wait(60), "the first update never started"
        time.sleep(1)  # the first update is inside the web image by now
        dut.write("ota_probe")
        dut.expect(rb"ota_probe: ESP_ERR_INVALID_STATE", timeout=CONSOLE_TIMEOUT_S)
        dut.write(f"ota_url_bg {bundle_server.url('ok')}")
        assert ota_result(dut) == "ESP_ERR_INVALID_STATE"
        bundle_server.release()
        assert (
            ota_result(dut) != "ESP_OK"
        )  # the stalled one ends with the dropped connection
        assert_pair_unchanged(dut, running, bundle_server.marker)

    def test_ota_reset_before_commit_keeps_running_pair(
        self, dut, bundle_server, reboot
    ):
        # Review focus 1: reset after both parts are written and verified but before esp_ota_set_boot_partition.
        # The board boots the previous pair (ota + web) unchanged.
        _, running = boot_and_running(dut)
        send_until_ack(dut, "ota_commit_delay 60000", rb"ota_commit_delay: ok")
        dut.write(f"ota_url_bg {bundle_server.url('ok')}")
        dut.expect(
            rb"BR_OTA: test build: committing in 60000 ms", timeout=OTA_TIMEOUT_S
        )
        reboot()
        dut.expect(CONSOLE_READY, timeout=BOOT_TIMEOUT_S)
        assert_pair_unchanged(dut, running, bundle_server.marker)
        # the board is not wedged: the next update, without the delay, succeeds (once Ethernet has an address again)
        wait_reachable(dut.expect(GOT_IP, timeout=BOOT_TIMEOUT_S).group(1).decode())
        dut.write(f"ota_url {bundle_server.url('ok')}")
        assert ota_result(dut) == "ESP_OK"
        dut.expect(CONSOLE_READY, timeout=BOOT_TIMEOUT_S)
        assert boot_and_running(dut) == (1 - running, 1 - running)

    def test_ota_client_disconnect_aborts_cleanly(self, dut, bundle_server):
        # Review focus 2: the server closes the connection halfway through the body. The update aborts, the boot
        # partition is unchanged, and the next upload succeeds.
        _, running = boot_and_running(dut)
        dut.write(f"ota_url {bundle_server.url('disconnect')}")
        assert ota_result(dut) == "ESP_FAIL"
        assert_pair_unchanged(dut, running, bundle_server.marker)
        dut.write(f"ota_url {bundle_server.url('ok')}")
        assert ota_result(dut) == "ESP_OK"
        dut.expect(CONSOLE_READY, timeout=BOOT_TIMEOUT_S)
        assert boot_and_running(dut) == (1 - running, 1 - running)
