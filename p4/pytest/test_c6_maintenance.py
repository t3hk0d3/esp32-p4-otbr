"""The built-in RCP's maintenance through the orchestrator (builtin_rcp_push_firmware, radio_mux_maintain's wait).

What the move out of br_core changed on the board: the push answers once the mode on the C6 runs again (or says it
does not), a push after an abandoned OpenThread start answers restart_failed without a reboot, a channel that faulted
before its first bind shows Thread's state, and Thread starts after a push asked right after the boot steps.

The push tests need a good RCP image (env OTBR_RCP_IMAGE, e.g. c6_rcp/build/c6_rcp.bin) and skip without it; each push
is a C6 flash write.
"""

import os
import time
from pathlib import Path

import pytest
import requests
from conftest import GOT_IP, wait_reachable

BOOT_TIMEOUT_S = 60
PUSH_TIMEOUT_S = 240
RUNNING_TIMEOUT_S = 180
STATE_TIMEOUT_S = 60
POLL_S = 0.2
NO_REBOOT_WAIT_S = 5
C6 = "builtin-c6"
ABANDONED_REASON = "an earlier OpenThread start was abandoned: reboot"
RESTART_PREFIX = "the image is installed, but OpenThread did not start on it: "


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


@pytest.fixture
def good_image():
    path = os.environ.get("OTBR_RCP_IMAGE")
    if not path or not Path(path).is_file():
        pytest.skip("OTBR_RCP_IMAGE does not point to an RCP image")
    return Path(path).read_bytes()


def get_json(base_url, path):
    reply = requests.get(base_url + path, timeout=10)
    reply.raise_for_status()
    return reply.json()


def c6_channel(base_url):
    return next(
        c for c in get_json(base_url, "/device/channels")["channels"] if c["id"] == C6
    )


def thread_mode(base_url):
    return next(
        m
        for m in get_json(base_url, "/device/channels")["modes"]
        if m["mode"] == "otbr"
    )


def push(base_url, image):
    return requests.post(
        base_url + "/device/radio/firmware", data=image, timeout=PUSH_TIMEOUT_S
    )


def put_c6(base_url, body):
    return requests.put(f"{base_url}/device/channels/{C6}", json=body, timeout=60)


