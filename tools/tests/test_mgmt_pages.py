"""Thread management pages (Network, Commissioner, Addresses, Tools): static/mgmt.js and the page markup.

Host-only: the pure MgmtLogic under node, and static checks of the pages.
"""

import re

import pytest
from device_js import FRONTEND, logic, needs_node

MGMT_PAGES = ["network", "commission", "addresses", "tools"]
LEGACY_PAGES = MGMT_PAGES + ["about"]
WELL_KNOWN = [
    "00112233445566778899aabbccddeeff",  # the example network key
    "1111111122222222",  # the example ext PAN ID
    "j01Nme",
    "J01NME",  # the example passphrase / PSKd
]


def m(expression):
    return logic("mgmt", expression)


# Deterministic stand-in for crypto.getRandomValues: bytes 0, 1, 2, ... per call.
RND = "(n) => Array.from({length: n}, (_, i) => (i * 37 + 11) & 0xff)"


@needs_node
class TestModeBanner:
    def test_running_needs_no_banner(self):
        assert m("L.modeBanner({mode: 'BR_RUNNING'})") is None

    def test_radio_fault_names_the_reason_and_links_firmware(self):
        b = m(
            "L.modeBanner({mode: 'RADIO_FAULT', fault_reason: 'esp-hosted firmware'})"
        )
        assert "esp-hosted firmware" in b["text"]
        assert b["href"] == "/firmware.html"

    def test_sniffer_links_the_capture(self):
        b = m("L.modeBanner({mode: 'SNIFFER'})")
        assert b["href"] == "/capture.html"
        assert "capture" in b["text"]

    def test_starting_says_to_wait(self):
        assert "starting" in m("L.modeBanner({mode: 'STARTING'})")["text"]

    def test_no_answer(self):
        assert "No answer" in m("L.modeBanner(null)")["text"]


@needs_node
class TestRandomNetwork:
    def test_fields_have_the_right_shape_and_pass_validation(self):
        r = m(f"L.randomNetwork({RND})")
        assert re.fullmatch(r"[0-9a-f]{32}", r["networkKey"])
        assert re.fullmatch(r"0x[0-9a-f]{4}", r["panId"])
        assert re.fullmatch(r"[0-9a-f]{16}", r["extPanId"])
        assert re.fullmatch(r"fd[0-9a-f]{2}:[0-9a-f]{4}:[0-9a-f]{4}::", r["prefix"])
        assert len(r["passphrase"]) == 12
        assert r["networkName"].startswith("Thread-") and len(r["networkName"]) <= 16
        assert (
            m(
                f"L.validateForm(Object.assign(L.randomNetwork({RND}), {{channel: '15'}}))"
            )
            == []
        )

    def test_never_the_broadcast_pan_id(self):
        assert m("L.randomNetwork((n) => Array(n).fill(0xff)).panId") == "0xfffe"

    def test_two_draws_differ(self):
        a = m(
            "L.randomNetwork((n) => Array.from({length: n}, () => Math.random() * 256 | 0)).networkKey"
        )
        b = m(
            "L.randomNetwork((n) => Array.from({length: n}, () => Math.random() * 256 | 0)).networkKey"
        )
        assert a != b


GOOD = (
    "{networkName: 'Home', networkKey: 'aa'.repeat(16), panId: '0xbeef', channel: '20',"
    " extPanId: 'ab'.repeat(8), passphrase: 'secret12', prefix: 'fd12:3456:789a::'}"
)


@needs_node
class TestValidateForm:
    def test_good_form_passes(self):
        assert m(f"L.validateForm({GOOD})") == []

    @pytest.mark.parametrize(
        ("patch", "field"),
        [
            ("{networkName: ''}", "networkName"),
            ("{networkName: 'x'.repeat(17)}", "networkName"),
            ("{networkKey: 'abc'}", "networkKey"),
            ("{panId: '1234'}", "panId"),
            ("{panId: '0xffff'}", "panId"),
            ("{channel: '10'}", "channel"),
            ("{channel: '27'}", "channel"),
            ("{extPanId: '12'}", "extPanId"),
            ("{passphrase: 'short'}", "passphrase"),
            ("{prefix: 'fd12::/64'}", "prefix"),
            ("{prefix: 'fd12:3456:789a::::'}", "prefix"),
            ("{prefix: 'fd12:3456:789a:bcde:1::'}", "prefix"),
        ],
    )
    def test_each_bad_field_is_named(self, patch, field):
        out = m(f"L.validateForm(Object.assign({GOOD}, {patch}))")
        assert [p["field"] for p in out] == [field]


