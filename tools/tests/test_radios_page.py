"""Radios page (BDD: RadiosLogic, RadiosPage) and the Thread hide rule. Runs static/radios.js under node."""

import json

import pytest
from device_js import FRONTEND, logic, needs_node, page, scenario

C6 = {
    "id": "c6",
    "kind": "c6",
    "label": "ESP32-C6 (on board)",
    "mode": "otbr",
    "stored_mode": "otbr",
    "state": "running",
    "error": None,
    "line": {"baud": 460800, "flow_control": False},
    "usb": None,
    "caps": {"firmware_update": True, "hw_reset": True, "flow_control": False},
    "rcp_proxy": {
        "direction": "listen",
        "adapter": "tcp",
        "port": 6638,
        "host": "",
        "framing": "hdlc",
        "state": None,
        "peer": None,
    },
    "firmware": {
        "type": "spinel",
        "version": "1.4.0",
        "chip": "esp32c6",
        "baud": None,
        "source": "passive",
        "at": 1762430000,
    },
    "counters": {
        "frames_in": 10,
        "frames_out": 9,
        "bytes_in": 1000,
        "bytes_out": 900,
        "drops": 1,
        "resets": 2,
        "rx_errors": 3,
    },
}
ZERO = dict.fromkeys(C6["counters"], 0)
OLLY = {
    "id": "usb-10c4-ea60-abc123",
    "kind": "usb",
    "label": "OllyZig CC2652P",
    "mode": "rcp-proxy",
    "stored_mode": "rcp-proxy",
    "state": "running",
    "error": None,
    "line": {"baud": 115200, "flow_control": True},
    "usb": {"vid": "10c4", "pid": "ea60", "serial": "abc123", "driver": "cp210x"},
    "caps": {"firmware_update": False, "hw_reset": False, "flow_control": True},
    "rcp_proxy": {
        "direction": "connect",
        "adapter": "udp",
        "port": 9999,
        "host": "host.local",
        "framing": "raw",
        "state": "connected",
        "peer": "192.168.1.20:9999",
    },
    "firmware": {
        "type": "spinel",
        "version": "1.4.0-Koenkk",
        "chip": None,
        "baud": 115200,
        "source": "probe",
        "at": None,
    },
    "counters": ZERO,
}
SLZB = {
    "id": "usb-10c4-ea60-a4c13861",
    "kind": "usb",
    "label": "SLZB-06",
    "mode": "unavailable",
    "stored_mode": "rcp-proxy",
    "state": "idle",
    "error": None,
    "line": {"baud": 115200, "flow_control": True},
    "usb": {"vid": "10c4", "pid": "ea60", "serial": "a4c13861", "driver": None},
    "caps": {"firmware_update": False, "hw_reset": False, "flow_control": True},
    "rcp_proxy": {
        "direction": "listen",
        "adapter": "tcp",
        "port": 6638,
        "host": "",
        "framing": "hdlc",
        "state": None,
        "peer": None,
    },
    "firmware": {
        "type": "znp",
        "version": "20240710",
        "chip": "CC2652P",
        "baud": 115200,
        "source": "probe",
        "at": 1762000000,
    },
    "counters": ZERO,
}
EZSP = {
    "id": "usb-10c4-ea60-0001",
    "kind": "usb",
    "label": "Sonoff ZBDongle-E",
    "mode": "off",
    "stored_mode": "otbr",
    "state": "idle",
    "error": "stored mode otbr needs spinel; detected ezsp",
    "line": {"baud": 115200, "flow_control": True},
    "usb": {"vid": "10c4", "pid": "ea60", "serial": "0001", "driver": "cp210x"},
    "caps": {"firmware_update": False, "hw_reset": False, "flow_control": True},
    "rcp_proxy": {
        "direction": "listen",
        "adapter": "tcp",
        "port": 6639,
        "host": "",
        "framing": "hdlc",
        "state": None,
        "peer": None,
    },
    "firmware": {
        "type": "ezsp",
        "version": "7.4.4",
        "chip": None,
        "baud": None,
        "source": "probe",
        "at": 1762430000,
    },
    "counters": ZERO,
}
SNAP = {"channels": [C6, OLLY, SLZB, EZSP]}
SNAP_ZIG = {**SNAP, "available_modes": ["off", "otbr", "rcp-proxy", "ziggurat"]}
SNAP_NOZIG = {**SNAP, "available_modes": ["off", "otbr", "rcp-proxy"]}
RUNNING = {"mode": "BR_RUNNING", "otbr_channel": "c6", "fault_reason": None}
NO_RADIO = {"mode": "NO_RADIO", "otbr_channel": None, "fault_reason": None}

