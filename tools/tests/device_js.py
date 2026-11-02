"""Helpers for the device page tests: run the page scripts under node against device_pages_harness.js."""

import json
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
FRONTEND = REPO / "p4" / "components" / "esp_ot_br_server" / "frontend"
STATIC = FRONTEND / "static"
HARNESS = Path(__file__).resolve().parent / "device_pages_harness.js"

needs_node = pytest.mark.skipif(
    shutil.which("node") is None, reason="node is not installed"
)


def logic(script, expression):
    """Evaluate a JS expression with L = the exports of static/<script>.js; return its JSON value."""
    source = (
        f"const L = require({json.dumps(str(STATIC / (script + '.js')))});"
        f"process.stdout.write(JSON.stringify({expression}));"
    )
    out = subprocess.run(
        ["node", "-e", source], capture_output=True, text=True, check=True, timeout=20
    )
    return json.loads(out.stdout) if out.stdout else None


def scenario(body):
    """Run `body` (async JS with H = the harness) under node; return its JSON result."""
    source = (
        f"const H = require({json.dumps(str(HARNESS))});"
        f"(async () => {{ const r = await (async () => {{ {body} }})(); process.stdout.write(JSON.stringify(r)); }})()"
        ".catch(e => { console.error(e); process.exit(1); });"
    )
    out = subprocess.run(
        ["node", "-e", source], capture_output=True, text=True, check=True, timeout=60
    )
    return json.loads(out.stdout) if out.stdout else None


def page(name, routes_js="() => ({status: 200, body: {}})"):
    """Start of a scenario: `t` is the harness env of page `name` with the given fake routes."""
    return f"const t = H.makeEnv({json.dumps(name)}, {routes_js});"


KIT_HTML = (
    '<div id="trialBanner" class="hidden"><span id="trialRemaining"></span>'
    '<button id="confirmBtn"></button><div id="confirmMsg"></div></div>'
    '<div id="sec"></div><button id="actA"></button><button id="actB"></button><div id="msg"></div>'
)


def kit(
    routes_js="() => ({status: 200, body: {}})", options="{actionIds: ['actA', 'actB']}"
):
    """Start of a scenario: `t` runs DeviceKit alone on a small page with the given options."""
    return f"const t = H.makeEnv({{html: {json.dumps(KIT_HTML)}, kit: {options}}}, {routes_js});"
