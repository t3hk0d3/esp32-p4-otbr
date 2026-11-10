"""Hybrid mode: Thread (OTBR) on one radio and the Zigbee coordinator on the other, at once.
Hardware-gated: written for the batched on-board pass, never run where it was written.

Rig: the board on the LAN with the built-in C6 and a spinel dongle on the USB-A port (`-m usb_dongle`). Thread keeps the
board's stored dataset: no test here forms a Thread network (RF rule: never 11, 15 or 25; Zigbee forms on TEST_CHANNEL
20). The unplug test needs a person (`-s -m manual_unplug`); the push test needs OTBR_RCP_IMAGE, as test_rcp_push.

Spec: docs/superpowers/specs/2026-11-10-hybrid-mode-design.md. Every test puts the board back as it found it: Thread on
the C6, the dongle off (fixture `hybrid`).
"""

import asyncio
import os
import re
import time

import pytest
import requests
from test_zigbee import (
    C6,
    EXPECTED_STATE,
    OTBR_MODE,
    PUSH_TIMEOUT_S,
    RUNNING_TIMEOUT_S,
    ZIGBEE_MODE,
    channel_id,
    channel_mode,
    get_json,
    run,
    set_mode,
    wait_mode,
    wait_rebooted,
    wait_state,
    zigbee_status,
    zigpy_app,
)

zigpy_types = pytest.importorskip("zigpy.types")

pytestmark = pytest.mark.usb_dongle


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


THREAD_ON_C6 = "thread_c6"  # Thread on the C6, Zigbee on the dongle
THREAD_ON_USB = "thread_usb"  # Thread on the dongle, Zigbee on the C6
ASSIGNMENTS = [THREAD_ON_C6, THREAD_ON_USB]
LOAD_S = 60
PING_COUNT = 20
PING_TIMEOUT_S = PING_COUNT + 20
REBOOT_BACK_S = 60
SCAN_TIMEOUT_S = 30
PING_SUMMARY = re.compile(rb"(\d+) packets transmitted, (\d+) packets received")
RLOC_ADDRESS = re.compile(r"[0-9a-f:]+:0:ff:fe00:[0-9a-f]{1,4}")


def status(base_url):
    return get_json(base_url, "/device/status")


def wait_thread(base_url, channel, timeout=RUNNING_TIMEOUT_S):
    """Until /device/status says BR_RUNNING on `channel`; fails with the last status."""
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            last = status(base_url)
            if last["mode"] == "BR_RUNNING" and last["otbr_channel"] == channel:
                return last
        except requests.RequestException:
            pass
        time.sleep(1)
    pytest.fail(f"Thread never ran on {channel}; last status {last}")


def restore(dut, base_url, usb):
    """Back to rest: the radio running Zigbee leaves it first (a reboot; otbr on it moves Thread back to the C6 when
    that radio is the C6), then the dongle off and Thread on the C6."""
    for channel in (C6, usb):
        if channel_mode(base_url, channel) == ZIGBEE_MODE:
            set_mode(dut, base_url, channel, OTBR_MODE if channel == C6 else "off")
    if channel_mode(base_url, usb) != "off":
        set_mode(dut, base_url, usb, "off")
        wait_mode(base_url, usb, "off")
    if channel_mode(base_url, C6) != OTBR_MODE:
        set_mode(dut, base_url, C6, OTBR_MODE)
    wait_thread(base_url, C6)
    assert status(base_url)["zigbee_channel"] is None


@pytest.fixture
def hybrid(dut, base_url):
    """hybrid(assignment): Thread on one radio, Zigbee on the other; returns (thread_channel, zigbee_channel).
    Restored to rest (Thread on the C6, the dongle off) when the test ends, on every path."""
    usb = channel_id(base_url, "usb")

    def enter(assignment):
        thread, zigbee = (C6, usb) if assignment == THREAD_ON_C6 else (usb, C6)
        if channel_mode(base_url, thread) != OTBR_MODE:
            set_mode(
                dut, base_url, thread, OTBR_MODE
            )  # a move off the C6: 202, stored once it ran
            wait_mode(base_url, thread, OTBR_MODE)
        set_mode(dut, base_url, zigbee, ZIGBEE_MODE)  # entering Zigbee: a reboot
        wait_thread(base_url, thread)
        wait_state(base_url, EXPECTED_STATE, RUNNING_TIMEOUT_S)
        return thread, zigbee

    yield enter
    restore(dut, base_url, usb)


def thread_router(base_url):
    """The RLOC address of a Thread router other than the board (from /topology), or a skip when there is none."""
    own = (
        requests.get(f"{base_url}/node/rloc", timeout=5).text.strip().strip('"').lower()
    )
    text = requests.get(f"{base_url}/topology", timeout=15).text.lower()
    for address in sorted(set(RLOC_ADDRESS.findall(text))):
        if address != own and not address.endswith(
            ":fc00"
        ):  # fc00: the leader's anycast locator
            return address
    pytest.skip("no other Thread router on the network to ping")