# t.snap, t.status: the bodies GET answers; t.put(url, body, method) -> {status, body} answers a PUT or DELETE.
ROUTES = """(u, o) => {
  if (o.method === 'PUT' || o.method === 'DELETE')
    return t.put ? t.put(u, o.body ? JSON.parse(o.body) : null, o.method) : {status: 200, body: {}};
  if (u === '/device/channels') return t.snapGone ? {status: 503, body: {}} : {status: 200, body: t.snap};
  if (u === '/device/status') return {status: 200, body: Object.assign(H.fixture(H.STATUS), t.status || {})};
  if (u === '/device/network') return {status: 200, body: H.NETWORK};
  return {status: 200, body: {}};
}"""


def setup(snap=SNAP, status=RUNNING):
    return (
        page("radios", ROUTES)
        + f"t.snap = {json.dumps(snap)}; t.status = {json.dumps(status)};"
        + "t.page.init(); await t.advance(10);"
    )


def rows_js():
    return "t.el('rows').children"


def open_row(i):
    return f"{rows_js()}[{i}].click(); await t.flush();"


def card(mode):
    return f"t.el('track').children.find((c) => c.attrs['data-m'] === '{mode}')"


def logic_of(expression):
    return logic("radios", expression)


@needs_node
class TestModeAvailable:
    """RadiosLogic.modeAvailable"""

    def test_listed_mode_is_available(self):
        # On a snapshot whose available_modes holds ziggurat: true.
        assert logic_of(f"L.modeAvailable('ziggurat', {json.dumps(SNAP_ZIG)})") is True

    def test_unlisted_mode_is_not_available(self):
        # On a snapshot whose available_modes lacks ziggurat: false; the listed ones stay available.
        assert logic_of(
            f"['ziggurat', 'otbr'].map((m) => L.modeAvailable(m, {json.dumps(SNAP_NOZIG)}))"
        ) == [False, True]

    def test_without_the_list_three_modes_are_available(self):
        # On a snapshot without available_modes: off, otbr and rcp-proxy true, ziggurat false.
        assert logic_of(
            f"['off', 'otbr', 'rcp-proxy', 'ziggurat'].map((m) => L.modeAvailable(m, {json.dumps(SNAP)}))"
        ) == [True, True, True, False]


