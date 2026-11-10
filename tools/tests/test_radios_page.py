"""Radios page (BDD: RadiosLogic, RadiosPage) and the Thread hide rule. Runs static/radios.js under node."""

import json

import pytest
from device_js import FRONTEND, logic, needs_node, page, scenario

C6 = {
    "id": "builtin-c6",
    "kind": "builtin",
    "label": "ESP32-C6 (built-in)",
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
        "last_error": None,
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
        "uart_errors": 0,
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
        "last_error": None,
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
        "last_error": None,
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
        "last_error": None,
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
RUNNING = {"mode": "BR_RUNNING", "otbr_channel": "builtin-c6", "fault_reason": None}
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

    def test_backoff_is_a_warning_with_the_last_error(self):
        # On a proxy in backoff whose listener failed: the chip warns (not ok) and says why.
        stuck = {
            **OLLY,
            "error": "tcp port 6638: Address already in use",
            "rcp_proxy": {
                **OLLY["rcp_proxy"],
                "state": "backoff",
                "peer": None,
                "last_error": "tcp port 6638: Address already in use",
            },
        }
        r = scenario(
            setup({"channels": [stuck]})
            + f"const chip = {rows_js()}[0].children[3].children[0]; return [chip.textContent, chip.className];"
        )
        assert r[0] == "Backoff: tcp port 6638: Address already in use"
        assert "chip-warn" in r[1] and "chip-ok" not in r[1]

    def test_backoff_without_a_reason_still_warns(self):
        # On backoff with last_error null: "Backoff", warning tone.
        stuck = {
            **OLLY,
            "rcp_proxy": {
                **OLLY["rcp_proxy"],
                "state": "backoff",
                "peer": None,
                "last_error": None,
            },
        }
        r = scenario(
            setup({"channels": [stuck]})
            + f"const chip = {rows_js()}[0].children[3].children[0]; return [chip.textContent, chip.className];"
        )
        assert r == ["Backoff", "chip chip-warn"]

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
        # On otbr_channel c6: the chip reads "Thread runs on: ESP32-C6 (built-in)".
        r = scenario(setup() + "return t.el('radios-chip').textContent;")
        assert r == "Thread runs on: ESP32-C6 (built-in)"

    def test_head_says_when_no_radio_runs_thread(self):
        # On otbr_channel null: the chip reads "No radio in Thread mode".
        r = scenario(setup(SNAP, NO_RADIO) + "return t.el('radios-chip').textContent;")
        assert r == "No radio in Thread mode"

    def test_head_warns_when_the_otbr_radio_is_unplugged(self):
        # On NO_RADIO with Thread stored on the unplugged OllyZig: a warning "Thread on <label>: <reason>", never
        # "Thread runs on".
        status = {
            "mode": "NO_RADIO",
            "otbr_channel": OLLY["id"],
            "fault_reason": "radio unplugged",
        }
        r = scenario(
            setup(SNAP, status)
            + "return [t.el('radios-chip').textContent, t.el('radios-chip').className];"
        )
        assert r == ["Thread on OllyZig CC2652P: radio unplugged", "chip chip-warn"]

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
        assert "ESP32-C6 (built-in)" in r[3]
        # A move has no rollback: a target that does not start keeps Thread, in fault.
        assert "goes back" not in r[3]
        assert "stays on this radio in fault" in r[3]

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
        # On a new port and Thread moved here: the proxy panel is hidden, so only the modes go (off first: rcp-proxy runs).
        r = scenario(
            setup()
            + open_row(1)
            + f"t.el('portIn').type('7000'); {card('otbr')}.click(); await t.flush(); t.el('mSave').click(); await t.advance(10); return "
            + self.puts()
            + ";"
        )
        assert r == [
            ["/device/channels/usb-10c4-ea60-abc123", {"mode": "off"}],
            ["/device/channels/usb-10c4-ea60-abc123", {"mode": "otbr"}],
        ]

    def test_line_settings_then_move(self):
        # On a new baud and Thread moved here: baud PUT first, then the mode PUTs.
        r = scenario(
            setup()
            + open_row(1)
            + f"t.el('baudSel').value = '230400'; {card('otbr')}.click(); await t.flush(); t.el('mSave').click(); await t.advance(10); return "
            + self.puts()
            + ";"
        )
        assert r == [
            ["/device/channels/usb-10c4-ea60-abc123", {"baud": 230400}],
            ["/device/channels/usb-10c4-ea60-abc123", {"mode": "off"}],
            ["/device/channels/usb-10c4-ea60-abc123", {"mode": "otbr"}],
        ]

    def test_otbr_on_a_radio_in_rcp_proxy_sets_it_off_first(self):
        # On otbr picked for a stick that runs rcp-proxy: PUT off, then PUT otbr (the API refuses otbr on a running proxy).
        r = scenario(
            setup()
            + open_row(1)
            + f"{card('otbr')}.click(); await t.flush(); t.el('mSave').click(); await t.advance(10); return "
            + self.puts()
            + ";"
        )
        assert [b for _, b in r] == [{"mode": "off"}, {"mode": "otbr"}]

    def test_otbr_on_an_idle_radio_goes_alone(self):
        # On otbr picked for a stick that is off: one PUT otbr.
        idle = {**OLLY, "mode": "off", "stored_mode": "off"}
        r = scenario(
            setup({"channels": [C6, idle]})
            + open_row(1)
            + f"{card('otbr')}.click(); await t.flush(); t.el('mSave').click(); await t.advance(10); return "
            + self.puts()
            + ";"
        )
        assert [b for _, b in r] == [{"mode": "otbr"}]

    def test_failed_off_put_stops_before_otbr(self):
        # On a refused PUT off (before otbr): otbr is not sent and the reason shows.
        r = scenario(
            setup()
            + "t.put = () => ({status: 409, body: {error: 'busy', detail: 'a move runs'}});"
            + open_row(1)
            + f"{card('otbr')}.click(); await t.flush(); t.el('mSave').click(); await t.advance(10); return [{self.puts()}.length, t.el('mMsg').textContent];"
        )
        assert r[0] == 1
        assert "a move runs" in r[1]

    def test_accepted_start_says_thread_starts_here(self):
        # On 202 for otbr while no radio has Thread: "Starting Thread on" the radio; it settles once Thread runs there.
        idle = {**OLLY, "mode": "off", "stored_mode": "off"}
        c6_off = {**C6, "mode": "off", "stored_mode": "off"}
        r = scenario(
            setup({"channels": [c6_off, idle]}, NO_RADIO)
            + "t.put = () => ({status: 202, body: {moving: true, moved_from: null}});"
            + open_row(1)
            + f"{card('otbr')}.click(); await t.flush(); t.el('mSave').click(); await t.advance(10);"
            + "const first = t.el('rMsg').textContent;"
            + f"t.snap = {{channels: [{json.dumps(c6_off)}, Object.assign({{}}, {json.dumps(idle)}, {{mode: 'otbr', stored_mode: 'otbr', state: 'running'}})]}};"
            + "t.status = {mode: 'BR_RUNNING', otbr_channel: 'usb-10c4-ea60-abc123', fault_reason: null};"
            + "await t.advance(5100); return [first, t.el('rMsg').textContent];"
        )
        assert "Starting Thread on OllyZig CC2652P" in r[0]
        assert "Thread now runs on OllyZig CC2652P" in r[1]

    def test_accepted_stop_says_thread_stops_and_then_that_it_stopped(self):
        # On 202 for off on the radio that runs Thread: "Stopping Thread"; once its stored mode is off: "stopped".
        r = scenario(
            setup()
            + "t.put = () => ({status: 202, body: {moving: true, moved_from: 'builtin-c6'}});"
            + open_row(0)
            + f"{card('off')}.click(); await t.flush(); t.el('mSave').click(); await t.advance(10);"
            + "const first = t.el('rMsg').textContent;"
            + f"t.snap = {{channels: [Object.assign({{}}, {json.dumps(C6)}, {{mode: 'off', stored_mode: 'off', state: 'idle'}})]}};"
            + "t.status = {mode: 'NO_RADIO', otbr_channel: null, fault_reason: 'no channel in otbr'};"
            + "await t.advance(5100); return [first, t.el('rMsg').textContent];"
        )
        assert "Stopping Thread on ESP32-C6 (built-in)" in r[0]
        assert "Thread stopped on ESP32-C6 (built-in)" in r[1]

    def test_refused_stop_says_thread_keeps_running(self):
        # On a stop whose sessions did not end: the radio stays in otbr with error drain_timeout; the page says so.
        r = scenario(
            setup()
            + "t.put = () => ({status: 202, body: {moving: true, moved_from: 'builtin-c6'}});"
            + open_row(0)
            + f"{card('off')}.click(); await t.flush(); t.el('mSave').click(); await t.advance(10);"
            + f"t.snap = {{channels: [Object.assign({{}}, {json.dumps(C6)}, {{error: 'drain_timeout'}})]}};"
            + "await t.advance(5100); return t.el('rMsg').textContent;"
        )
        assert "did not stop" in r and "drain_timeout" in r

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
            + "]}; return {status: 202, body: {moving: true, moved_from: 'builtin-c6'}}; };"
            + open_row(1)
            + f"{card('otbr')}.click(); await t.flush(); t.el('mSave').click(); await t.advance(10);"
            + "const text = t.el('rMsg').textContent; const open = t.el('dlg').open;"
            + "const n0 = t.calls.filter((c) => c.url === '/device/channels' && c.method === 'GET').length;"
            + "await t.advance(4100); const n1 = t.calls.filter((c) => c.url === '/device/channels' && c.method === 'GET').length;"
            + "return [open, text, n1 - n0];"
        )
        assert r[0] is False
        assert "Moving Thread" in r[1] and "ESP32-C6 (built-in)" in r[1]
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
            (409, "probe_failed", "the test run failed"),
            (409, "c6_busy", "busy"),
            (409, "busy", "busy"),
            (409, "drain_timeout", "did not stop in time"),
            (409, "port_taken", "already listens on this port"),
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
            "{mode: 'RADIO_FAULT', otbr_channel: 'builtin-c6'}",
            "{mode: 'BR_RUNNING', otbr_channel: 'builtin-c6'}",
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
                    C.threadNotice({mode: 'NO_RADIO', otbr_channel: 'u1'}), C.threadNotice({mode: 'BR_RUNNING', otbr_channel: 'builtin-c6'}),
                    C.threadNotice({mode: 'RADIO_FAULT', otbr_channel: 'builtin-c6', fault_reason: 'x'})];"""
        )
        assert r[0]["href"] == "/radios.html"
        assert (
            r[1]["text"] == "Packet capture needs a radio in Thread border router mode."
        )
        assert "No radio in OTBR mode" in r[2]["text"] and r[2]["fault"] is False
        assert r[3] is None
        assert r[4]["fault"] is True

    def test_dashboard_shows_the_thread_placeholder_and_the_radios_card(self):
        # On the Dashboard with no OTBR channel: the Thread section's placeholder shows (it replaces the shared
        # notice); with c6 running Thread: the content shows and the Radios card names Thread on the C6.
        r = scenario(
            page("index", ROUTES)
            + f"t.snap = {json.dumps(SNAP)}; t.status = {json.dumps(NO_RADIO)}; t.page.init(); await t.advance(100);"
            + "const a = [t.el('thread-off').hidden(), t.el('thread-on').hidden()];"
            + f"t.status = {json.dumps(RUNNING)}; await t.advance(11000);"
            + "return [a, t.el('thread-off').hidden(), t.el('thread-on').hidden(), t.el('t-radio-c6').textContent];"
        )
        assert r[0] == [False, True]
        assert r[1:] == [True, False, "Thread"]

    def test_dashboard_leaves_a_radio_fault_to_its_status_lines(self):
        # On RADIO_FAULT with no RCP firmware on the C6: the board line says it, the Thread content stays shown.
        r = scenario(
            page("index", ROUTES)
            + "t.snap = {channels: []}; t.status = {mode: 'RADIO_FAULT', otbr_channel: 'builtin-c6', fault_reason: 'no rcp firmware'};"
            + "t.page.init(); await t.advance(100);"
            + "return [t.el('b-title').textContent, t.el('thread-off').hidden()];"
        )
        assert r == ["Board fault", True]

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
        # On the Thread pages and Packet capture: a hidden #threadNotice live region (the Dashboard has its
        # Thread section placeholder instead).
        for name in ["routing", "topology", "capture"]:
            html = (FRONTEND / f"{name}.html").read_text()
            assert 'id="threadNotice"' in html, name
            assert (
                'role="status"' in html.split('id="threadNotice"')[1].split(">")[0]
            ), name


CONSOLE_ROUTES = """(u, o) => {
  if (u === '/device/status') return {status: 200, body: Object.assign(H.fixture(H.STATUS), t.status || {})};
  if (u === '/device/console') return {status: 200, body: {enabled: true, session: null}};
  if (u === '/node') return {status: 200, body: {networkName: 'x'}};
  return {status: 200, body: {}};
}"""


@needs_node
class TestThreadPagesWithoutRadio:
    """Console, Routing and Topology while no radio runs Thread"""

    def test_console_does_not_connect_and_shows_the_notice(self):
        # On NO_RADIO: the notice shows, no WebSocket opens, the pill says Thread is not running.
        r = scenario(
            page("console", CONSOLE_ROUTES)
            + f"t.status = {json.dumps(NO_RADIO)}; t.page.init(); await t.advance(100);"
            + "return [t.sockets.length, t.el('threadNotice').hidden(), t.el('threadNotice').allText(), t.el('con-pill').textContent];"
        )
        assert r[0] == 0
        assert r[1] is False
        assert "Open Radios" in r[2]
        assert r[3] == "Thread not running"

    def test_console_connects_when_the_radio_comes_back(self):
        # On a radio that returns: the page tries again by itself, hides the notice and connects.
        r = scenario(
            page("console", CONSOLE_ROUTES)
            + f"t.status = {json.dumps(NO_RADIO)}; t.page.init(); await t.advance(100);"
            + f"t.status = {json.dumps(RUNNING)}; await t.advance(6000);"
            + "return [t.sockets.length, t.el('threadNotice').hidden()];"
        )
        assert r == [1, True]

    def test_failed_handshake_names_the_missing_radio(self):
        # On a handshake that fails while no radio runs Thread: the notice shows and the page does not retry blindly.
        r = scenario(
            page("console", CONSOLE_ROUTES)
            + f"t.status = {json.dumps(RUNNING)}; t.page.init(); await t.advance(100);"
            + f"t.status = {json.dumps(NO_RADIO)}; t.sockets[0].drop(1006); await t.advance(100);"
            + "return [t.sockets.length, t.el('threadNotice').hidden(), t.el('con-pill').textContent];"
        )
        assert r == [1, False, "Thread not running"]

    def test_failed_handshake_with_thread_up_retries(self):
        # On a handshake that fails while Thread runs: the usual reconnect.
        r = scenario(
            page("console", CONSOLE_ROUTES)
            + f"t.status = {json.dumps(RUNNING)}; t.page.init(); await t.advance(100);"
            + "t.sockets[0].drop(1006); await t.advance(100);"
            + "return [t.el('threadNotice').hidden(), t.el('con-pill').textContent];"
        )
        assert r == [True, "Reconnecting"]

    def test_routing_shows_the_notice_instead_of_the_raw_503(self):
        # On a 503 from /device/routing in NO_RADIO: the notice shows and the stale line stays empty.
        r = scenario(
            page(
                "routing",
                ROUTES.replace(
                    "if (u === '/device/channels')",
                    "if (u === '/device/routing') return {status: 503, body: {error: 'unavailable', detail: 'no radio in OTBR mode'}};\n  if (u === '/device/channels')",
                ),
            )
            + f"t.status = {json.dumps(NO_RADIO)}; t.page.init(); await t.advance(200);"
            + "return [t.el('threadNotice').hidden(), t.el('r-stale').allText(), t.el('r-stale').hidden()];"
        )
        assert r[0] is False
        assert "no radio in OTBR mode" not in r[1]
        assert r[2] is True

    def test_routing_still_shows_a_503_when_thread_is_up(self):
        # On a 503 with Thread up: the raw error stays visible.
        r = scenario(
            page(
                "routing",
                ROUTES.replace(
                    "if (u === '/device/channels')",
                    "if (u === '/device/routing') return {status: 503, body: {error: 'unavailable', detail: 'starting'}};\n  if (u === '/device/channels')",
                ),
            )
            + f"t.status = {json.dumps(RUNNING)}; t.page.init(); await t.advance(200);"
            + "return t.el('r-stale').textContent;"
        )
        assert "could not be read" in r

    def test_topology_scan_start_shows_the_notice_instead_of_the_raw_503(self):
        # On a 503 from the scan start in NO_RADIO: the notice shows and the start note has no raw error.
        r = scenario(
            page(
                "topology",
                ROUTES.replace(
                    "if (u === '/device/channels')",
                    "if (u === '/device/topology') return {status: 503, body: {error: 'unavailable', detail: 'no radio in OTBR mode'}};\n  if (u === '/device/channels')",
                ),
            )
            + f"t.status = {json.dumps(NO_RADIO)}; t.page.init(); await t.advance(200);"
            + "t.el('t-load').click(); await t.advance(200);"
            + "return [t.el('threadNotice').hidden(), t.el('t-start-note').textContent];"
        )
        assert r[0] is False
        assert "no radio in OTBR mode" not in r[1]


THREAD_PAGES = [
    "index",
    "network",
    "topology",
    "routing",
    "commission",
    "addresses",
    "tools",
    "console",
]


class TestSharedBehaviour:
    """Thread pages in a browser-like scope"""

    @pytest.mark.parametrize("name", THREAD_PAGES)
    def test_every_thread_page_has_a_notice_slot_or_the_mode_guard(self, name):
        # On each Thread-menu page and the Dashboard: a #threadNotice live region, MgmtPage.guard (whose banner says
        # it), or the Dashboard's Thread section placeholder.
        html = (FRONTEND / f"{name}.html").read_text()
        assert (
            'id="threadNotice"' in html
            or "MgmtPage.guard(" in html
            or 'id="thread-off"' in html
        )

    @pytest.mark.parametrize("name", ["network", "commission", "addresses", "tools"])
    @needs_node
    def test_management_scripts_run_without_require(self, name):
        # On the scripts a management page loads up to mgmt.js, in a scope without require: modeBanner answers NO_RADIO.
        import re
        import subprocess

        html = (FRONTEND / f"{name}.html").read_text()
        tags = re.findall(r'<script src="/static/([^"]+)"></script>', html)
        upto = tags[: tags.index("mgmt.js") + 1]
        assert "device_common.js" in upto
        assert upto.index("device_common.js") < upto.index("mgmt.js")
        source = (
            "const vm = require('vm'); const fs = require('fs');"
            "const ctx = vm.createContext({document: {getElementById: () => null}});"
            f"for (const f of {json.dumps(upto)}) vm.runInContext(fs.readFileSync({json.dumps(str(FRONTEND / 'static'))} + '/' + f, 'utf8'), ctx);"
            "process.stdout.write(JSON.stringify(vm.runInContext(\"[MgmtLogic.modeBanner({mode: 'NO_RADIO', otbr_channel: null}), "
            "MgmtLogic.modeBanner({mode: 'BR_RUNNING', otbr_channel: 'builtin-c6'}), typeof MgmtPage]\", ctx)));"
        )
        out = subprocess.run(
            ["node", "-e", source],
            capture_output=True,
            text=True,
            check=True,
            timeout=20,
        )
        banner, running, kind = json.loads(out.stdout)
        assert banner["href"] == "/radios.html"
        assert running is None
        assert kind == "object"


EZ_FIRST = {"channels": [EZSP, C6, OLLY]}


@needs_node
class TestMoveSource:
    """Which radio a move takes Thread from"""

    def test_the_boards_otbr_channel_wins_over_stored_modes(self):
        # On the EZSP stick (stored otbr, forced off) listed before the C6 that runs Thread: Thread comes from the C6.
        r = scenario(
            setup(EZ_FIRST)
            + open_row(2)
            + f"{card('otbr')}.click(); await t.flush(); return [t.el('mSave').textContent, t.el('otbrNote').textContent];"
        )
        assert r[0] == "Move Thread here"
        assert "from ESP32-C6 (built-in)" in r[1]
        assert "Sonoff" not in r[1]

    def test_the_accepted_move_names_the_boards_radio_without_moved_from(self):
        # On a 202 without moved_from: the message names the C6, not the first stored-otbr stick.
        r = scenario(
            setup(EZ_FIRST)
            + "t.put = () => ({status: 202, body: {moving: true}});"
            + open_row(2)
            + f"{card('otbr')}.click(); await t.flush(); t.el('mSave').click(); await t.advance(10); return t.el('rMsg').textContent;"
        )
        assert "from ESP32-C6 (built-in)" in r

    def test_stored_modes_are_the_fallback_when_no_channel_runs_thread(self):
        # On otbr_channel null: the stick stored otbr is the source of the move.
        r = scenario(
            setup(EZ_FIRST, NO_RADIO)
            + open_row(2)
            + f"{card('otbr')}.click(); await t.flush(); return t.el('otbrNote').textContent;"
        )
        assert "from Sonoff ZBDongle-E" in r

    def test_the_radio_that_runs_thread_has_no_source(self):
        # On the C6 itself: Save, no move.
        r = scenario(
            setup(EZ_FIRST) + open_row(1) + "return t.el('mSave').textContent;"
        )
        assert r == "Save"


@needs_node
class TestSaveFailureText:
    """RadiosLogic.saveFailureText"""

    def test_a_single_refused_put_gives_its_reason_alone(self):
        # On the only PUT refused: the refusal's text, nothing about earlier steps.
        r = logic_of(
            "L.saveFailureText([{mode: 'off'}], [{status: 409, body: {error: 'mode_taken'}}])"
        )
        assert r == "Another radio already runs this mode."

    def test_a_lost_connection_after_off_names_the_switch_off(self):
        # On off accepted and the otbr PUT's connection lost: the radio was switched off, then the message.
        r = logic_of(
            "L.saveFailureText([{mode: 'off'}, {mode: 'otbr'}], [{status: 200, body: {}}, {status: 0, message: 'timeout'}])"
        )
        assert r.startswith("The radio was switched off") and r.endswith("timeout")


@needs_node
class TestTwoStepSave:
    """Both PUTs of a save: their replies, a refused second one, a lost connection"""

    def test_reboot_from_the_first_reply_counts(self):
        # On a settings PUT that says reboot and a mode PUT that does not: the page says the board reboots.
        r = scenario(
            setup()
            + "t.put = (u, b) => b.baud ? {status: 200, body: {reboot: true}} : {status: 200, body: {}};"
            + open_row(1)
            + f"t.el('baudSel').value = '230400'; {card('off')}.click(); await t.flush(); t.el('mSave').click(); await t.advance(10);"
            + "return t.el('rMsg').textContent;"
        )
        assert "reboot" in r.lower()

    def test_mode_refused_after_settings_saved_says_both(self):
        # On a refused mode PUT after saved settings: the dialog says the settings were saved and why the mode failed.
        r = scenario(
            setup()
            + "t.put = (u, b) => b.mode ? {status: 409, body: {error: 'mode_taken', detail: 'x'}} : {status: 200, body: {}};"
            + open_row(1)
            + f"t.el('baudSel').value = '230400'; {card('otbr')}.click(); await t.flush(); t.el('mSave').click(); await t.advance(10);"
            + "return [t.el('dlg').open, t.el('mMsg').textContent];"
        )
        assert r[0] is True
        assert "settings were saved" in r[1] and "mode change failed" in r[1]
        assert "another radio" in r[1].lower()

    def test_otbr_refused_after_the_radio_was_switched_off_says_so(self):
        # On a refused otbr PUT after the PUT off of a proxy radio went through: the dialog says the radio was switched
        # off and why Thread did not start (the refusal's detail), never that settings were saved.
        r = scenario(
            setup()
            + "t.put = (u, b) => b.mode === 'otbr' ? {status: 409, body: {error: 'probe_failed', detail: 'no spinel answer'}} : {status: 200, body: {}};"
            + open_row(1)
            + f"{card('otbr')}.click(); await t.flush(); t.el('mSave').click(); await t.advance(10);"
            + "return [t.el('dlg').open, t.el('mMsg').textContent];"
        )
        assert r[0] is True
        assert "switched off" in r[1] and "Thread did not start" in r[1]
        assert "no spinel answer" in r[1]
        assert "settings were saved" not in r[1]

    def test_otbr_refused_after_settings_and_off_says_both(self):
        # On settings and the PUT off saved, then otbr refused: the text says both happened and why Thread did not start.
        r = scenario(
            setup()
            + "t.put = (u, b) => b.mode === 'otbr' ? {status: 409, body: {error: 'busy', detail: 'a move runs'}} : {status: 200, body: {}};"
            + open_row(1)
            + f"t.el('baudSel').value = '230400'; {card('otbr')}.click(); await t.flush(); t.el('mSave').click(); await t.advance(10);"
            + "return t.el('mMsg').textContent;"
        )
        assert "settings were saved and the radio was switched off" in r
        assert "a move runs" in r

    def test_list_is_read_again_after_a_refused_put(self):
        # On a refused PUT: the list is read again (the settings may have gone through).
        r = scenario(
            setup()
            + "t.put = () => ({status: 409, body: {error: 'mode_taken'}});"
            + open_row(1)
            + f"{card('off')}.click(); await t.flush(); const n = t.calls.length; t.el('mSave').click(); await t.advance(10);"
            + "return t.calls.slice(n).filter((c) => c.url === '/device/channels' && c.method === 'GET').length;"
        )
        assert r >= 1

    def test_lost_connection_on_the_second_put_is_reported_and_the_list_refreshed(self):
        # On a dropped connection during the mode PUT: the text says the settings were saved; the list is read again.
        r = scenario(
            setup()
            + "t.put = (u, b) => { if (b.mode) throw new Error('drop'); return {status: 200, body: {}}; };"
            + open_row(1)
            + f"t.el('baudSel').value = '230400'; {card('otbr')}.click(); await t.flush(); const n = t.calls.length; t.el('mSave').click(); await t.advance(10);"
            + "return [t.el('mMsg').textContent, t.calls.slice(n).filter((c) => c.url === '/device/channels' && c.method === 'GET').length];"
        )
        assert "settings were saved" in r[0]
        assert r[1] >= 1


OLLY_STARTING = {
    **OLLY,
    "mode": "otbr",
    "stored_mode": "otbr",
    "state": "starting",
    "id": "usb-10c4-ea60-abc123",
}


@needs_node
class TestSettleMove:
    """When a move counts as failed"""

    def accept(self, after_js=""):
        return (
            setup()
            + after_js
            + "t.put = () => ({status: 202, body: {moving: true, moved_from: 'builtin-c6'}});"
            + open_row(1)
            + f"{card('otbr')}.click(); await t.flush(); t.el('mSave').click();"
        )

    def test_first_refresh_after_the_202_is_never_a_failure(self):
        # On a 202 followed at once by a list that still shows the old picture: the move is not called failed.
        r = scenario(
            self.accept() + "await t.advance(10); return t.el('rMsg').textContent;"
        )
        assert "Moving Thread" in r
        assert "did not start" not in r

    def test_target_seen_starting_then_settled_without_thread_is_a_failure(self):
        # On a target that starts and then falls to a fault while Thread still runs on the C6 (drain_timeout): the move
        # failed and Thread did not move.
        starting = json.dumps({"channels": [C6, OLLY_STARTING]})
        fault = json.dumps(
            {
                "channels": [
                    {**C6, "mode": "otbr"},
                    {**OLLY, "state": "fault", "error": "drain_timeout"},
                ]
            }
        )
        r = scenario(
            self.accept(f"t.status = {json.dumps(RUNNING)};")
            + f"await t.advance(10); t.snap = {starting}; await t.advance(2500);"
            + f"t.snap = {fault}; await t.advance(2500); return t.el('rMsg').textContent;"
        )
        assert "did not move" in r and "drain_timeout" in r

    def test_target_becoming_the_otbr_channel_is_success(self):
        # On a target that starts and then becomes otbr_channel: "Thread now runs on".
        starting = json.dumps(
            {
                "channels": [
                    {**C6, "mode": "off", "stored_mode": "off"},
                    OLLY_STARTING,
                ]
            }
        )
        done = json.dumps(
            {
                "channels": [
                    {**C6, "mode": "off", "stored_mode": "off"},
                    {**OLLY, "mode": "otbr", "stored_mode": "otbr"},
                ]
            }
        )
        r = scenario(
            self.accept()
            + f"await t.advance(10); t.snap = {starting}; await t.advance(2500);"
            + f"t.snap = {done}; t.status = {json.dumps({'mode': 'BR_RUNNING', 'otbr_channel': 'usb-10c4-ea60-abc123'})}; await t.advance(2500);"
            + "return t.el('rMsg').textContent;"
        )
        assert "Thread now runs on OllyZig CC2652P" in r

    def test_committed_target_whose_start_failed_is_a_failure_and_stays_there(self):
        # On a move committed to the target (otbr_channel is it) whose start then failed (RADIO_FAULT, the target in
        # fault): the move failed, Thread stays on the target in fault; no word of going back to the old radio.
        starting = json.dumps({"channels": [C6, OLLY_STARTING]})
        fault = json.dumps(
            {
                "channels": [
                    {**C6, "mode": "off", "stored_mode": "off"},
                    {
                        **OLLY,
                        "mode": "otbr",
                        "stored_mode": "otbr",
                        "state": "fault",
                        "error": "rcp incompatible",
                    },
                ]
            }
        )
        failed = {
            "mode": "RADIO_FAULT",
            "otbr_channel": "usb-10c4-ea60-abc123",
            "fault_reason": "rcp incompatible",
        }
        r = scenario(
            self.accept()
            + f"await t.advance(10); t.snap = {starting}; await t.advance(2500);"
            + f"t.snap = {fault}; t.status = {json.dumps(failed)}; await t.advance(2500);"
            + "return t.el('rMsg').textContent;"
        )
        assert "did not start" in r and "rcp incompatible" in r, r
        assert "now runs" not in r
        assert "previous radio" not in r
        assert "stays" in r

    def test_drain_timeout_keeps_thread_on_the_source_and_says_retry(self):
        # On a move refused after the 202 because sessions did not end (the target in fault with drain_timeout,
        # otbr_channel still the source): Thread runs on where it was; the text says so and asks for a retry.
        fault = json.dumps(
            {"channels": [C6, {**OLLY, "state": "fault", "error": "drain_timeout"}]}
        )
        kept = {
            "mode": "BR_RUNNING",
            "otbr_channel": "builtin-c6",
            "fault_reason": None,
        }
        r = scenario(
            self.accept(f"t.status = {json.dumps(kept)};")
            + f"await t.advance(10); t.snap = {fault}; await t.advance(2500); await t.advance(2500);"
            + "return t.el('rMsg').textContent;"
        )
        assert "did not move" in r, r
        assert "still runs on ESP32-C6 (built-in)" in r, r
        assert "try" in r.lower(), r
        assert "in fault" not in r, r

    def test_committed_target_in_fault_with_stale_status_is_not_called_a_drain_timeout(
        self,
    ):
        # On a target already stored otbr (the move committed) in fault while /device/status still names the source (a
        # status read that failed or lags): never "still runs on" the source; the target is where Thread stays.
        fault = json.dumps(
            {
                "channels": [
                    {**C6, "mode": "off", "stored_mode": "off"},
                    {
                        **OLLY,
                        "mode": "otbr",
                        "stored_mode": "otbr",
                        "state": "fault",
                        "error": "rcp incompatible",
                    },
                ]
            }
        )
        stale = {
            "mode": "BR_RUNNING",
            "otbr_channel": "builtin-c6",
            "fault_reason": None,
        }
        r = scenario(
            self.accept(f"t.status = {json.dumps(stale)};")
            + f"await t.advance(10); t.snap = {fault}; await t.advance(2500); await t.advance(2500);"
            + "return t.el('rMsg').textContent;"
        )
        assert "still runs on" not in r, r

    def test_nothing_after_90_seconds_is_a_failure(self):
        # On a target never seen starting and never becoming otbr_channel: failed after 90 s, not before.
        r = scenario(
            self.accept()
            + "await t.advance(60000); const early = t.el('rMsg').textContent; await t.advance(40000);"
            + "return [early, t.el('rMsg').textContent];"
        )
        assert "Moving Thread" in r[0]
        assert "did not start" in r[1]


RESTART = "Saving reboots the board. Thread restarts with it: its devices lose the border router for about 20 s."
REBOOT_ONLY = "Saving reboots the board."
OLLY_ZIG = {**OLLY, "mode": "ziggurat", "stored_mode": "ziggurat"}
SNAP_HYBRID = {**SNAP_ZIG, "channels": [C6, OLLY_ZIG, SLZB, EZSP]}


@needs_node
class TestRebootNote:
    """RadiosLogic.rebootNote"""

    def note(self, snap, ch, pick, status):
        return logic_of(
            f"L.rebootNote({json.dumps(snap)}, {json.dumps(ch)}, {json.dumps(pick)}, {json.dumps(status)})"
        )

    def test_entering_zigbee_while_thread_runs_elsewhere_says_thread_restarts(self):
        # On ziggurat picked on the OllyZig while Thread runs on the C6: the restart text.
        assert self.note(SNAP_ZIG, OLLY, "ziggurat", RUNNING) == RESTART

    def test_leaving_zigbee_while_thread_runs_elsewhere_says_thread_restarts(self):
        # On off picked on the OllyZig that runs Zigbee while Thread runs on the C6: the restart text.
        assert self.note(SNAP_HYBRID, OLLY_ZIG, "off", RUNNING) == RESTART

    def test_otbr_on_zigbees_radio_says_the_move_reboots(self):
        # On otbr picked on the OllyZig that runs Zigbee while Thread runs on the C6: the move-with-reboot text.
        assert self.note(SNAP_HYBRID, OLLY_ZIG, "otbr", RUNNING) == (
            "Saving reboots the board and moves Thread from ESP32-C6 (built-in) to this radio; Zigbee stops. "
            "Thread devices lose the border router for about 20 s."
        )

    def test_entering_zigbee_without_thread_says_only_the_reboot(self):
        # On ziggurat picked while no radio runs Thread: the reboot, nothing about Thread.
        c6_off = {**C6, "mode": "off", "stored_mode": "off"}
        ezsp_off = {**EZSP, "stored_mode": "off"}  # the fixture stores otbr there
        snap = {**SNAP_ZIG, "channels": [c6_off, OLLY, SLZB, ezsp_off]}
        assert self.note(snap, OLLY, "ziggurat", NO_RADIO) == REBOOT_ONLY

    def test_leaving_zigbee_without_thread_says_only_the_reboot(self):
        # On off picked on the OllyZig that runs Zigbee while no radio runs Thread: the reboot only.
        c6_off = {**C6, "mode": "off", "stored_mode": "off"}
        ezsp_off = {**EZSP, "stored_mode": "off"}
        snap = {**SNAP_ZIG, "channels": [c6_off, OLLY_ZIG, SLZB, ezsp_off]}
        assert self.note(snap, OLLY_ZIG, "off", NO_RADIO) == REBOOT_ONLY

    def test_otbr_on_zigbees_radio_without_thread_says_thread_starts_through_a_reboot(
        self,
    ):
        # On otbr picked on the OllyZig that runs Zigbee while no radio runs Thread: the start-with-reboot text.
        c6_off = {**C6, "mode": "off", "stored_mode": "off"}
        ezsp_off = {**EZSP, "stored_mode": "off"}
        snap = {**SNAP_ZIG, "channels": [c6_off, OLLY_ZIG, SLZB, ezsp_off]}
        assert self.note(snap, OLLY_ZIG, "otbr", NO_RADIO) == (
            "Saving reboots the board and starts Thread on this radio; Zigbee stops."
        )

    def test_an_unplugged_radio_stored_as_zigbee_says_nothing(self):
        # On the stored ziggurat picked on an unplugged OllyZig while Thread runs on the C6: "" (no mode change is sent).
        unplugged = {**OLLY, "mode": "unavailable", "stored_mode": "ziggurat"}
        snap = {**SNAP_ZIG, "channels": [C6, unplugged, SLZB, EZSP]}
        assert self.note(snap, unplugged, "ziggurat", RUNNING) == ""

    def test_zigbee_on_the_radio_thread_runs_on_says_thread_stops(self):
        # On ziggurat picked on the C6 while Thread runs on the C6: Thread stops for good (no other radio runs it).
        assert self.note(SNAP_ZIG, C6, "ziggurat", RUNNING) == (
            "Saving reboots the board and starts Zigbee on this radio. Thread stops: no other radio runs it, so its "
            "devices lose the border router until you set another radio to Thread."
        )

    def test_a_change_that_does_not_touch_zigbee_says_nothing(self):
        # On rcp-proxy picked on the OllyZig (off) while Thread runs on the C6: "".
        assert self.note(SNAP_ZIG, OLLY, "rcp-proxy", RUNNING) == ""


@needs_node
class TestRebootNoteDialog:
    """Radios page dialog: #zigbeeNote"""

    def test_picking_zigbee_shows_the_restart_note(self):
        # On ziggurat picked on the OllyZig while Thread runs on the C6: #zigbeeNote shows the restart text.
        r = scenario(
            setup(SNAP_ZIG)
            + open_row(1)
            + f"{card('ziggurat')}.click(); await t.flush();"
            + "return [t.el('zigbeeNote').hidden(), t.el('zigbeeNote').textContent];"
        )
        assert r == [False, RESTART]

    def test_picking_a_mode_that_does_not_touch_zigbee_hides_the_note(self):
        # On rcp-proxy picked on the OllyZig: #zigbeeNote hidden.
        r = scenario(
            setup(SNAP_ZIG)
            + open_row(1)
            + f"{card('rcp-proxy')}.click(); await t.flush();"
            + "return t.el('zigbeeNote').hidden();"
        )
        assert r is True

    def test_otbr_on_zigbees_radio_puts_the_reboot_text_in_the_otbr_note(self):
        # On otbr picked on the OllyZig that runs Zigbee: #otbrNote has the move-with-reboot text, #zigbeeNote hidden.
        r = scenario(
            setup(SNAP_HYBRID)
            + open_row(1)
            + f"{card('otbr')}.click(); await t.flush();"
            + "return [t.el('otbrNote').textContent, t.el('zigbeeNote').hidden()];"
        )
        assert r[0].startswith(
            "Saving reboots the board and moves Thread from ESP32-C6 (built-in)"
        )
        assert r[1] is True

    def test_no_text_for_hybrid_not_supported_is_left(self):
        # On the page's code texts: no hybrid_not_supported entry (the board no longer sends it).
        src = (FRONTEND / "static" / "radios.js").read_text()
        assert "hybrid_not_supported" not in src


