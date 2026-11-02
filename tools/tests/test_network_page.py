"""Network page: join an existing Thread network by its active dataset TLVs (PATCHES.md item 15).

Host-only: static checks of network.html and static/dataset_join.js, and the pure logic of dataset_join.js run under
node with a recording fake fetch (skipped when node is not installed). Serving and the round trip against a board are in
p4/pytest/test_network_page.py (deferred to the hardware pass).
"""

import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
FRONTEND = REPO / "p4" / "components" / "esp_ot_br_server" / "frontend"
HTML = FRONTEND / "network.html"
JS = FRONTEND / "static" / "dataset_join.js"

needs_node = pytest.mark.skipif(
    shutil.which("node") is None, reason="node is not installed"
)

# Fake fetch: answers come from the queue `answers` ({status, body} or "error" for a network failure); every call is
# recorded as {method, url, headers, body}.
FAKE_FETCH = """
function fakeFetch(answers) {
  const calls = [];
  const fn = (url, opts) => {
    opts = opts || {};
    calls.push({method: opts.method || 'GET', url: url, headers: opts.headers || {}, body: opts.body === undefined ? null : opts.body});
    const a = answers.shift();
    if (a === undefined || a === 'error') return Promise.reject(new Error('network'));
    const text = a.body === undefined ? '' : (typeof a.body === 'string' ? a.body : JSON.stringify(a.body));
    return Promise.resolve({status: a.status, ok: a.status >= 200 && a.status < 300,
                            text: () => a.textError ? Promise.reject(new Error('reset')) : Promise.resolve(text), json: () => Promise.resolve(JSON.parse(text))});
  };
  fn.calls = calls;
  return fn;
}
"""


def tlv(kind, value):
    return bytes([kind, len(value)]) + value


TLVS = {
    14: tlv(14, bytes.fromhex("0000000000010000")),  # Active Timestamp
    0: tlv(0, bytes.fromhex("00000f")),  # Channel: page 0, channel 15
    53: tlv(53, bytes.fromhex("0004001fffe0")),  # Channel Mask
    2: tlv(2, bytes.fromhex("1111111122222222")),  # Ext PAN ID
    7: tlv(7, bytes.fromhex("fd11002200000000")),  # Mesh-Local Prefix
    5: tlv(5, bytes.fromhex("00112233445566778899aabbccddeeff")),  # Network Key
    3: tlv(3, b"MyThread"),  # Network Name
    1: tlv(1, bytes.fromhex("1234")),  # PAN ID
    4: tlv(4, bytes.fromhex("aabbccddeeff00112233445566778899")),  # PSKc
    12: tlv(12, bytes.fromhex("02a0f7f8")),  # Security Policy
}


def dataset(without=(), extra=b""):
    return (b"".join(v for k, v in TLVS.items() if k not in without) + extra).hex()


COMPLETE = dataset()


def logic(expression):
    """Evaluate a JS expression against dataset_join.js (exported as L); return its JSON value."""
    script = f"const L = require({json.dumps(str(JS))}); process.stdout.write(JSON.stringify({expression}));"
    out = subprocess.run(
        ["node", "-e", script], capture_output=True, text=True, check=True, timeout=20
    )
    return json.loads(out.stdout) if out.stdout else None


def scenario(body):
    """Run `body` (async JS with L = dataset_join.js and fakeFetch) under node; return its JSON result."""
    script = (
        f"const L = require({json.dumps(str(JS))});{FAKE_FETCH}"
        f"(async () => {{ const r = await (async () => {{ {body} }})(); process.stdout.write(JSON.stringify(r)); }})()"
        ".catch(e => { console.error(e); process.exit(1); });"
    )
    out = subprocess.run(
        ["node", "-e", script], capture_output=True, text=True, check=True, timeout=60
    )
    return json.loads(out.stdout) if out.stdout else None


def parse(text):
    return logic(f"L.parseDatasetTlvs({json.dumps(text)})")