@needs_node
class TestTable:
    """radios.js table"""

    def test_one_row_per_channel(self):
        # On the snapshot: four rows, in the order of the list.
        r = scenario(
            setup()
            + f"return {rows_js()}.map((tr) => tr.allText().includes('SLZB-06') ? 'slzb' : tr.allText().slice(0, 8));"
        )
        assert len(r) == 4
        assert r[2] == "slzb"

    def test_unplugged_rows_are_greyed(self):
        # On the unplugged SLZB-06: its row carries "gone"; the plugged ones do not.
        r = scenario(
            setup() + f"return {rows_js()}.map((tr) => tr.classList.contains('gone'));"
        )
        assert r == [False, False, True, False]

    def test_mode_icon_and_name(self):
        # On the C6 in otbr and the OllyZig in rcp-proxy: the mode's icon and name.
        r = scenario(
            setup()
            + f"return {rows_js()}.slice(0, 2).map((tr) => [tr.findAll((e) => e.tag === 'use').map((u) => u.attrs.href), tr.allText()]);"
        )
        assert r[0][0][0] == "#i-otbr"
        assert "Thread border router" in r[0][1]
        assert r[1][0][0] == "#i-proxy"
        assert "RCP proxy" in r[1][1]

    def test_stored_mode_that_does_not_fit_reads_off_stored(self):
        # On the EZSP stick stored otbr but detected ezsp: "Off, stored: Thread border router" and the reason.
        r = scenario(
            setup()
            + f"return [{rows_js()}[3].allText(), {rows_js()}[3].findAll((e) => e.tag === 'use')[0].attrs.href];"
        )
        assert "Off, stored: Thread border router" in r[0]
        assert "detected ezsp" in r[0]
        assert r[1] == "#i-off"

    def test_unplugged_row_says_the_mode_applies_when_back(self):
        # On the unplugged SLZB-06 stored rcp-proxy: "RCP proxy (when back)", the gone icon, state Unplugged.
        r = scenario(
            setup()
            + f"return [{rows_js()}[2].allText(), {rows_js()}[2].findAll((e) => e.tag === 'use')[0].attrs.href];"
        )
        assert "RCP proxy (when back)" in r[0]
        assert "Unplugged" in r[0]
        assert r[1] == "#i-gone"

    def test_firmware_cell_shows_type_version_and_chip(self):
        # On the C6: spinel, 1.4.0 and esp32c6.
        r = scenario(setup() + f"return {rows_js()}[0].allText();")
        assert "spinel" in r and "1.4.0" in r and "esp32c6" in r

    def test_empty_chip_and_missing_firmware_read_as_a_dash(self):
        # On a null chip and on a channel without a firmware record: "—".
        bare = {**OLLY, "firmware": None}
        r = scenario(
            setup({"channels": [OLLY, bare]})
            + f"return [{rows_js()}[0].allText(), {rows_js()}[1].allText()];"
        )
        assert "—" in r[0]
        assert "—" in r[1]

    def test_empty_version_reads_as_a_dash(self):
        # On a firmware record with a null version: "—" after the type.
        ch = {**OLLY, "firmware": {**OLLY["firmware"], "version": None}}
        r = scenario(setup({"channels": [ch]}) + f"return {rows_js()}[0].allText();")
        assert "spinel —" in r

    def test_state_chips(self):
        # On running, unplugged and a faulted channel: Running, Unplugged, Fault with its reason.
        bad = {
            **EZSP,
            "id": "u2",
            "mode": "otbr",
            "state": "fault",
            "error": "drain_timeout",
        }
        r = scenario(
            setup({"channels": [C6, SLZB, bad]})
            + f"return {rows_js()}.map((tr) => tr.allText());"
        )
        assert "Running" in r[0]
        assert "Unplugged" in r[1]
        assert "Fault" in r[2] and "drain_timeout" in r[2]

    def test_activity_names_the_peer_or_the_frames(self):
        # On the connected OllyZig: the peer; on the C6: the frames in and the drops.
        r = scenario(
            setup() + f"return [{rows_js()}[0].allText(), {rows_js()}[1].allText()];"
        )
        assert "10 frames in" in r[0] and "1 drops" in r[0]
        assert "192.168.1.20:9999" in r[1]

    def test_device_text_is_rendered_as_text(self):
        # On a label with markup: it lands in textContent unchanged.
        ch = {**C6, "label": "<img src=x onerror=alert(1)>"}
        r = scenario(setup({"channels": [ch]}) + f"return {rows_js()}[0].allText();")
        assert "<img src=x onerror=alert(1)>" in r

    def test_head_names_the_radio_thread_runs_on(self):
        # On otbr_channel c6: the chip reads "Thread runs on: ESP32-C6 (on board)".
        r = scenario(setup() + "return t.el('radios-chip').textContent;")
        assert r == "Thread runs on: ESP32-C6 (on board)"

    def test_head_says_when_no_radio_runs_thread(self):
        # On otbr_channel null: the chip reads "Thread runs on: no radio".
        r = scenario(setup(SNAP, NO_RADIO) + "return t.el('radios-chip').textContent;")
        assert r == "Thread runs on: no radio"

    def test_unanswered_list_shows_the_stale_line(self):
        # On a failing GET: the stale line says so and the rows stay as they were.
        r = scenario(
            setup()
            + "t.snapGone = true; await t.advance(6000); return [t.el('radios-stale').hidden(), t.el('radios-stale').textContent, "
            + rows_js()
            + ".length];"
        )
        assert r[0] is False
        assert r[1]
        assert r[2] == 4


