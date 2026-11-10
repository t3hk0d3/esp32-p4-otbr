"""USB/IP page (BDD: UsbipLogic, UsbipPage). Runs static/usbip.js under node."""

import json

from device_js import logic, needs_node, page, scenario

OFF = {
    "enabled": False,
    "active": False,
    "pending_reboot": False,
    "state": "off",
    "reason": None,
    "port": 3240,
    "interface": "mgmt",
    "device": None,
    "client": None,
    "counters": {
        "sessions": 0,
        "refused": 0,
        "submits": 0,
        "completed": 0,
        "errors": 0,
        "unlinks": 0,
        "bytes_in": 0,
        "bytes_out": 0,
        "sessions_ended_overload": 0,
    },
    "memory": {
        "inflight_bytes": 0,
        "inflight_peak": 0,
        "waiting_bytes": 0,
        "waiting_peak": 0,
    },
    "last_error": None,
}
CP2102 = {
    "busid": "1-1",
    "vid": "10c4",
    "pid": "ea60",
    "bcd_device": "0100",
    "class": "00",
    "speed": "full",
    "manufacturer": "Silicon Labs",
    "product": "CP2102N USB to UART Bridge Controller",
    "serial": "abc123",
    "interfaces": 1,
    "endpoints": 2,
}
HUB = {
    **CP2102,
    "vid": "05e3",
    "pid": "0608",
    "product": "USB 2.0 Hub",
    "manufacturer": "Genesys Logic",
    "serial": "",
    "speed": "high",
}
COUNTERS = {
    "sessions": 3,
    "refused": 1,
    "submits": 12000,
    "completed": 11950,
    "errors": 2,
    "unlinks": 48,
    "bytes_in": 1048576,
    "bytes_out": 65536,
    "sessions_ended_overload": 0,
}
MEMORY = {
    "inflight_bytes": 4096,
    "inflight_peak": 16384,
    "waiting_bytes": 0,
    "waiting_peak": 2048,
}
NOW_S = 1800000000  # the clock of the scenarios, epoch seconds
ON = {**OFF, "enabled": True, "active": True}
NO_DEVICE = {**ON, "state": "no_device"}
EXPORTABLE = {
    **ON,
    "state": "exportable",
    "device": CP2102,
    "counters": COUNTERS,
    "memory": MEMORY,
}
ATTACHED = {
    **EXPORTABLE,
    "state": "attached",
    "client": {"addr": "192.168.10.20:51322", "since": NOW_S - 2 * 3600 - 14 * 60},
}
UNSUPPORTED = {
    **ON,
    "state": "unsupported",
    "device": HUB,
    "reason": "Hubs are not supported. Plug the device straight into the board.",
}
BLOCKED = {
    **OFF,
    "enabled": True,
    "state": "blocked",
    "reason": "usb-10c4-ea60-abc123 runs otbr",
}
PENDING_ON = {**OFF, "enabled": True, "pending_reboot": True}
PENDING_OFF = {**ATTACHED, "enabled": False, "pending_reboot": True}

EVIL = "<img src=x onerror=alert(1)>"
ALL_SHOWN = {"device": True, "client": True, "stats": True, "attach": True}
NONE_SHOWN = {"device": False, "client": False, "stats": False, "attach": False}

# t.status: the body GET /device/usbip answers; t.put(body) -> {status, body} answers a PUT.
ROUTES = """(u, o) => {
  if (u === '/device/usbip' && o.method === 'PUT')
    return t.put ? t.put(JSON.parse(o.body)) : {status: 200, body: {reboot: false}};
  if (u === '/device/usbip') return t.fail ? {status: 503, body: {}} : {status: 200, body: t.status};
  if (u === '/device/diag') { t.diag = (t.diag || 0) + 1; if (t.diag === 2) throw new Error('down'); return {status: 200, body: {uptime_s: t.diag}}; }
  if (u === '/device/network') return {status: 200, body: H.NETWORK};
  return {status: 200, body: {}};
}"""