class TestPageFiles:
    """network.html and static/dataset_join.js"""

    def test_page_loads_the_dataset_join_script_after_api_js(self):
        html = HTML.read_text()
        assert '<script src="/static/dataset_join.js"></script>' in html
        assert html.index("/static/api.js") < html.index("/static/dataset_join.js")

    @pytest.mark.parametrize(
        "element_id",
        ["datasetTlvs", "datasetPreview", "datasetJoinBtn", "datasetStatus"],
    )
    def test_page_has_the_join_card_elements(self, element_id):
        assert f'id="{element_id}"' in HTML.read_text()

    def test_join_card_sits_between_scan_and_form(self):
        html = HTML.read_text()
        assert (
            html.index("Discover Thread Networks")
            < html.index('id="datasetTlvs"')
            < html.index("Form New Network")
        )

    def test_script_never_inserts_text_as_markup(self):
        source = JS.read_text()
        assert not re.search(
            r"innerHTML|outerHTML|insertAdjacentHTML|document\.write", source
        )
        assert "textContent" in source

    def test_script_page_code_is_guarded_for_node(self):
        source = JS.read_text()
        assert "typeof document !== 'undefined'" in source
        assert "module.exports" in source


@needs_node
class TestParseDatasetTlvs:
    """parseDatasetTlvs"""

    def test_complete_dataset_returns_ok_with_compact_lower_case_hex(self):
        result = parse(COMPLETE.upper())
        assert result["ok"] is True
        assert result["hex"] == COMPLETE

    def test_complete_dataset_returns_the_network_fields(self):
        assert parse(COMPLETE)["network"] == {
            "name": "MyThread",
            "channel": 15,
            "panId": "0x1234",
            "extPanId": "1111111122222222",
        }

    def test_upper_case_hex_over_several_lines_gives_the_compact_hex(self):
        text = "\n".join(
            COMPLETE.upper()[i : i + 32] for i in range(0, len(COMPLETE), 32)
        )
        assert parse("  " + text.replace("\n", " \r\n\t") + "\n")["hex"] == COMPLETE

    @pytest.mark.parametrize("text", ["", "   \n "])
    def test_empty_input_is_refused(self, text):
        assert parse(text) == {"ok": False, "error": "Paste the dataset TLVs (hex)"}

    @pytest.mark.parametrize(
        "text", [COMPLETE + "0", COMPLETE[:-2] + "zz", "0x" + COMPLETE]
    )
    def test_non_hex_or_odd_length_is_refused(self, text):
        assert parse(text) == {"ok": False, "error": "Not a hex string"}

    def test_more_than_254_bytes_is_refused(self):
        too_long = dataset(extra=tlv(0x80, bytes(160)))
        assert len(too_long) // 2 > 254
        assert parse(too_long) == {
            "ok": False,
            "error": "Dataset too long (max 254 bytes)",
        }

    def test_tlv_running_past_the_end_is_refused(self):
        assert parse(COMPLETE + "0310aabb") == {"ok": False, "error": "Malformed TLVs"}

    @pytest.mark.parametrize(
        "kind,name",
        [
            (14, "Active Timestamp"),
            (0, "Channel"),
            (1, "PAN ID"),
            (2, "Ext PAN ID"),
            (3, "Network Name"),
            (5, "Network Key"),
            (7, "Mesh-Local Prefix"),
            (4, "PSKc"),
            (12, "Security Policy"),
            (53, "Channel Mask"),
        ],
    )
    def test_missing_required_tlv_is_named(self, kind, name):
        assert parse(dataset(without=[kind])) == {
            "ok": False,
            "error": f"Not a complete active dataset: missing {name}",
        }

    def test_every_missing_tlv_is_named(self):
        assert parse(dataset(without=[3, 5])) == {
            "ok": False,
            "error": "Not a complete active dataset: missing Network Key, Network Name",
        }

    @pytest.mark.parametrize(
        "kind,value,name",
        [
            (0, "000f", "Channel"),
            (5, "", "Network Key"),
            (2, "11111111222222", "Ext PAN ID"),
            (3, "", "Network Name"),
            (1, "12", "PAN ID"),
        ],
    )
    def test_wrong_tlv_length_is_named(self, kind, value, name):
        text = dataset(without=[kind], extra=tlv(kind, bytes.fromhex(value)))
        assert parse(text) == {
            "ok": False,
            "error": f"Malformed TLVs: wrong length of {name}",
        }

    def test_unknown_tlvs_are_kept(self):
        text = dataset(extra=tlv(0x7E, b"\x01\x02"))
        result = parse(text)
        assert result["ok"] is True and result["hex"] == text


@needs_node
class TestDescribeNetwork:
    """describeNetwork"""

    def test_names_network_channel_and_pan(self):
        network = {
            "name": "MyThread",
            "channel": 15,
            "panId": "0x1234",
            "extPanId": "1111111122222222",
        }
        assert (
            logic(f"L.describeNetwork({json.dumps(network)})")
            == "MyThread (channel 15, PAN 0x1234)"
        )