@needs_node
class TestDialog:
    """radios.js dialog"""

    def test_row_click_opens_the_dialog_for_that_channel(self):
        # On a click on the OllyZig row: the dialog opens with its label and id.
        r = scenario(
            setup()
            + open_row(1)
            + "return [t.el('dlg').open, t.el('mTitle').textContent, t.el('mSub').textContent];"
        )
        assert r[0] is True
        assert r[1] == "OllyZig CC2652P"
        assert "usb-10c4-ea60-abc123" in r[2]

    def test_configure_button_opens_it_too(self):
        # On the Configure button of a row: the dialog opens.
        r = scenario(
            setup()
            + "const b = t.el('rows').children[0].findAll((e) => e.tag === 'button')[0]; b.click(); await t.flush(); return t.el('dlg').open;"
        )
        assert r is True

    def test_four_mode_cards_with_the_stored_mode_pressed(self):
        # On the OllyZig: cards off, otbr, rcp-proxy, ziggurat; rcp-proxy is pressed.
        r = scenario(
            setup()
            + open_row(1)
            + "return t.el('track').children.map((c) => [c.attrs['data-m'], c.attrs['aria-pressed']]);"
        )
        assert r == [
            ["off", "false"],
            ["otbr", "false"],
            ["rcp-proxy", "true"],
            ["ziggurat", "false"],
        ]

    def test_ziggurat_card_is_disabled_without_the_list(self):
        # On a snapshot without available_modes: the ziggurat card is disabled with "Not in this build".
        r = scenario(
            setup()
            + open_row(1)
            + f"const c = {card('ziggurat')}; return [c.disabled, c.allText()];"
        )
        assert r[0] is True
        assert "Not in this build" in r[1]

    def test_ziggurat_card_is_enabled_when_listed(self):
        # On a snapshot listing ziggurat: the card can be picked.
        r = scenario(
            setup(SNAP_ZIG) + open_row(1) + f"return {card('ziggurat')}.disabled;"
        )
        assert r is False

    def test_mode_missing_from_the_list_is_disabled(self):
        # On a list without rcp-proxy: that card is disabled with the reason.
        snap = {**SNAP, "available_modes": ["off", "otbr"]}
        r = scenario(
            setup(snap)
            + open_row(0)
            + f"const c = {card('rcp-proxy')}; return [c.disabled, c.allText()];"
        )
        assert r[0] is True
        assert "Not in this build" in r[1]

    def test_card_the_firmware_cannot_take_is_disabled_with_the_reason(self):
        # On the EZSP stick: the otbr card is disabled, naming what it needs and what the stick runs; off is pressed.
        r = scenario(
            setup()
            + open_row(3)
            + f"const c = {card('otbr')}; return [c.disabled, c.allText(), {card('off')}.attrs['aria-pressed']];"
        )
        assert r[0] is True
        assert "OpenThread RCP" in r[1] and "EZSP" in r[1]
        assert r[2] == "true"

    def test_unplugged_stick_takes_no_other_mode(self):
        # On the unplugged SLZB-06: the other cards say "Plug it in to change the mode"; its own stays enabled.
        r = scenario(
            setup()
            + open_row(2)
            + "return t.el('track').children.map((c) => [c.attrs['data-m'], c.disabled, c.allText().includes('Plug it in')]);"
        )
        assert r[0] == ["off", True, True]
        assert r[2] == ["rcp-proxy", False, False]

    def test_unplugged_stick_explains_and_offers_forget(self):
        # On the unplugged SLZB-06: the unavailable note and the Forget button show; on the C6 they do not.
        r = scenario(
            setup()
            + open_row(2)
            + "const a = [t.el('mUnavailable').hidden(), t.el('mForget').hidden()]; t.el('mCancel').click();"
            + open_row(0)
            + "return [a, [t.el('mUnavailable').hidden(), t.el('mForget').hidden()]];"
        )
        assert r == [[False, False], [True, True]]

    def test_choosing_otbr_on_another_channel_turns_save_into_move_thread_here(self):
        # On picking otbr on the OllyZig: the button reads "Move Thread here" and the move text shows.
        r = scenario(
            setup()
            + open_row(1)
            + f"const before = t.el('mSave').textContent; {card('otbr')}.click(); await t.flush();"
            + "return [before, t.el('mSave').textContent, t.el('panelOtbr').hidden(), t.el('otbrNote').textContent];"
        )
        assert r[0] == "Save"
        assert r[1] == "Move Thread here"
        assert r[2] is False
        assert "Thread devices lose the border router" in r[3]
        assert "ESP32-C6 (on board)" in r[3]

    def test_the_radio_that_runs_thread_keeps_save(self):
        # On the C6 (already otbr): Save stays Save and no move text shows.
        r = scenario(
            setup()
            + open_row(0)
            + "return [t.el('mSave').textContent, t.el('panelOtbr').hidden()];"
        )
        assert r == ["Save", True]

    def test_connect_shows_the_host_field(self):
        # On the OllyZig (connect): the host field shows; on switching to listen it hides.
        r = scenario(
            setup()
            + open_row(1)
            + "const a = t.el('hostField').hidden(); t.el('dir-listen').click(); const b = t.el('hostField').hidden();"
            + "t.el('dir-connect').click(); return [a, b, t.el('hostField').hidden(), t.el('hostIn').value];"
        )
        assert r == [False, True, False, "host.local"]

    def test_udp_shows_the_framing_choice(self):
        # On udp: the framing choice shows; on tcp it hides.
        r = scenario(
            setup()
            + open_row(1)
            + "const a = t.el('framingField').hidden(); t.el('ad-tcp').click(); return [a, t.el('framingField').hidden()];"
        )
        assert r == [False, True]

    def test_proxy_panel_follows_the_picked_mode(self):
        # On the OllyZig: the proxy panel shows for rcp-proxy and hides when off is picked.
        r = scenario(
            setup()
            + open_row(1)
            + f"const a = t.el('panelProxy').hidden(); {card('off')}.click(); await t.flush(); return [a, t.el('panelProxy').hidden()];"
        )
        assert r == [False, True]

    def test_proxy_fields_are_prefilled_from_the_stored_settings(self):
        # On the OllyZig: the port, host and pressed buttons carry the stored settings.
        r = scenario(
            setup()
            + open_row(1)
            + "return [t.el('portIn').value, t.el('hostIn').value, t.el('dir-connect').attrs['aria-pressed'], t.el('ad-udp').attrs['aria-pressed'], t.el('fr-raw').attrs['aria-pressed']];"
        )
        assert r == ["9999", "host.local", "true", "true", "true"]

    def test_serial_line_is_for_usb_sticks_only(self):
        # On the C6: no serial line panel; on the OllyZig: baud 115200 and flow control ticked.
        r = scenario(
            setup()
            + open_row(0)
            + "const c6 = t.el('panelLine').hidden(); t.el('mCancel').click();"
            + open_row(1)
            + "return [c6, t.el('panelLine').hidden(), t.el('baudSel').value, t.el('flowIn').checked];"
        )
        assert r == [True, False, "115200", True]

    def test_flow_control_is_greyed_where_the_bridge_has_none(self):
        # On a stick whose caps lack flow control: the box is disabled and unticked.
        ch = {**OLLY, "caps": {**OLLY["caps"], "flow_control": False}}
        r = scenario(
            setup({"channels": [ch]})
            + open_row(0)
            + "return [t.el('flowIn').disabled, t.el('flowIn').checked];"
        )
        assert r == [True, False]

    def test_cancel_closes_without_a_request(self):
        # On Cancel: the dialog closes and nothing is sent.
        r = scenario(
            setup()
            + open_row(1)
            + "t.el('mCancel').click(); return [t.el('dlg').open, t.calls.filter((c) => c.method === 'PUT').length];"
        )
        assert r == [False, 0]


