"""sniffer: pcap capture through GET /device/sniffer.pcap and DELETE /device/sniffer (Task 15 replaced the debug console
commands). Hardware-gated: deferred to V2.

The Thread peer is only needed by the tshark test: an idle channel still yields a valid pcap (global header, maybe no
frames).
"""

import shutil
import struct
import subprocess
import threading
import time

import pytest
import requests
from conftest import PCAP_GLOBAL_HEADER_LEN, start_capture, stop_capture

BOOT_TIMEOUT_S = 60
CHANNEL = 15
DURATION_S = 3
CYCLES = 20
BACK_TO_BR_S = 2
LEADER_TIMEOUT_S = 60
RECOVERY_S = 30
KILL_RELEASE_DELAY_S = 2
PCAP_MAGIC = 0xA1B2C3D4
LINKTYPE_IEEE802_15_4_TAP = 283
STREAM_TIMEOUT_S = 60
TSHARK_CAPTURE_S = 20
PEER_PINGS = 6


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


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
        time.sleep(0.2)
    pytest.fail(f"mode is {current}, expected {mode}")


def wait_for_leader(base_url):
    """Poll GET /node/state until it is leader (or LEADER_TIMEOUT_S passed); return the last state."""
    deadline = time.monotonic() + LEADER_TIMEOUT_S
    state = None
    while time.monotonic() < deadline:
        try:
            response = requests.get(f"{base_url}/node/state", timeout=10)
            state = (
                response.json() if response.status_code == requests.codes.ok else None
            )
        except requests.ConnectionError:  # the board may not answer ARP yet
            state = None
        if state == "leader":
            break
        time.sleep(1)
    return state


def peer_traffic(peer):
    """Make the peer transmit while the capture runs: multicast pings leave as frames whoever listens."""
    for _ in range(PEER_PINGS):
        peer.command("ping ff03::1", timeout=10)
        time.sleep(2)


def pcap_header(data):
    magic, _major, _minor, _tz, _sigfigs, _snaplen, linktype = struct.unpack(
        "<IHHiIII", data[:PCAP_GLOBAL_HEADER_LEN]
    )
    return magic, linktype


@pytest.fixture
def capture_stopped(base_url):
    yield
    # Leave the board in BR_RUNNING whatever the test did (best effort).
    stop_capture(base_url)


class TestSniffer:
    """sniffer"""

    def test_capture_streams_a_pcap_and_returns_to_br_mode(
        self, base_url, capture_stopped
    ):
        # On stop or duration elapsed: stops capture and requests BR mode; the stream is a pcap (magic a1b2c3d4,
        # linktype 283) on the requested channel.
        response = requests.get(
            f"{base_url}/device/sniffer.pcap",
            params={"channel": CHANNEL, "duration": DURATION_S},
            timeout=STREAM_TIMEOUT_S,
        )
        assert response.status_code == requests.codes.ok
        assert response.headers["Content-Type"] == "application/vnd.tcpdump.pcap"
        assert pcap_header(response.content) == (
            PCAP_MAGIC,
            LINKTYPE_IEEE802_15_4_TAP,
        )
        stats = status(base_url)["sniffer"]
        assert stats["channel"] == CHANNEL
        assert not stats["active"]
        assert not stats["ended_by_fault"]
        assert stats["elapsed_s"] >= DURATION_S
        wait_for_mode(base_url, "BR_RUNNING", BACK_TO_BR_S)

    def test_start_while_active_is_refused(self, base_url, capture_stopped):
        # On start while active: returns 409.
        stream = start_capture(base_url, CHANNEL)
        try:
            second = requests.get(
                f"{base_url}/device/sniffer.pcap",
                params={"channel": CHANNEL},
                timeout=10,
            )
            assert second.status_code == requests.codes.conflict
        finally:
            stop_capture(base_url, stream)

    def test_stop_returns_to_br_mode(self, base_url):
        # On stop: stops capture and requests BR mode.
        stream = start_capture(base_url, CHANNEL)
        assert status(base_url)["mode"] == "SNIFFER"
        assert (
            requests.delete(f"{base_url}/device/sniffer", timeout=30).status_code
            == requests.codes.ok
        )
        assert len(stream.content) >= PCAP_GLOBAL_HEADER_LEN  # the stream ends
        wait_for_mode(base_url, "BR_RUNNING", BACK_TO_BR_S)
        assert not status(base_url)["sniffer"]["active"]
        stream.close()

    def test_rcp_failure_ends_the_capture_and_flags_it(
        self, dut, base_url, send, capture_stopped
    ):
        # On the RCP failing while capturing (R17): the sniffer ends by itself (active false, ended_by_fault true), the
        # stream ends with the terminating chunk, and the device ends in RADIO_FAULT (the hold keeps the C6 in reset;
        # the teardown hook clears it).
        stream = start_capture(base_url, CHANNEL)
        send("rcp_kill", rb"rcp_kill: ESP_OK")
        dut.expect(rb"mode_fsm: mode RADIO_FAULT", timeout=30)
        # iter_content raises ChunkedEncodingError unless the terminating chunk arrived: a clean end, promptly.
        body = b"".join(stream.iter_content(chunk_size=None))
        assert len(body) >= PCAP_GLOBAL_HEADER_LEN
        stats = status(base_url)["sniffer"]
        assert not stats["active"]
        assert stats["ended_by_fault"]

    def test_rcp_reset_while_capturing_is_survived(
        self, stored_dataset, base_url, send, capture_stopped
    ):
        # On the RCP resetting by itself while capturing: OpenThread restores it (no radio fault); the capture keeps
        # running, and stopping it returns to BR_RUNNING with Thread restored (leader).
        assert wait_for_leader(base_url) == "leader"
        stream = start_capture(base_url, CHANNEL)
        send("rcp_glitch", rb"rcp_glitch: ok")
        time.sleep(RECOVERY_S / 3)
        stats = status(base_url)
        assert stats["mode"] == "SNIFFER"
        assert stats["sniffer"]["active"]
        assert not stats["sniffer"]["ended_by_fault"]
        stop_capture(base_url, stream)
        wait_for_mode(base_url, "BR_RUNNING", BACK_TO_BR_S)
        assert wait_for_leader(base_url) == "leader"

    def test_rcp_failure_that_recovers_ends_the_capture_and_returns_to_the_leader(
        self, stored_dataset, base_url, send, capture_stopped
    ):
        # On the RCP failing while capturing and the C6 answering the recovery reset (the hold is lifted before it):
        # the capture ends flagged and the device returns to BR_RUNNING with Thread restored (leader).
        assert wait_for_leader(base_url) == "leader"
        stream = start_capture(base_url, CHANNEL)
        send("rcp_kill", rb"rcp_kill: ESP_OK")
        time.sleep(
            KILL_RELEASE_DELAY_S
        )  # the failure is detected, the recovery reset has not run yet
        send("c6_hold 0", rb"c6_hold: ok")
        body = b"".join(stream.iter_content(chunk_size=None))
        assert len(body) >= PCAP_GLOBAL_HEADER_LEN
        assert status(base_url)["sniffer"]["ended_by_fault"]
        wait_for_mode(base_url, "BR_RUNNING", RECOVERY_S)
        assert wait_for_leader(base_url) == "leader"