@needs_node
class TestPskdAndConfirm:
    @pytest.mark.parametrize(
        "pskd", ["ABCDEF", "J01NME", "123456789ABCDEFGHJKLMNPRSTUVWXY1"]
    )
    def test_valid_pskd(self, pskd):
        assert m(f"L.checkPskd('{pskd}')") is None

    @pytest.mark.parametrize(
        "pskd",
        ["", "ABCDE", "abcdef", "ABCDEI", "ABCDEO", "ABCDEQ", "ABCDEZ", "A" * 33],
    )
    def test_invalid_pskd(self, pskd):
        assert m(f"L.checkPskd('{pskd}')")

    def test_replace_question_names_the_current_network(self):
        q = m(
            "L.replaceQuestion('Form the network X on channel 15', {name: 'Home', channel: 20})"
        )
        assert (
            "Home" in q
            and "joined again" in q
            and q.endswith("Form the network X on channel 15?")
        )

    def test_replace_question_without_a_network_just_asks(self):
        assert m("L.replaceQuestion('Join Y', null)") == "Join Y?"

    def test_replace_question_when_unreadable_still_warns(self):
        assert "could not be read" in m("L.replaceQuestion('Join Y', {unknown: true})")


@pytest.mark.parametrize("name", LEGACY_PAGES)
class TestLegacyPages:
    def test_no_well_known_credentials_are_prefilled(self, name):
        html = (FRONTEND / f"{name}.html").read_text()
        for value in WELL_KNOWN:
            assert value not in html, value

    def test_footer_is_the_project_footer(self, name):
        html = (FRONTEND / f"{name}.html").read_text()
        assert "All rights reserved" not in html
        assert "https://github.com/t3hk0d3/esp32-p4-otbr" in html

    def test_every_form_label_names_its_field(self, name):
        html = (FRONTEND / f"{name}.html").read_text()
        for label in re.findall(r'<label class="form-label"[^>]*>', html):
            assert "for=" in label, label


@pytest.mark.parametrize("name", MGMT_PAGES)
def test_management_page_is_guarded_by_the_mode(name):
    # On every management page: mgmt.js after api.js, and the mode guard instead of the role-only check.
    html = (FRONTEND / f"{name}.html").read_text()
    assert html.index("/static/api.js") < html.index("/static/mgmt.js")
    assert "MgmtPage.guard(" in html
    assert "checkThreadState" not in html


def test_api_js_no_longer_has_the_role_only_guard():
    api = (FRONTEND / "static" / "api.js").read_text()
    assert "disableManagementPage" not in api and "checkThreadState" not in api


def test_random_bytes_come_from_crypto_not_math_random():
    js = (FRONTEND / "static" / "mgmt.js").read_text()
    assert "crypto.getRandomValues" in js and "Math.random" not in js


def test_guard_retries_and_restores_while_blocked():
    js = (FRONTEND / "static" / "mgmt.js").read_text()
    assert "setTimeout(function() { guard(needsNetwork); }, RETRY_MS)" in js
    assert "function restore()" in js and ".modal-overlay.active" in js


def test_own_dataset_read_ignores_stale_answers_and_clears_on_failure():
    html = (FRONTEND / "network.html").read_text()
    assert "var req = ++ownDatasetReq;" in html
    assert html.count("if (req !== ownDatasetReq) return;") == 2


def test_nav_keeps_aria_expanded_in_step_with_hover_and_focus():
    api = (FRONTEND / "static" / "api.js").read_text()
    for ev in ("mouseenter", "mouseleave", "focusin", "focusout"):
        assert f"'{ev}'" in api