def view(status, host="board.lan"):
    return logic("usbip", f"L.view({json.dumps(status)}, {json.dumps(host)})")


def setup(status=ATTACHED):
    return (
        page("usbip", ROUTES)
        + f"t.status = {json.dumps(status)};"
        + f"t.env.now = () => {NOW_S * 1000}; t.page.init(); await t.advance(10);"
    )


def puts():
    return "t.calls.filter((c) => c.method === 'PUT').map((c) => JSON.parse(c.body))"


@needs_node
class TestView:
    """UsbipLogic.view"""

    def test_off_shows_no_box(self):
        # On state off: chip "Off", plain; no box shows.
        v = view(OFF)
        assert v["chip"] == {"text": "Off", "cls": ""}
        assert v["show"] == NONE_SHOWN

    def test_blocked_shows_no_box(self):
        # On state blocked: chip "Not running" in red; no box shows.
        v = view(BLOCKED)
        assert v["chip"] == {"text": "Not running", "cls": "chip-bad"}
        assert v["show"] == NONE_SHOWN

    def test_turned_on_but_not_rebooted_says_so(self):
        # On enabled with the port not yet owned (state off): chip "Reboot pending", a warning; no box shows.
        v = view(PENDING_ON)
        assert v["chip"] == {"text": "Reboot pending", "cls": "chip-warn"}
        assert v["show"] == NONE_SHOWN

    def test_no_device_shows_the_boxes(self):
        # On state no_device: chip "No device" (warning); all four boxes show.
        v = view(NO_DEVICE)
        assert v["chip"] == {"text": "No device", "cls": "chip-warn"}
        assert v["show"] == ALL_SHOWN

    def test_unsupported_shows_the_boxes(self):
        # On state unsupported: chip "Cannot export" in red; all four boxes show.
        v = view(UNSUPPORTED)
        assert v["chip"] == {"text": "Cannot export", "cls": "chip-bad"}
        assert v["show"] == ALL_SHOWN

    def test_exportable_is_ready_to_attach(self):
        # On state exportable: chip "Ready to attach" in green; all four boxes show.
        v = view(EXPORTABLE)
        assert v["chip"] == {"text": "Ready to attach", "cls": "chip-ok"}
        assert v["show"] == ALL_SHOWN

    def test_attached_is_attached(self):
        # On state attached: chip "Attached" in green; all four boxes show.
        v = view(ATTACHED)
        assert v["chip"] == {"text": "Attached", "cls": "chip-ok"}
        assert v["show"] == ALL_SHOWN

    def test_commands_carry_the_boards_address(self):
        # On host board.lan: the Linux and Windows commands list and attach with that address and bus id 1-1.
        v = view(EXPORTABLE)
        for key in ("linux", "windows"):
            assert "usbip list -r board.lan" in v["commands"][key]
            assert "usbip attach -r board.lan -b 1-1" in v["commands"][key]

    def test_the_port_of_the_page_address_is_left_out(self):
        # On host 192.168.10.5:8080 and [fd00::1]:80: the address alone, IPv6 without brackets.
        assert (
            "usbip list -r 192.168.10.5\n"
            in view(EXPORTABLE, "192.168.10.5:8080")["commands"]["linux"]
        )
        assert (
            "usbip list -r fd00::1\n"
            in view(EXPORTABLE, "[fd00::1]:80")["commands"]["linux"]
        )

    def test_linux_loads_the_driver_and_windows_points_to_usbip_win2(self):
        # On the commands: Linux starts with modprobe vhci-hcd; Windows names usbip-win2.
        c = view(EXPORTABLE)["commands"]
        assert "sudo modprobe vhci-hcd" in c["linux"]
        assert "usbip-win2" in c["windows"]


