"""GET/POST /device/update against a local release server (BDD: device_api endpoints, self-update Task 5).

The test build's console command `update_source <base>` points the board's releases base at an http.server on the
host. It serves a `releases/` tree laid out like GitHub's:
  releases/latest/download/versions.txt
  releases/download/<tag>/versions.txt, <board_id>_<chip_id>.ota (the name the board reports in GET /device/update), c6_rcp.bin
The files are what `make bundle` / `make package` put into dist/, built here from the running test build: a release
build (`make package` refuses test builds) would drop the test console commands after the first install. The bundle's
app is re-stamped with the version versions.txt names, so a successful install boots that version; c6_rcp.bin is the
build's own radio firmware, re-stamped where a test needs one that differs. Runs in the hardware pass only.
"""

# ruff: noqa: F811  (fixtures re-exported from test_device_api are used as parameters)
import threading
import time
from datetime import datetime, timedelta
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest
import requests
from conftest import GOT_IP, send_until_ack, wait_reachable
from rcp_image import retag, version_of
from test_device_api import (
    assert_error_body,
    rcp_image,  # noqa: F401  (fixture re-export)
    status,
    wait_for_mode,
)
from test_ota import boot_and_running, host_address_towards, load_make_bundle

ENDPOINT = "/device/update"
TAG = "v9.9.9"
STAMP = "%Y%m%dT%H%M%SZ"
CHECK_TIMEOUT_S = 10
INSTALL_TIMEOUT_S = 300
BOOT_TIMEOUT_S = 60
CONFIRM_TIMEOUT_S = 90  # a freshly installed app is confirmed within 60 s; until then an update answers 409 busy
HOLD_TIMEOUT_S = 120
PUSH_TIMEOUT_S = 240
DEFAULT_RELEASES = "https://github.com/t3hk0d3/esp32-p4-otbr/releases"


def later_version(version, hours=1):
    """A well-formed version whose stamp is `hours` after the stamp of `version`."""
    stamp = datetime.strptime(version[:16], STAMP) + timedelta(hours=hours)
    return f"{stamp.strftime(STAMP)}-1234567"