@needs_node
class TestSave:
    """radios.js save, move and forget"""

    def puts(self):
        return "t.calls.filter((c) => c.method === 'PUT').map((c) => [c.url, JSON.parse(c.body)])"

    def test_settings_alone_send_only_what_changed(self):
        # On a new port for the OllyZig: one PUT with only the proxy settings, no mode.
        r = scenario(
            setup()
            + open_row(1)
            + "t.el('portIn').type('7000'); t.el('mSave').click(); await t.advance(10); return "
            + self.puts()
            + ";"
        )
        assert r == [
            [
                "/device/channels/usb-10c4-ea60-abc123",
                {"rcp_proxy": {"port": 7000}},
            ]
        ]

    def test_unchanged_dialog_sends_nothing(self):
        # On Save without a change: the dialog closes and nothing is sent.
        r = scenario(
            setup()
            + open_row(1)
            + "t.el('mSave').click(); await t.advance(10); return [t.el('dlg').open, "
            + self.puts()
            + "];"
        )
        assert r == [False, []]

    def test_baud_and_flow_are_sent_with_the_line_settings(self):
        # On a new baud and flow control off: one PUT with baud and flow_control.
        r = scenario(
            setup()
            + open_row(1)
            + "t.el('baudSel').value = '230400'; t.el('flowIn').tick(false); t.el('mSave').click(); await t.advance(10); return "
            + self.puts()
            + ";"
        )
        assert r[0][1] == {"baud": 230400, "flow_control": False}

    def test_mode_change_sends_the_mode(self):
        # On picking off for the OllyZig: one PUT {mode: off}; the dialog closes and the list is read again.
        r = scenario(
            setup()
            + open_row(1)
            + f"{card('off')}.click(); await t.flush(); const n = t.calls.length; t.el('mSave').click(); await t.advance(10);"
            + "return [t.el('dlg').open, "
            + self.puts()
            + ", t.calls.slice(n).filter((c) => c.url === '/device/channels' && c.method === 'GET').length > 0];"
        )
        assert r[0] is False
        assert r[1] == [["/device/channels/usb-10c4-ea60-abc123", {"mode": "off"}]]
        assert r[2] is True

    def test_proxy_settings_of_a_hidden_panel_are_not_sent(self):
        # On a new port and Thread moved here: the proxy panel is hidden, so only the mode goes.
        r = scenario(
            setup()
            + open_row(1)
            + f"t.el('portIn').type('7000'); {card('otbr')}.click(); await t.flush(); t.el('mSave').click(); await t.advance(10); return "
            + self.puts()
            + ";"
        )
        assert r == [["/device/channels/usb-10c4-ea60-abc123", {"mode": "otbr"}]]

    def test_line_settings_then_move(self):
        # On a new baud and Thread moved here: baud PUT first, then the mode PUT.
        r = scenario(
            setup()
            + open_row(1)
            + f"t.el('baudSel').value = '230400'; {card('otbr')}.click(); await t.flush(); t.el('mSave').click(); await t.advance(10); return "
            + self.puts()
            + ";"
        )
        assert r == [
            ["/device/channels/usb-10c4-ea60-abc123", {"baud": 230400}],
            ["/device/channels/usb-10c4-ea60-abc123", {"mode": "otbr"}],
        ]

    def test_failed_settings_put_stops_before_the_move(self):
        # On a refused settings PUT: the mode PUT is not sent and the reason shows in the dialog.
        r = scenario(
            setup()
            + "t.put = () => ({status: 400, body: {error: 'bad_value', detail: 'baud'}});"
            + open_row(1)
            + f"t.el('baudSel').value = '230400'; {card('otbr')}.click(); await t.flush(); t.el('mSave').click(); await t.advance(10); return [{self.puts()}.length, t.el('dlg').open, t.el('mMsg').textContent];"
        )
        assert r[0] == 1
        assert r[1] is True
        assert "baud" in r[2]

    def test_accepted_move_shows_the_move_text_and_polls_every_two_seconds(self):
        # On 202: the dialog closes, the page says Thread is moving, and while a channel is starting the list is read every 2 s.
        starting = {**OLLY, "mode": "otbr", "stored_mode": "otbr", "state": "starting"}
        r = scenario(
            setup()
            + "t.put = () => { t.snap = {channels: [Object.assign({}, t.snap.channels[0], {mode: 'off', stored_mode: 'off'}),"
            + json.dumps(starting)
            + "]}; return {status: 202, body: {moving: true, moved_from: 'c6'}}; };"
            + open_row(1)
            + f"{card('otbr')}.click(); await t.flush(); t.el('mSave').click(); await t.advance(10);"
            + "const text = t.el('rMsg').textContent; const open = t.el('dlg').open;"
            + "const n0 = t.calls.filter((c) => c.url === '/device/channels' && c.method === 'GET').length;"
            + "await t.advance(4100); const n1 = t.calls.filter((c) => c.url === '/device/channels' && c.method === 'GET').length;"
            + "return [open, text, n1 - n0];"
        )
        assert r[0] is False
        assert "Moving Thread" in r[1] and "ESP32-C6 (on board)" in r[1]
        assert r[2] >= 2

    def test_polling_returns_to_normal_once_nothing_starts(self):
        # On a settled list: the next read comes after the normal period, not after 2 s.
        r = scenario(
            setup()
            + "const n0 = t.calls.filter((c) => c.url === '/device/channels').length; await t.advance(3000);"
            + "return t.calls.filter((c) => c.url === '/device/channels').length - n0;"
        )
        assert r == 0

    @pytest.mark.parametrize(
        ("status", "code", "words"),
        [
            (409, "mode_taken", "another radio"),
            (409, "incompatible", "firmware"),
            (409, "unavailable", "not plugged in"),
            (409, "not_available", "not in this build"),
            (409, "hybrid_not_supported", "Thread and Zigbee"),
            (409, "probe_failed", "the test run failed"),
            (409, "c6_busy", "busy"),
            (409, "busy", "busy"),
            (409, "drain_timeout", "did not stop in time"),
            (404, "unknown_channel", "no longer"),
            (500, "failed", "failed"),
        ],
    )
    def test_conflict_codes_get_a_human_text(self, status, code, words):
        # On each 409 code (and the others): the dialog stays open with a text that names the cause; probe_failed adds the reason.
        detail = "no spinel answer"
        r = scenario(
            setup()
            + f"t.put = () => ({{status: {status}, body: {{error: {json.dumps(code)}, detail: {json.dumps(detail)}}}}});"
            + open_row(1)
            + f"{card('off')}.click(); await t.flush(); t.el('mSave').click(); await t.advance(10); return [t.el('dlg').open, t.el('mMsg').textContent];"
        )
        assert r[0] is True
        assert words.lower() in r[1].lower()
        if code == "probe_failed":
            assert detail in r[1]

    def test_bad_value_names_the_field(self):
        # On 400 bad_value with detail rcp_proxy.port: the text names the port.
        r = scenario(
            setup()
            + "t.put = () => ({status: 400, body: {error: 'bad_value', detail: 'rcp_proxy.port'}});"
            + open_row(1)
            + "t.el('portIn').type('7000'); t.el('mSave').click(); await t.advance(10); return t.el('mMsg').textContent;"
        )
        assert "rcp_proxy.port" in r

    def test_connect_without_host_is_refused_before_sending(self):
        # On connect with an empty host: nothing is sent; the dialog says the host is needed.
        r = scenario(
            setup()
            + open_row(1)
            + "t.el('hostIn').type(''); t.el('mSave').click(); await t.advance(10); return [t.calls.filter((c) => c.method === 'PUT').length, t.el('mMsg').textContent];"
        )
        assert r[0] == 0
        assert "host" in r[1].lower()

    def test_port_outside_range_is_refused_before_sending(self):
        # On port 0 and on text: nothing is sent.
        r = scenario(
            setup()
            + open_row(1)
            + "t.el('portIn').type('0'); t.el('mSave').click(); await t.advance(10); const a = t.el('mMsg').textContent;"
            + "t.el('portIn').type('x'); t.el('mSave').click(); await t.advance(10);"
            + "return [t.calls.filter((c) => c.method === 'PUT').length, a];"
        )
        assert r[0] == 0
        assert "port" in r[1].lower()

    def test_reboot_reply_says_the_board_restarts(self):
        # On a 200 with reboot true: the page says the board is rebooting.
        r = scenario(
            setup()
            + "t.put = () => ({status: 200, body: Object.assign({}, t.snap.channels[1], {reboot: true})});"
            + open_row(1)
            + f"{card('off')}.click(); await t.flush(); t.el('mSave').click(); await t.advance(10); return t.el('rMsg').textContent;"
        )
        assert "reboot" in r.lower()

    def test_forget_deletes_after_confirmation(self):
        # On Forget for the unplugged SLZB-06: a confirm, then DELETE; 204 closes the dialog and reads the list again.
        r = scenario(
            setup()
            + "t.put = () => ({status: 204, body: ''});"
            + open_row(2)
            + "t.el('mForget').click(); await t.advance(10);"
            + "return [t.confirms().length, t.calls.filter((c) => c.method === 'DELETE').map((c) => c.url), t.el('dlg').open];"
        )
        assert r == [1, ["/device/channels/usb-10c4-ea60-a4c13861"], False]

    def test_forget_refused_shows_the_reason(self):
        # On a 409 in_use: the dialog stays open and says the radio is in use.
        r = scenario(
            setup()
            + "t.put = () => ({status: 409, body: {error: 'in_use', detail: 'x'}});"
            + open_row(2)
            + "t.el('mForget').click(); await t.advance(10); return [t.el('dlg').open, t.el('mMsg').textContent];"
        )
        assert r[0] is True
        assert "in use" in r[1].lower()