@needs_node
class TestText:
    """UsbipLogic.sinceText, sizeText"""

    def test_since_is_the_time_since_the_epoch_second(self):
        # On since 1000 s before now: "16 min".
        assert logic("usbip", "L.sinceText(1000000 - 1000, 1000000 * 1000)") == "16 min"

    def test_since_zero_is_unknown(self):
        # On since 0 (the board's clock is unset): "unknown".
        assert logic("usbip", "L.sinceText(0, 1000000 * 1000)") == "unknown"

    def test_sizes(self):
        # On 0, 500, 4096, 65536 and 1048576 bytes: whole units without a trailing .0.
        assert logic("usbip", "[0, 500, 4096, 65536, 1048576].map(L.sizeText)") == [
            "0 B",
            "500 B",
            "4 KB",
            "64 KB",
            "1 MB",
        ]


@needs_node
class TestPageShows:
    """UsbipPage.refresh"""

    def test_chip_and_switch_follow_the_status(self):
        # On an attached status: chip "Attached", switch on.
        r = scenario(
            setup()
            + "return [t.el('headChip').textContent, t.el('headChip').hidden(), t.el('toggle').attrs['aria-checked']];"
        )
        assert r == ["Attached", False, "true"]

    def test_off_hides_the_boxes_and_the_switch_is_off(self):
        # On an off status: switch off; device, client, counters and attach boxes are hidden.
        r = scenario(
            setup(OFF)
            + "return [t.el('toggle').attrs['aria-checked'], ['deviceCard','clientCard','statsCard','howCard'].map((i) => t.el(i).hidden())];"
        )
        assert r == ["false", [True, True, True, True]]

    def test_blocked_explains_why_thread_wins(self):
        # On blocked: the note shows the reason; the live boxes hide.
        r = scenario(
            setup(BLOCKED)
            + "return [t.el('blockedNote').hidden(), t.el('blockedText').textContent];"
        )
        assert r[0] is False
        assert "usb-10c4-ea60-abc123 runs otbr" in r[1]

    def test_pending_reboot_is_said(self):
        # On pending_reboot: the reboot note shows; without it, it is hidden.
        r = scenario(
            setup(PENDING_OFF)
            + "const a = t.el('pendingNote').hidden();"
            + "t.status = "
            + json.dumps(ATTACHED)
            + "; await t.advance(6000);"
            + "return [a, t.el('pendingNote').hidden()];"
        )
        assert r == [False, True]

    def test_turned_off_but_still_exported_says_when_it_ends(self):
        # On enabled false while the port is still exported: the switch text says it turns off at the next reboot.
        r = scenario(setup(PENDING_OFF) + "return t.el('switchText').textContent;")
        assert "next reboot" in r
        assert "Radios page" not in r

    def test_security_note_names_the_interface(self):
        # On interface mgmt: "management network"; on single: "Ethernet network".
        r = scenario(
            setup({**ATTACHED, "interface": "mgmt"})
            + "const a = t.el('ifaceName').textContent;"
            + f"t.status = {json.dumps({**ATTACHED, 'interface': 'single'})}; await t.advance(6000);"
            + "return [a, t.el('ifaceName').textContent];"
        )
        assert "management" in r[0]
        assert "Ethernet" in r[1]

    def test_device_card_lists_product_ids_speed_and_bus_id(self):
        # On an exportable CP2102N: product, maker and a key/value list with ids, serial, speed, bus id.
        r = scenario(
            setup(EXPORTABLE)
            + "return [t.el('devName').textContent, t.el('devMaker').textContent, t.el('devKv').allText(),"
            "t.el('devInfo').hidden(), t.el('devEmpty').hidden(), t.el('devWhy').hidden()];"
        )
        assert r[0] == "CP2102N USB to UART Bridge Controller"
        assert r[1] == "Silicon Labs"
        for want in ("10c4:ea60", "abc123", "full", "1-1", "1 interface"):
            assert want in r[2]
        assert r[3:] == [False, True, True]

    def test_hostile_device_strings_stay_text(self):
        # On markup in every string the device or the board sends: each shows as the literal text, no element is made.
        evil = EVIL
        s = {
            **ATTACHED,
            "state": "unsupported",
            "reason": evil,
            "last_error": evil,
            "device": {**CP2102, "product": evil, "manufacturer": evil, "serial": evil},
            "client": {"addr": evil, "since": 0},
        }
        r = scenario(
            setup(s)
            + "t.put = () => ({status: 409, body: {error: 'usb_channel_in_use', detail: "
            + json.dumps(evil)
            + "}});"
            "t.el('toggle').click(); t.el('dOk').click(); await t.advance(100);"
            "const texts = ['devName', 'devMaker', 'devWhy', 'clientAddr', 'err409Text'].map((i) => t.el(i).textContent);"
            "const kv = t.el('devKv').allText();"
            "return [texts, kv, t.doc.markupWrites];"
        )
        assert r[0] == [EVIL] * 5
        assert EVIL in r[1]
        assert r[2] == []

    def test_blocked_reason_stays_text(self):
        # On markup in the blocked reason: the note holds it as text.
        r = scenario(
            setup({**BLOCKED, "reason": EVIL})
            + "return [t.el('blockedText').textContent, t.doc.markupWrites];"
        )
        assert EVIL in r[0]
        assert r[1] == []

    def test_unsupported_device_shows_the_reason(self):
        # On a hub: the reason shows in the device card, and the client box says there is no client.
        r = scenario(
            setup(UNSUPPORTED)
            + "return [t.el('devWhy').hidden(), t.el('devWhy').textContent, t.el('clientEmpty').textContent];"
        )
        assert r[0] is False
        assert "Hubs are not supported" in r[1]
        assert r[2] == "No client."

    def test_no_device_says_nothing_is_plugged_in(self):
        # On no_device: the empty note shows instead of the device.
        r = scenario(
            setup(NO_DEVICE)
            + "return [t.el('devInfo').hidden(), t.el('devEmpty').hidden(), t.el('devEmpty').textContent];"
        )
        assert r[:2] == [True, False]
        assert "Nothing is plugged" in r[2]

    def test_client_card_shows_address_and_since(self):
        # On an attached status: address without the TCP port, and how long (2 h 14 min); Kick shows.
        r = scenario(
            setup()
            + "return [t.el('clientAddr').textContent, t.el('clientSince').textContent, t.el('clientInfo').hidden(), t.el('kickBtn').hidden()];"
        )
        assert r[0] == "192.168.10.20"
        assert r[1] == "2 h 14 min"
        assert r[2:] == [False, False]

    def test_client_since_zero_reads_unknown(self):
        # On a client with since 0: "unknown".
        s = {**ATTACHED, "client": {"addr": "10.0.0.2:4000", "since": 0}}
        r = scenario(setup(s) + "return t.el('clientSince').textContent;")
        assert r == "unknown"

    def test_no_client_hides_kick(self):
        # On exportable: "No computer has attached it yet." and no Kick.
        r = scenario(
            setup(EXPORTABLE)
            + "return [t.el('clientEmpty').hidden(), t.el('clientEmpty').textContent, t.el('kickBtn').hidden(), t.el('clientInfo').hidden()];"
        )
        assert r == [False, "No computer has attached it yet.", True, True]

    def test_counters(self):
        # On the sample counters: sessions, transfers, bytes and buffer peaks in words.
        r = scenario(
            setup()
            + "return ['stSessions','stSessionsSub','stTransfers','stTransfersSub','stFrom','stFromSub','stBuf','stBufSub'].map((i) => t.el(i).textContent);"
        )
        assert r[0] == "3"
        assert "1 refused" in r[1]
        assert r[2].replace(" ", " ") == "11 950"
        assert "2 errors" in r[3] and "48 cancelled" in r[3]
        assert r[4] == "1 MB"
        assert "64 KB" in r[5]
        assert r[6] == "4 KB"
        assert "peak 16 KB of 32 KB" in r[7]

    def test_attach_box_carries_the_address(self):
        # On host board.lan: both command boxes hold the commands with board.lan; Linux shows first.
        r = scenario(
            setup()
            + "return [t.el('cmdLinux').allText(), t.el('cmdWin').allText(), t.el('cmdLinux').hidden(), t.el('cmdWin').hidden()];"
        )
        assert "usbip attach -r board.lan -b 1-1" in r[0]
        assert "usbip attach -r board.lan -b 1-1" in r[1]
        assert r[2:] == [False, True]

    def test_os_tabs_switch_the_commands(self):
        # On clicking Windows: the Windows commands show and the Linux ones hide; back again on Linux.
        r = scenario(
            setup()
            + "t.el('osWin').click(); const a = [t.el('cmdLinux').hidden(), t.el('cmdWin').hidden()];"
            "t.el('osLinux').click(); return [a, [t.el('cmdLinux').hidden(), t.el('cmdWin').hidden()]];"
        )
        assert r == [[True, False], [False, True]]