class ReleaseServer:
    """http.server over a `releases/` tree. Counts the requests per path; `hold(path)` makes requests for that path
    wait until `release()` (they are answered normally afterwards: 404 when the file does not exist)."""

    def __init__(self, root, host, bundle_name):
        self.root = root
        self.bundle_name = (
            bundle_name  # the board's own target file, e.g. <board_id>_<chip_id>.ota
        )
        self.requests = {}
        self.held = threading.Event()  # set when a held request arrived
        self._hold_path = None
        self._release = threading.Event()
        server = self

        class Handler(SimpleHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                server.requests[self.path] = server.requests.get(self.path, 0) + 1
                if self.path == server._hold_path:
                    server.held.set()
                    server._release.wait(HOLD_TIMEOUT_S)
                super().do_GET()

        self.httpd = ThreadingHTTPServer(
            (host, 0), partial(Handler, directory=str(root))
        )
        self.base = f"http://{host}:{self.httpd.server_address[1]}/releases"
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def publish(self, app, rcp, bundle=None, rcp_bin=None, tag=TAG):
        """versions.txt as the latest release (with a `release:` line) and under its tag, plus the given assets."""
        text = f"release: {tag}\np4 app:      {app}\n"
        if rcp is not None:
            text += f"c6_rcp:      {rcp}\n"
        text += "ESP-IDF:     v6.1\n"
        for directory in (
            self.root / "releases" / "latest" / "download",
            self.root / "releases" / "download" / tag,
        ):
            directory.mkdir(parents=True, exist_ok=True)
            (directory / "versions.txt").write_text(text)
        assets = self.root / "releases" / "download" / tag
        for name, data in ((self.bundle_name, bundle), ("c6_rcp.bin", rcp_bin)):
            path = assets / name
            if data is None:
                path.unlink(missing_ok=True)
            else:
                path.write_bytes(data)

    @property
    def bundle_path(self):
        return f"/releases/download/{TAG}/{self.bundle_name}"

    def count(self, name):
        return sum(n for path, n in self.requests.items() if path.endswith(name))

    def hold(self, path):
        self._hold_path = path
        self.held.clear()
        self._release.clear()

    def release(self):
        self._hold_path = None
        self._release.set()

    def close(self):
        self.release()
        self.httpd.shutdown()
        self.httpd.server_close()


VERSIONS_PATH = "/releases/latest/download/versions.txt"


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


@pytest.fixture
def release_server(dut, board_ip, tmp_path):
    """A release server the board's update source points at (the default is restored afterwards)."""
    server = ReleaseServer(
        tmp_path,
        host_address_towards(board_ip),
        board_bundle_name(f"http://{board_ip}"),
    )
    point_board_at(dut, server.base)
    yield server
    server.close()
    point_board_at(dut, DEFAULT_RELEASES)


def board_bundle_name(base_url):
    """The release file this firmware downloads: `bundle` in GET /device/update."""
    name = requests.get(base_url + ENDPOINT, timeout=10).json()["bundle"]
    assert name.endswith(".ota"), name
    return name


def point_board_at(dut, base):
    send_until_ack(dut, f"update_source {base}", rb"update_source: \S+")


@pytest.fixture
def make_release_bundle(dut, tmp_path):
    """make_release_bundle(version): bundle of the running test build (web and app), its app re-stamped `version`."""
    build = Path(dut.app.binary_path)

    def make(version):
        app = tmp_path / f"p4-{version}.bin"
        app.write_bytes(retag((build / "p4.bin").read_bytes(), version))
        out = tmp_path / f"otbr-{version}.bundle"
        args = ["--web", str(build / "web_0.bin"), "--app", str(app), "--out", str(out)]
        assert load_make_bundle().main(args) == 0
        return out.read_bytes()

    return make


def get_update(base_url, **params):
    return requests.get(base_url + ENDPOINT, params=params, timeout=10)


def wait_check_done(base_url, timeout=CHECK_TIMEOUT_S):
    """Poll the plain GET until the state is no longer `checking`; return the body."""
    deadline = time.monotonic() + timeout
    body = None
    while time.monotonic() < deadline:
        body = get_update(base_url).json()
        if body["state"] != "checking":
            return body
        time.sleep(0.5)
    pytest.fail(f"still checking after {timeout} s: {body}")


def post_update(base_url, force=False):
    """POST /device/update, repeated while the running app is not confirmed yet (409 busy "not confirmed")."""
    params = {"force": "1"} if force else {}
    deadline = time.monotonic() + CONFIRM_TIMEOUT_S
    while True:
        response = requests.post(
            base_url + ENDPOINT, params=params, timeout=INSTALL_TIMEOUT_S
        )
        unconfirmed = (
            response.status_code == requests.codes.conflict
            and "not confirmed" in (response.json().get("detail") or "")
        )
        if not unconfirmed or time.monotonic() > deadline:
            return response
        time.sleep(5)


def post_in_background(base_url):
    """POST /device/update on a thread; returns (thread, result list holding the response once it arrived)."""
    result = []

    def run():
        result.append(requests.post(base_url + ENDPOINT, timeout=INSTALL_TIMEOUT_S))

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    return thread, result


def wait_address(dut):
    """After a reboot: the board's address once it accepts connections again."""
    address = dut.expect(GOT_IP, timeout=BOOT_TIMEOUT_S).group(1).decode()
    wait_reachable(address)
    return address


def wait_back(dut):
    """After a reboot: the base URL once the board has its address again."""
    return f"http://{wait_address(dut)}"


def running_versions(base_url):
    current = status(base_url)
    return current["app"]["version"], current["rcp"]["installed"]


class TestUpdateState:
    """device_api endpoints: GET /device/update"""

    def test_fresh_boot_reports_never_and_asks_nothing(self, dut, reboot, tmp_path):
        # Fresh boot: 200, state "never"; no request reaches the local server.
        reboot()
        address = wait_address(dut)
        base_url = f"http://{address}"
        server = ReleaseServer(
            tmp_path, host_address_towards(address), board_bundle_name(base_url)
        )
        try:
            point_board_at(dut, server.base)
            response = get_update(base_url)
            assert response.status_code == requests.codes.ok
            body = response.json()
            assert body["state"] == "never"
            assert body["checked_age_s"] is None
            assert body["release"] is None
            assert body["app"] is None
            assert body["available"] is False
            assert body["repo_url"] == "https://github.com/t3hk0d3/esp32-p4-otbr"
            time.sleep(2)
            assert server.requests == {}
        finally:
            server.close()
            point_board_at(dut, DEFAULT_RELEASES)

    def test_bad_check_value_is_400_invalid_request(self, base_url):
        # check=yes: 400 invalid_request.
        response = get_update(base_url, check="yes")
        assert response.status_code == requests.codes.bad_request
        assert_error_body(response)
        assert response.json()["error"] == "invalid_request"


class TestUpdateCheck:
    """device_api endpoints: GET /device/update?check=true"""

    def test_check_reports_checking_then_the_served_release(
        self, base_url, release_server
    ):
        # Server serves versions.txt: 200, state "checking"; within 10 s a plain GET shows state done, release as
        # served.
        app, rcp = running_versions(base_url)
        newer = later_version(app)
        release_server.publish(newer, rcp)
        response = get_update(base_url, check="true")
        assert response.status_code == requests.codes.ok
        assert response.json()["state"] == "checking"
        body = wait_check_done(base_url)
        assert body["state"] == "done"
        assert body["error"] is None
        assert body["checked_age_s"] is not None
        assert body["release"]["tag"] == TAG
        assert body["release"]["app"] == newer
        assert body["release"]["rcp"] == rcp
        assert body["release"]["url"] == f"{release_server.base}/tag/{TAG}"
        assert body["running"]["app"] == app
        assert body["app"] == "newer"
        assert body["rcp_differs"] is False
        assert body["available"] is True

    def test_second_check_joins_the_running_one(self, base_url, release_server):
        # Second check=true while the first runs: 200, state "checking"; the local server sees one versions.txt
        # request.
        app, rcp = running_versions(base_url)
        release_server.publish(later_version(app), rcp)
        release_server.hold(VERSIONS_PATH)
        assert get_update(base_url, check="true").json()["state"] == "checking"
        assert release_server.held.wait(CHECK_TIMEOUT_S), "the check never asked"
        second = get_update(base_url, check="true")
        assert second.status_code == requests.codes.ok
        assert second.json()["state"] == "checking"
        release_server.release()
        assert wait_check_done(base_url)["state"] == "done"
        assert release_server.count("versions.txt") == 1

    def test_server_down_fails_the_check_with_an_error(
        self, dut, base_url, release_server
    ):
        # Server down: a plain GET shows state failed, error set.
        dead = f"{release_server.base.rsplit(':', 1)[0]}:9/releases"  # discard port: nothing listens
        point_board_at(dut, dead)
        assert get_update(base_url, check="true").status_code == requests.codes.ok
        body = wait_check_done(base_url, timeout=30)
        assert body["state"] == "failed"
        assert isinstance(body["error"], str)
        assert body["error"]

    def test_check_during_an_install_is_409_busy(self, base_url, release_server):
        # During an install: 409 busy. The held bundle is then answered 404: the install fails and changes nothing.
        app, rcp = running_versions(base_url)
        release_server.publish(
            later_version(app), rcp
        )  # no bundle file: 404 once released
        release_server.hold(release_server.bundle_path)
        thread, result = post_in_background(base_url)
        assert release_server.held.wait(CHECK_TIMEOUT_S * 3), (
            "the install never asked for the bundle"
        )
        response = get_update(base_url, check="true")
        assert response.status_code == requests.codes.conflict
        assert response.json()["error"] == "busy"
        release_server.release()
        thread.join(INSTALL_TIMEOUT_S)
        assert result and result[0].status_code == 502
        assert result[0].json()["error"] == "transfer_failed"


class TestUpdateInstall:
    """device_api endpoints: POST /device/update"""

    def test_newer_release_installs_the_app_and_reboots_into_it(
        self, dut, base_url, release_server, make_release_bundle, rcp_image
    ):
        # Release newer, RCP equal to the installed one: 200 app installed, rcp unchanged, rebooting; the board comes
        # back on the new stamp.
        app, rcp = running_versions(base_url)
        newer = later_version(app)
        release_server.publish(
            newer, rcp, bundle=make_release_bundle(newer), rcp_bin=rcp_image
        )
        response = post_update(base_url)
        assert response.status_code == requests.codes.ok, response.text
        body = response.json()
        assert body["status"] == "ok"
        assert body["app"] == "installed"
        assert body["rcp"] == "unchanged"
        assert body["rebooting"] is True
        back = wait_back(dut)
        assert running_versions(back)[0] == newer
        assert release_server.count("c6_rcp.bin") == 0

    def test_same_release_with_another_radio_firmware_installs_only_the_radio_firmware(
        self, dut, base_url, release_server, rcp_image
    ):
        # Release same, RCP differs: 200 app unchanged, rcp installed, rebooting false; rcp.installed is the served
        # stamp.
        app, rcp = running_versions(base_url)
        served = later_version(rcp or version_of(rcp_image))
        release_server.publish(app, served, rcp_bin=retag(rcp_image, served))
        try:
            response = post_update(base_url)
            assert response.status_code == requests.codes.ok, response.text
            body = response.json()
            assert body["app"] == "unchanged"
            assert body["rcp"] == "installed"
            assert body["rebooting"] is False
            assert release_server.count(release_server.bundle_name) == 0
            assert running_versions(base_url)[1] == served
        finally:
            # Put the build's own radio firmware back.
            if running_versions(base_url)[1] != version_of(rcp_image):
                put_back = requests.post(
                    base_url + "/device/radio/firmware",
                    data=rcp_image,
                    timeout=PUSH_TIMEOUT_S,
                )
                assert put_back.status_code == requests.codes.ok
                wait_for_mode(base_url, "BR_RUNNING", PUSH_TIMEOUT_S)

    def test_nothing_to_do_is_409_up_to_date(self, base_url, release_server):
        # Nothing to do: 409 up_to_date.
        app, rcp = running_versions(base_url)
        release_server.publish(app, rcp)
        response = post_update(base_url)
        assert response.status_code == requests.codes.conflict
        assert_error_body(response)
        assert response.json()["error"] == "up_to_date"
        assert release_server.count(release_server.bundle_name) == 0

    def test_nothing_to_do_with_force_installs_the_app(
        self, dut, base_url, release_server, make_release_bundle, rcp_image
    ):
        # Nothing to do, force=1: 200 app installed.
        app, rcp = running_versions(base_url)
        release_server.publish(
            app, rcp, bundle=make_release_bundle(app), rcp_bin=rcp_image
        )
        response = post_update(base_url, force=True)
        assert response.status_code == requests.codes.ok, response.text
        assert response.json()["app"] == "installed"
        assert response.json()["rebooting"] is True
        assert running_versions(wait_back(dut))[0] == app

    def test_bundle_404_is_502_transfer_failed_and_changes_nothing(
        self, dut, base_url, release_server
    ):
        # Bundle URL answers 404: 502 transfer_failed; slot and boot partition unchanged.
        app, rcp = running_versions(base_url)
        before = boot_and_running(dut)
        release_server.publish(later_version(app), rcp)  # no bundle file
        response = post_update(base_url)
        assert response.status_code == 502
        assert_error_body(response)
        assert response.json()["error"] == "transfer_failed"
        assert (
            release_server.bundle_path in response.json()["detail"]
        )  # names the URL asked for
        assert boot_and_running(dut) == before
        assert running_versions(base_url)[0] == app
        # the slot was released: a check runs
        assert get_update(base_url, check="true").status_code == requests.codes.ok
        wait_check_done(base_url)

    def test_install_during_a_check_is_409_busy(self, base_url, release_server):
        # During a running check: 409 busy.
        app, rcp = running_versions(base_url)
        release_server.publish(later_version(app), rcp)
        release_server.hold(VERSIONS_PATH)
        assert get_update(base_url, check="true").status_code == requests.codes.ok
        assert release_server.held.wait(CHECK_TIMEOUT_S), "the check never asked"
        response = requests.post(base_url + ENDPOINT, timeout=10)
        assert response.status_code == requests.codes.conflict
        assert response.json()["error"] == "busy"
        release_server.release()
        assert wait_check_done(base_url)["state"] == "done"
        assert release_server.count(release_server.bundle_name) == 0