def ping_loss(dut, address):
    """`ot ping` from the board's console: the number of lost replies out of PING_COUNT."""
    dut.write(f"ot ping {address} 32 {PING_COUNT} 0.5")
    m = dut.expect(PING_SUMMARY, timeout=PING_TIMEOUT_S)
    return int(m.group(1)) - int(m.group(2))


@pytest.mark.parametrize("assignment", ASSIGNMENTS)
def test_both_run(hybrid, base_url, record_property, assignment):
    """Thread on one radio and Zigbee on the other: the board says BR_RUNNING on Thread's radio, names Zigbee's radio,
    and Zigbee runs. The internal heap is recorded against the spec's section 2."""
    thread, zigbee = hybrid(assignment)
    s = status(base_url)
    assert s["mode"] == "BR_RUNNING"
    assert s["otbr_channel"] == thread
    assert s["zigbee_channel"] == zigbee
    assert zigbee_status(base_url)["state"] == EXPECTED_STATE
    memory = get_json(base_url, "/device/diag")["memory"]["internal"]
    record_property("internal_free", memory["free"])
    record_property("internal_min_free", memory["min_free"])


def test_zigbee_load_leaves_thread_pings(hybrid, base_url, dut):
    """Energy scans through a zigpy client for LOAD_S while the board pings a Thread router: no scan fails and no
    ping is lost."""
    hybrid(THREAD_ON_C6)
    router = thread_router(base_url)
    lost = []

    async def body():
        async with zigpy_app(base_url.removeprefix("http://")) as app:
            end = time.monotonic() + LOAD_S
            while time.monotonic() < end:
                scan = await app.energy_scan(
                    channels=zigpy_types.Channels.ALL_CHANNELS, duration_exp=2, count=1
                )
                assert len(scan) == 16
                lost.append(ping_loss(dut, router))

    run(body())
    assert lost and sum(lost) == 0, lost


def test_reboot_keeps_both(hybrid, base_url, dut):
    """A reboot brings Thread back on its radio and Zigbee on the other."""
    thread, zigbee = hybrid(THREAD_ON_C6)
    requests.post(f"{base_url}/device/reboot", timeout=10)
    wait_rebooted(dut, base_url)
    wait_thread(base_url, thread, REBOOT_BACK_S)
    wait_state(base_url, EXPECTED_STATE, REBOOT_BACK_S)
    assert status(base_url)["zigbee_channel"] == zigbee


def test_otbr_onto_zigbees_radio_moves_through_a_reboot(hybrid, base_url, dut):
    """otbr on the dongle that runs Zigbee while Thread runs on the C6: the PUT answers reboot, and afterwards Thread
    runs on the dongle, the C6 is off and no radio runs Zigbee."""
    _, zigbee = hybrid(THREAD_ON_C6)
    body = set_mode(dut, base_url, zigbee, OTBR_MODE)
    assert body.get("reboot") is True
    wait_thread(base_url, zigbee)
    assert channel_mode(base_url, C6) == "off"
    assert status(base_url)["zigbee_channel"] is None


def test_c6_push_keeps_zigbee_on_the_dongle(hybrid, base_url):
    """A C6 firmware push with Thread on the C6 and Zigbee on the dongle: Zigbee stays running with its client."""
    path = os.environ.get("OTBR_RCP_IMAGE")
    if not path or not os.path.isfile(path):
        pytest.skip("OTBR_RCP_IMAGE does not point to an RCP image")
    hybrid(THREAD_ON_C6)
    with open(path, "rb") as f:
        image = f.read()

    def push(data):
        return requests.post(
            f"{base_url}/device/radio/firmware", data=data, timeout=PUSH_TIMEOUT_S
        )

    async def body():
        async with zigpy_app(base_url.removeprefix("http://")):
            before = zigbee_status(base_url)["client"]
            reply = await asyncio.get_running_loop().run_in_executor(None, push, image)
            after = zigbee_status(base_url)
            return reply, before, after

    reply, before, after = run(body())
    assert reply.status_code == requests.codes.ok, reply.text
    assert after["state"] == EXPECTED_STATE
    assert (
        after["client"] is not None and after["client"]["address"] == before["address"]
    )
    wait_thread(base_url, C6)


@pytest.mark.manual_unplug
def test_dongle_unplug_leaves_thread(hybrid, base_url):
    """With Thread on the C6 and Zigbee on the dongle, a person unplugs and replugs the dongle: Thread stays
    BR_RUNNING throughout; Zigbee shows no_radio while it is out and runs again after the replug."""
    hybrid(THREAD_ON_C6)
    print("\nUnplug the USB dongle now.")
    deadline = time.monotonic() + 120
    while zigbee_status(base_url)["state"] != "no_radio":
        assert time.monotonic() < deadline, "the dongle was not unplugged within 120 s"
        assert status(base_url)["mode"] == "BR_RUNNING"
        time.sleep(1)
    print("Plug it back in now.")
    deadline = time.monotonic() + 180
    while zigbee_status(base_url)["state"] != EXPECTED_STATE:
        assert time.monotonic() < deadline, (
            "Zigbee did not run again within 180 s of the unplug"
        )
        assert status(base_url)["mode"] == "BR_RUNNING"
        time.sleep(1)