@needs_node
class TestPageToggle:
    """UsbipPage toggle"""

    def test_asks_before_anything_is_sent(self):
        # On clicking the switch while off: the dialog opens and states the reboot; nothing is sent.
        r = scenario(
            setup(OFF)
            + "t.el('toggle').click(); await t.flush();"
            + f"return [t.el('dlg').open, t.el('dTitle').textContent, t.el('dText').textContent, {puts()}];"
        )
        assert r[0] is True
        assert r[1] == "Turn USB/IP on?"
        assert "reboots" in r[2]
        assert r[3] == []

    def test_cancel_sends_nothing(self):
        # On Cancel: the dialog closes, nothing is sent, the switch stays.
        r = scenario(
            setup(OFF)
            + "t.el('toggle').click(); t.el('dCancel').click(); await t.advance(10);"
            + f"return [t.el('dlg').open, {puts()}, t.el('toggle').attrs['aria-checked']];"
        )
        assert r == [False, [], "false"]

    def test_turning_on_puts_enabled_true_and_waits_for_the_board(self):
        # On confirming: PUT {"enabled": true}; on {"reboot": true} the page says so, waits and reloads once.
        r = scenario(
            setup(OFF) + "t.put = () => ({status: 200, body: {reboot: true}});"
            "t.el('toggle').click(); t.el('dOk').click(); await t.advance(1000);"
            + f"const a = [{puts()}, t.el('toggleMsg').textContent, t.reloads()];"
            "await t.advance(20000); return [a, t.reloads()];"
        )
        assert r[0][0] == [{"enabled": True}]
        assert "rebooting" in r[0][1]
        assert r[0][2] == 0
        assert r[1] == 1

    def test_turning_off_puts_enabled_false(self):
        # On confirming while on: the dialog warns a client loses the device; PUT {"enabled": false}.
        r = scenario(
            setup() + "t.put = () => ({status: 200, body: {reboot: true}});"
            "t.el('toggle').click(); const text = t.el('dText').textContent; const title = t.el('dTitle').textContent;"
            "t.el('dOk').click(); await t.advance(100);"
            + f"return [title, text, {puts()}];"
        )
        assert r[0] == "Turn USB/IP off?"
        assert "loses it" in r[1]
        assert r[2] == [{"enabled": False}]

    def test_no_reboot_needed_just_refreshes(self):
        # On {"reboot": false}: no wait for the board, no reload, the controls work again.
        r = scenario(
            setup(OFF) + "t.put = () => ({status: 200, body: {reboot: false}});"
            "t.el('toggle').click(); t.el('dOk').click(); await t.advance(10000);"
            "return [t.reloads(), t.calls.filter((c) => c.url === '/device/diag').length, t.el('toggle').disabled];"
        )
        assert r == [0, 0, False]

    def test_channel_in_use_shows_the_detail_and_the_radios_link(self):
        # On 409 usb_channel_in_use: the detail shows with a link to the Radios page; no wait for the board.
        detail = "usb-10c4-ea60-abc123 runs otbr: move it or switch it off first"
        r = scenario(
            setup(OFF)
            + f"t.put = () => ({{status: 409, body: {{error: 'usb_channel_in_use', detail: {json.dumps(detail)}}}}});"
            "t.el('toggle').click(); t.el('dOk').click(); await t.advance(10000);"
            "return [t.el('err409').hidden(), t.el('err409Text').textContent, t.el('err409Link').attrs.href,"
            "t.reloads(), t.el('toggle').attrs['aria-checked'], t.el('toggle').disabled];"
        )
        assert r[0] is False
        assert detail in r[1]
        assert r[2] == "/radios.html"
        assert r[3] == 0
        assert r[4] == "false"
        assert r[5] is False

    def test_the_409_goes_away_on_the_next_try(self):
        # On a second confirm that succeeds: the 409 box hides.
        detail = "x runs otbr: move it or switch it off first"
        r = scenario(
            setup(OFF)
            + f"t.put = () => ({{status: 409, body: {{error: 'usb_channel_in_use', detail: {json.dumps(detail)}}}}});"
            "t.el('toggle').click(); t.el('dOk').click(); await t.advance(100);"
            "t.put = () => ({status: 200, body: {reboot: false}});"
            "t.el('toggle').click(); t.el('dOk').click(); await t.advance(100);"
            "return t.el('err409').hidden();"
        )
        assert r is True

    def test_busy_is_shown_as_a_message(self):
        # On 409 busy: the message names the busy board; the 409 box stays hidden.
        r = scenario(
            setup(OFF)
            + "t.put = () => ({status: 409, body: {error: 'busy', detail: 'an update runs'}});"
            "t.el('toggle').click(); t.el('dOk').click(); await t.advance(100);"
            "return [t.el('toggleMsg').textContent, t.el('err409').hidden()];"
        )
        assert "busy" in r[0] and "an update runs" in r[0]
        assert r[1] is True

    def test_bad_value_is_shown_as_a_message(self):
        # On 400 bad_value: the message shows the detail.
        r = scenario(
            setup(OFF)
            + "t.put = () => ({status: 400, body: {error: 'bad_value', detail: 'enabled must be a boolean'}});"
            "t.el('toggle').click(); t.el('dOk').click(); await t.advance(100);"
            "return t.el('toggleMsg').textContent;"
        )
        assert "enabled must be a boolean" in r