def wait_for(predicate, timeout):
    """Polls `predicate` until it is true or `timeout` passes; returns its last value."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            value = predicate()
        except requests.RequestException:
            value = None
        if value or time.monotonic() >= deadline:
            return value
        time.sleep(POLL_S)


def wait_mode(base_url, mode, timeout=RUNNING_TIMEOUT_S):
    return wait_for(
        lambda: get_json(base_url, "/device/status")["mode"] == mode, timeout
    )


def reboot_and_reach(dut, reboot):
    reboot()
    address = dut.expect(GOT_IP, timeout=BOOT_TIMEOUT_S).group(1).decode()
    wait_reachable(address)


def test_boot_binds_thread_once_after_the_boot_steps(dut, reboot, base_url):
    # On a boot with Thread stored on the C6: the C6's boot steps end ready, Thread gets one BIND and no BACK, and the
    # board reaches BR_RUNNING.
    reboot()
    before_end = dut.expect(
        rb"builtin-c6: reinit \(boot steps\) ended: ready",
        timeout=BOOT_TIMEOUT_S,
        return_what_before_match=True,
    )
    after_end = dut.expect(
        rb"mode_fsm: mode BR_RUNNING",
        timeout=RUNNING_TIMEOUT_S,
        return_what_before_match=True,
    )
    log = before_end + after_end
    assert log.count(b"builtin-c6: otbr bind") == 1, log[-2000:]
    assert b"builtin-c6: otbr back" not in log


def test_empty_probe_shows_thread_fault_on_the_c6(dut, send, reboot, base_url):
    # On a boot whose probe finds no RCP firmware (rcp_probe_override empty): the C6 is in fault, and the read API shows
    # Thread's state there as fault (it was never bound), as /device/status's RADIO_FAULT.
    send("rcp_probe_override empty", rb"rcp_probe_override: ok")
    try:
        reboot_and_reach(dut, reboot)
        assert wait_mode(base_url, "RADIO_FAULT", STATE_TIMEOUT_S)
        channel = c6_channel(base_url)
        assert channel["channel_state"] == "fault", channel
        assert channel["mode_state"] == "fault", channel
        thread = thread_mode(base_url)
        assert thread["channel"] == C6, thread
        assert thread["state"] == "fault", thread
    finally:
        send("rcp_probe_override real", rb"rcp_probe_override: ok")
        reboot_and_reach(dut, reboot)
        wait_mode(base_url, "BR_RUNNING")


def test_push_after_an_abandoned_start_answers_restart_failed_without_reboot(
    dut, send, reboot, base_url, good_image
):
    # On a push whose OpenThread start afterwards is abandoned (ot_start_abandon): 502 restart_failed with the whole
    # abandoned-start text, RADIO_FAULT, and no reboot.
    assert wait_mode(base_url, "BR_RUNNING")
    send(f"ot_start_abandon {C6}", rb"ot_start_abandon: ok")
    try:
        uptime = get_json(base_url, "/device/diag")["uptime_s"]
        reply = push(base_url, good_image)
        assert reply.status_code == requests.codes.bad_gateway, reply.text
        assert reply.json()["error"] == "restart_failed", reply.text
        assert reply.json()["detail"] == RESTART_PREFIX + ABANDONED_REASON, reply.text
        status = get_json(base_url, "/device/status")
        assert status["mode"] == "RADIO_FAULT", status
        assert status["fault_reason"] == ABANDONED_REASON, status
        time.sleep(NO_REBOOT_WAIT_S)
        assert get_json(base_url, "/device/diag")["uptime_s"] > uptime, (
            "the board rebooted"
        )
    finally:
        reboot_and_reach(dut, reboot)  # an abandoned start lasts until a reboot
        wait_mode(base_url, "BR_RUNNING")


def test_push_with_the_proxy_on_the_c6_answers_ok(base_url, good_image):
    # On a push while rcp-proxy (no peer) holds the C6: 200 once the proxy listens again.
    assert wait_mode(base_url, "BR_RUNNING")
    reply = put_c6(base_url, {"mode": "rcp-proxy"})
    assert reply.status_code in (requests.codes.ok, requests.codes.accepted), reply.text
    try:
        assert wait_for(
            lambda: c6_channel(base_url)["mode_state"] == "listening", STATE_TIMEOUT_S
        )
        reply = push(base_url, good_image)
        assert reply.status_code == requests.codes.ok, reply.text
        assert c6_channel(base_url)["mode_state"] == "listening"
    finally:
        put_c6(base_url, {"mode": "off"})
        put_c6(base_url, {"mode": "otbr"})
        wait_mode(base_url, "BR_RUNNING")


def test_push_right_after_the_boot_steps_still_lets_thread_start(
    dut, reboot, base_url, good_image
):
    # On a push sent as soon as the C6's boot steps ended: refused while Thread starts (409 busy) or done, and Thread
    # reaches BR_RUNNING either way. It does not force a REINIT onto a BIND Thread has not taken yet: device_api refuses
    # a push while Thread on the C6 is STARTING, so HTTP cannot reach that overlap. The host tests pin it
    # (mode_fsm_inbox: "On a REINIT after an unhandled BIND: both are taken fresh").
    reboot_and_reach(dut, reboot)
    assert wait_for(
        lambda: c6_channel(base_url)["channel_state"] == "ready", BOOT_TIMEOUT_S
    )
    reply = push(base_url, good_image)
    assert reply.status_code in (requests.codes.ok, requests.codes.conflict), reply.text
    if reply.status_code == requests.codes.conflict:
        assert reply.json()["error"] == "busy", reply.text
    assert wait_mode(base_url, "BR_RUNNING"), get_json(base_url, "/device/status")