# While USB/IP is on (spec 2026-11-10 section 4): the USB channels are `unavailable` with a channel_reason, and the
# snapshot says "usbip": true.
USBIP_REASON = "USB is exported over USB/IP"
SLZB_EXT = {**SLZB, "channel_reason": USBIP_REASON}
OLLY_EXT = {
    **OLLY,
    "mode": "unavailable",
    "state": "idle",
    "channel_reason": USBIP_REASON,
}
SNAP_USBIP = {"usbip": True, "channels": [C6, OLLY_EXT, SLZB_EXT]}
USBIP_RUNNING = {
    "mode": "BR_RUNNING",
    "otbr_channel": "builtin-c6",
    "fault_reason": None,
}


def links(row_js):
    return f"{row_js}.findAll((e) => e.tag === 'a').map((a) => [a.attrs.href, a.textContent])"


@needs_node
class TestUsbipOn:
    """RadiosLogic.external, RadiosPage while "usbip" is true"""

    def test_external_is_a_usb_channel_while_usbip_is_on(self):
        # On the snapshot with usbip true: the USB rows are external, the C6 is not; without the flag none is.
        assert logic_of(
            f"[L.external({json.dumps(SNAP_USBIP)}, {json.dumps(SLZB_EXT)}),"
            f" L.external({json.dumps(SNAP_USBIP)}, {json.dumps(C6)}),"
            f" L.external({json.dumps(SNAP)}, {json.dumps(SLZB)})]"
        ) == [True, False, False]

    def test_usb_rows_show_the_reason_with_a_link_to_the_usbip_page(self):
        # On a USB row while USB/IP is on: its activity cell holds the reason and a link to /usbip.html.
        r = scenario(
            setup(SNAP_USBIP, USBIP_RUNNING)
            + f"const row = {rows_js()}[2];"
            + f"return [row.children[4].allText(), {links('row.children[4]')}, row.children[3].allText()];"
        )
        assert USBIP_REASON in r[0]
        assert r[1] == [["/usbip.html", "Open USB/IP"]]
        assert "Unplugged" not in r[2]

    def test_other_rows_have_no_usbip_link(self):
        # On the C6 row: no link to the USB/IP page.
        r = scenario(
            setup(SNAP_USBIP, USBIP_RUNNING) + f"return {links(rows_js() + '[0]')};"
        )
        assert r == []

    def test_usb_row_keeps_the_stored_mode_in_words(self):
        # On the SLZB-06 (stored rcp-proxy): the mode cell says the stored mode applies again after USB/IP.
        r = scenario(
            setup(SNAP_USBIP, USBIP_RUNNING)
            + f"return {rows_js()}[2].children[2].allText();"
        )
        assert "RCP proxy" in r
        assert "USB/IP" in r

    def test_mode_selector_is_disabled_for_usb_channels(self):
        # On a USB row's dialog: every mode card is disabled and says why; on the C6 the first three are not.
        r = scenario(
            setup(SNAP_USBIP, USBIP_RUNNING)
            + open_row(2)
            + "const usb = t.el('track').children.map((c) => [c.disabled, c.allText().includes('USB/IP')]);"
            "t.el('mCancel').click();"
            + open_row(0)
            + "const c6 = t.el('track').children.map((c) => c.disabled);"
            "return [usb, c6];"
        )
        assert all(d and why for d, why in r[0])
        assert r[1][:3] == [
            False,
            False,
            False,
        ]  # ziggurat is not in this snapshot's build

    def test_dialog_explains_and_links_instead_of_unplugged(self):
        # On a USB row's dialog: the note gives the reason (not "Unplugged") and the link to USB/IP shows.
        r = scenario(
            setup(SNAP_USBIP, USBIP_RUNNING)
            + open_row(2)
            + "return [t.el('mUnavailable').hidden(), t.el('mUnavailable').textContent, t.el('mUsbipLink').hidden()];"
        )
        assert r[0] is False
        assert USBIP_REASON in r[1]
        assert "Unplugged" not in r[1]
        assert r[2] is False

    def test_dialog_of_the_c6_hides_the_usbip_link(self):
        # On the C6's dialog: no link.
        r = scenario(
            setup(SNAP_USBIP, USBIP_RUNNING)
            + open_row(0)
            + "return t.el('mUsbipLink').hidden();"
        )
        assert r is True

    def test_line_settings_can_still_be_saved_without_a_mode(self):
        # On changing only the baud of a USB channel: the PUT carries the baud and no mode.
        r = scenario(
            setup(SNAP_USBIP, USBIP_RUNNING)
            + "const sent = []; t.put = (u, b) => { sent.push(b); return {status: 200, body: {}}; };"
            + open_row(2)
            + "t.el('baudSel').value = '230400'; t.el('mSave').click(); await t.advance(100); return sent;"
        )
        assert r == [{"baud": 230400}]

    def test_without_usbip_an_unplugged_stick_is_unchanged(self):
        # On the plain snapshot: the unplugged SLZB-06 shows Unplugged and no link.
        r = scenario(
            setup()
            + f"const row = {rows_js()}[2];"
            + f"return [row.children[3].allText(), {links('row.children[4]')}];"
        )
        assert r == ["Unplugged", []]

    def test_hostile_channel_reason_stays_text(self):
        # On markup in channel_reason: the row and the dialog show it as text; no element is made from it.
        evil = "<img src=x onerror=alert(1)>"
        ch = {**SLZB_EXT, "channel_reason": evil}
        r = scenario(
            setup({"usbip": True, "channels": [C6, ch]}, USBIP_RUNNING)
            + f"const row = {rows_js()}[1]; const a = row.children[4].allText();"
            + open_row(1)
            + "const imgs = Object.values(t.doc.byId).filter((e) => e.findAll((x) => x.tag === 'img').length);"
            "return [a, t.el('mUnavailable').textContent, imgs.length];"
        )
        assert evil in r[0]
        assert evil in r[1]
        assert r[2] == 0
