"""Thread console: the board's OpenThread CLI over GET /device/console/ws, its setting, and the USB `ot` command that
shares the same arbiter (spec 2026-11-04-thread-console). Hardware-gated: run in the batched on-board pass.

Rig: the board alone, running as a border router with a stored dataset. No Thread peer is needed.
"""

import re

from conftest import ot, wait_ot_state

RUNNING_ROLES = ("leader", "router", "child")
STATE_TIMEOUT_S = 120
USB_TIMEOUT_S = 10


def test_usb_ot_command_still_works(dut, board_ip):
    wait_ot_state(dut, RUNNING_ROLES, STATE_TIMEOUT_S)
    lines = ot(dut, "state")
    assert lines[-1] in RUNNING_ROLES


def test_usb_ot_command_times_out(dut, board_ip):
    wait_ot_state(dut, RUNNING_ROLES, STATE_TIMEOUT_S)
    mleid = ot(dut, "ipaddr mleid")[0]
    # Five pings 3 s apart: the prompt comes after about 12 s, past the command's 10 s wait.
    dut.write(f"ot ping {mleid} 8 5 3")
    dut.expect(
        re.compile(rb"No prompt after 10 s: \"ping [^\"]+\" is still running"),
        timeout=USB_TIMEOUT_S + 5,
    )
    dut.expect(re.compile(rb"5 packets transmitted, 5 packets received"), timeout=10)
    dut.expect(re.compile(rb"\r?\nDone\r?\n"), timeout=5)
    assert ot(dut, "state")[-1] in RUNNING_ROLES