@needs_node
class TestPageKick:
    """UsbipPage kick"""

    def test_kick_puts_kick_true_without_asking(self):
        # On Kick: PUT {"kick": true} at once, no confirmation, and the status is read again.
        r = scenario(
            setup()
            + "const n = t.calls.length; t.el('kickBtn').click(); await t.advance(100);"
            + f"return [{puts()}, t.confirms().length, t.el('dlg').open, t.calls.slice(n).filter((c) => c.method === 'GET' && c.url === '/device/usbip').length > 0];"
        )
        assert r == [[{"kick": True}], 0, False, True]

    def test_kick_failure_is_shown(self):
        # On a refused kick: the message shows the board's answer.
        r = scenario(
            setup()
            + "t.put = () => ({status: 409, body: {error: 'busy', detail: 'an update runs'}});"
            "t.el('kickBtn').click(); await t.advance(100); return t.el('toggleMsg').textContent;"
        )
        assert "an update runs" in r


@needs_node
class TestPagePolling:
    """UsbipPage poll"""

    def test_the_page_follows_the_board(self):
        # On a status that changes from exportable to attached: the chip follows without a reload.
        r = scenario(
            setup(EXPORTABLE)
            + "const a = t.el('headChip').textContent;"
            + f"t.status = {json.dumps(ATTACHED)}; await t.advance(6000);"
            "return [a, t.el('headChip').textContent];"
        )
        assert r == ["Ready to attach", "Attached"]

    def test_unreadable_status_is_said(self):
        # On a failing read: the stale note shows; it hides after a good read.
        r = scenario(
            setup()
            + "t.fail = true; await t.advance(6000); const a = t.el('usbipStale').hidden();"
            "t.fail = false; await t.advance(60000); return [a, t.el('usbipStale').hidden()];"
        )
        assert r == [False, True]