@needs_node
class TestHideRule:
    """DeviceCommon.threadState / threadNotice, the pages' notice, api.js"""

    def test_state_by_status(self):
        # On no answer, otbr_channel null, NO_RADIO with a channel, RADIO_FAULT, running, and an older board: the state.
        states = [
            "null",
            "{mode: 'NO_RADIO', otbr_channel: null}",
            "{mode: 'NO_RADIO', otbr_channel: 'u1'}",
            "{mode: 'RADIO_FAULT', otbr_channel: 'c6'}",
            "{mode: 'BR_RUNNING', otbr_channel: 'c6'}",
            "{mode: 'BR_RUNNING'}",
        ]
        r = scenario(
            "return [" + ", ".join(f"H.Common.threadState({s})" for s in states) + "];"
        )
        assert r == ["unknown", "none", "down", "down", "ok", "ok"]

    def test_notice_texts_and_link(self):
        # On none and down: the notice links the Radios page; the capture page asks for a Thread radio.
        r = scenario(
            """const C = H.Common;
            return [C.threadNotice({mode: 'NO_RADIO', otbr_channel: null}), C.threadNotice({mode: 'NO_RADIO', otbr_channel: null}, 'capture'),
                    C.threadNotice({mode: 'NO_RADIO', otbr_channel: 'u1'}), C.threadNotice({mode: 'BR_RUNNING', otbr_channel: 'c6'}),
                    C.threadNotice({mode: 'RADIO_FAULT', otbr_channel: 'c6', fault_reason: 'x'})];"""
        )
        assert r[0]["href"] == "/radios.html"
        assert (
            r[1]["text"] == "Packet capture needs a radio in Thread border router mode."
        )
        assert "No radio in OTBR mode" in r[2]["text"] and r[2]["fault"] is False
        assert r[3] is None
        assert r[4]["fault"] is True

    def test_dashboard_shows_the_notice_and_the_radio_chip(self):
        # On the Dashboard with no OTBR channel: the notice with its link shows and the chip says no radio; with c6: the chip names it.
        r = scenario(
            page("index", ROUTES)
            + f"t.snap = {json.dumps(SNAP)}; t.status = {json.dumps(NO_RADIO)}; t.page.init(); await t.advance(100);"
            + "const a = [t.el('threadNotice').hidden(), t.el('threadNotice').allText(), t.el('h-thread-on').hidden()];"
            + f"t.status = {json.dumps(RUNNING)}; await t.advance(4000);"
            + "return [a, t.el('threadNotice').hidden(), t.el('h-thread-on').hidden(), t.el('h-thread-on').textContent];"
        )
        assert r[0][0] is False
        assert "Open Radios" in r[0][1]
        assert r[0][2] is False
        assert r[1] is True
        assert r[2] is False
        assert r[3] == "Thread runs on: ESP32-C6 (on board)"

    def test_dashboard_leaves_a_radio_fault_to_its_hero(self):
        # On RADIO_FAULT: the hero explains it and the notice stays hidden.
        r = scenario(
            page("index", ROUTES)
            + "t.snap = {channels: []}; t.status = {mode: 'RADIO_FAULT', otbr_channel: 'c6', fault_reason: 'no rcp firmware'};"
            + "t.page.init(); await t.advance(100); return t.el('threadNotice').hidden();"
        )
        assert r is True

    def test_management_banner_for_no_radio(self):
        # On NO_RADIO and on otbr_channel null: modeBanner links the Radios page.
        r = logic(
            "mgmt",
            "[L.modeBanner({mode: 'NO_RADIO', otbr_channel: null}), L.modeBanner({mode: 'NO_RADIO', otbr_channel: 'u1'})]",
        )
        assert r[0]["href"] == "/radios.html"
        assert r[1]["href"] == "/radios.html"
        assert "no radio" in r[1]["text"].lower()

    @pytest.mark.parametrize("name", ["routing", "topology"])
    def test_routing_and_topology_show_the_notice(self, name):
        # On a Thread page while no radio runs Thread: the notice shows after the first status read.
        r = scenario(
            page(name, ROUTES)
            + f"t.status = {json.dumps(NO_RADIO)}; t.snap = {json.dumps(SNAP)}; t.page.init(); await t.advance(100);"
            + "return [t.el('threadNotice').hidden(), t.el('threadNotice').allText()];"
        )
        assert r[0] is False
        assert "Open Radios" in r[1]

    def test_capture_asks_for_a_thread_radio(self):
        # On the Packet capture page without an OTBR channel: the notice asks for a radio in Thread border router mode.
        r = scenario(
            page("capture", ROUTES)
            + f"t.status = {json.dumps(NO_RADIO)}; t.page.init(); await t.advance(100);"
            + "return [t.el('threadNotice').hidden(), t.el('threadNotice').allText()];"
        )
        assert r[0] is False
        assert "Packet capture needs a radio in Thread border router mode" in r[1]
        assert "Open Radios" in r[1]

    def test_every_thread_page_has_the_notice_slot(self):
        # On the Thread pages and Packet capture: a hidden #threadNotice live region.
        for name in ["index", "routing", "topology", "capture"]:
            html = (FRONTEND / f"{name}.html").read_text()
            assert 'id="threadNotice"' in html, name
            assert (
                'role="status"' in html.split('id="threadNotice"')[1].split(">")[0]
            ), name