@needs_node
class TestReadCurrentNetwork:
    """readCurrentNetwork"""

    def test_stored_dataset_resolves_to_its_network(self):
        result = scenario(
            "const f = fakeFetch([{status: 200, body: {networkName: 'Home', channel: 20, panId: 4660}}]);"
            "const r = await L.readCurrentNetwork(f); return {r, calls: f.calls};"
        )
        assert result["r"] == {"name": "Home", "channel": 20, "panId": "0x1234"}
        assert result["calls"] == [
            {
                "method": "GET",
                "url": "/node/dataset/active",
                "headers": {"Accept": "application/json"},
                "body": None,
            }
        ]

    @pytest.mark.parametrize("answer", ["{status: 204}", "{status: 503}", "'error'"])
    def test_no_dataset_or_failure_resolves_to_null(self, answer):
        assert (
            scenario(f"return await L.readCurrentNetwork(fakeFetch([{answer}]));")
            is None
        )


@needs_node
class TestJoinWithDataset:
    """joinWithDataset"""

    def run(self, answers):
        return scenario(
            f"const f = fakeFetch([{answers}]);"
            f"const r = await L.joinWithDataset({json.dumps(COMPLETE)}, f); return {{r, calls: f.calls}};"
        )

    def test_running_thread_puts_the_dataset_and_does_not_enable(self):
        result = self.run("{status: 200}, {status: 200, body: 'leader'}")
        assert result["r"] == {"ok": True, "started": False}
        assert result["calls"] == [
            {
                "method": "PUT",
                "url": "/node/dataset/active",
                "headers": {"Content-Type": "text/plain"},
                "body": COMPLETE,
            },
            {"method": "GET", "url": "/node/state", "headers": {}, "body": None},
        ]

    def test_disabled_thread_is_enabled_after_the_dataset(self):
        result = self.run(
            "{status: 201}, {status: 200, body: 'disabled'}, {status: 200}"
        )
        assert result["r"] == {"ok": True, "started": True}
        assert result["calls"][2] == {
            "method": "PUT",
            "url": "/node/state",
            "headers": {"Content-Type": "application/json"},
            "body": '"enable"',
        }

    @pytest.mark.parametrize("status", [400, 403, 409, 500, 503])
    def test_failed_dataset_put_stops_there(self, status):
        result = self.run(f"{{status: {status}}}")
        assert result["r"] == {"ok": False, "step": "dataset", "status": status}
        assert len(result["calls"]) == 1

    def test_network_error_on_the_dataset_put_reports_status_0(self):
        result = self.run("'error'")
        assert result["r"] == {"ok": False, "step": "dataset", "status": 0}
        assert len(result["calls"]) == 1

    def test_failed_state_read_reports_step_state(self):
        assert self.run("{status: 200}, {status: 503}")["r"] == {
            "ok": False,
            "step": "state",
            "status": 503,
        }

    def test_body_read_failure_reports_status_0_at_its_step(self):
        assert self.run("{status: 200}, {status: 200, textError: true}")["r"] == {
            "ok": False,
            "step": "state",
            "status": 0,
        }

    def test_failed_enable_reports_step_enable(self):
        assert self.run(
            "{status: 200}, {status: 200, body: 'disabled'}, {status: 500}"
        )["r"] == {"ok": False, "step": "enable", "status": 500}


@needs_node
class TestJoinErrorMessage:
    """joinErrorMessage"""

    def message(self, step, status):
        return logic(f"L.joinErrorMessage({json.dumps(step)}, {status})")

    def test_403_points_to_the_thread_backbone_address(self):
        text = self.message("dataset", 403)
        assert "management address" in text and "Thread backbone address" in text

    def test_409_says_busy(self):
        assert "busy" in self.message("dataset", 409)

    def test_503_says_radio_not_available(self):
        text = self.message("dataset", 503)
        assert "radio is not available" in text and "Device page" in text

    def test_400_on_dataset_says_rejected(self):
        assert "rejected the dataset" in self.message("dataset", 400)

    def test_500_on_dataset_says_it_could_not_apply(self):
        text = self.message("dataset", 500)
        assert "could not apply the dataset" in text and "reboot" in text

    @pytest.mark.parametrize("status", [0, 409, 500])
    def test_enable_step_says_dataset_stored(self, status):
        text = self.message("enable", status)
        assert "dataset is stored" in text and "could not start" in text

    def test_status_0_says_connection_lost(self):
        assert "Connection lost" in self.message("state", 0)

    def test_other_status_names_step_and_status(self):
        text = self.message("state", 418)
        assert "state" in text and "418" in text