class TestSnifferCycles:
    """sniffer, repeated use"""

    def test_twenty_start_stop_and_disconnect_cycles_keep_the_leader(
        self, stored_dataset, base_url, capture_stopped
    ):
        # 20 cycles alternating DELETE and client disconnect: every cycle is back in BR_RUNNING within 2 s and
        # /node/state ends as leader.
        for cycle in range(CYCLES):
            stream = start_capture(base_url, CHANNEL)
            assert status(base_url)["mode"] == "SNIFFER", cycle
            if cycle % 2 == 0:
                assert (
                    requests.delete(
                        f"{base_url}/device/sniffer", timeout=30
                    ).status_code
                    == requests.codes.ok
                ), cycle
            stream.close()  # disconnect (after DELETE: nothing left to disconnect from)
            wait_for_mode(base_url, "BR_RUNNING", BACK_TO_BR_S)
        deadline = time.monotonic() + LEADER_TIMEOUT_S
        state = None
        while time.monotonic() < deadline:
            response = requests.get(f"{base_url}/node/state", timeout=10)
            state = (
                response.json() if response.status_code == requests.codes.ok else None
            )
            if state == "leader":
                break
            time.sleep(2)
        assert state == "leader"

    @pytest.mark.skipif(
        shutil.which("tshark") is None, reason="tshark is not installed"
    )
    def test_capture_shows_frames_from_the_thread_peer(
        self, stored_dataset, base_url, peer, tmp_path, capture_stopped
    ):
        # The capture parses with tshark and shows frames from the Thread peer (the channel defaults to the
        # active dataset's channel).
        assert (
            wait_for_leader(base_url) == "leader"
        )  # the peer must find a network to join
        peer.command(f"dataset set active {stored_dataset}")
        peer.command("ifconfig up")
        peer.command("thread start")
        peer.wait_state({"child", "router"}, 120)
        channel = int(peer.command("channel")[-1])
        traffic = threading.Thread(target=peer_traffic, args=(peer,), daemon=True)
        traffic.start()
        response = requests.get(
            f"{base_url}/device/sniffer.pcap",
            params={"duration": TSHARK_CAPTURE_S},
            timeout=STREAM_TIMEOUT_S,
        )
        traffic.join()
        assert response.status_code == requests.codes.ok
        path = tmp_path / "capture.pcap"
        path.write_bytes(response.content)
        # Every frame carries the channel and RSS TAP fields, and none has a trailing FCS that Wireshark rejects
        # (the RCP's two trailing bytes are RSSI/LQI, not an FCS: they must not reach the pcap as one).
        result = subprocess.run(
            [
                "tshark",
                "-r",
                str(path),
                "-Y",
                "wpan",
                "-T",
                "fields",
                "-e",
                "wpan-tap.ch_num",
                "-e",
                "wpan-tap.rss",
            ],
            capture_output=True,
            text=True,
            timeout=60,
            check=True,
        )
        rows = [line.split("\t") for line in result.stdout.splitlines() if line]
        assert rows, "no 802.15.4 frames in the capture"
        assert all(row[0] == str(channel) for row in rows), rows
        assert all(row[1] for row in rows), rows
        bad_fcs = subprocess.run(
            [
                "tshark",
                "-r",
                str(path),
                "-Y",
                'wpan && _ws.expert.message contains "FCS"',
            ],
            capture_output=True,
            text=True,
            timeout=60,
            check=True,
        )
        assert not bad_fcs.stdout.strip(), bad_fcs.stdout
