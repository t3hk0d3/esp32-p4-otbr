"""Device pages as a whole (BDD: api.js nav, Pages). Static checks of the five pages and their scripts."""

import json
import re
import subprocess

import pytest

from device_js import FRONTEND, needs_node

API_JS = FRONTEND / "static" / "api.js"

DEVICE_MENU = [
    ["/device.html", "Status"],
    ["/ethernet.html", "Ethernet"],
    ["/firmware.html", "Firmware"],
    ["/capture.html", "Packet capture"],
    ["/diagnostics.html", "Diagnostics"],
]


def nav_html(pathname):
    """Run api.js under node with a fake nav element at `pathname`; return the nav markup it builds."""
    source = (
        "const vm = require('vm'); const fs = require('fs');"
        "const nav = {innerHTML: ''};"
        f"const ctx = {{document: {{getElementById: (id) => id === 'nav' ? nav : null}}, location: {{pathname: {json.dumps(pathname)}}}}};"
        f"vm.runInNewContext(fs.readFileSync({json.dumps(str(API_JS))}, 'utf8'), ctx);"
        "process.stdout.write(JSON.stringify(nav.innerHTML));"
    )
    out = subprocess.run(["node", "-e", source], capture_output=True, text=True, check=True, timeout=20)
    return json.loads(out.stdout)


@needs_node
class TestNav:
    """api.js nav"""

    def test_device_is_a_dropdown_with_the_five_pages_in_order(self):
        # On any page: the Device toggle is followed by its five entries in order.
        html = nav_html("/index.html")
        menu = html.split('nav-dropdown-toggle">Device</a>', 1)[1].split("</ul>", 1)[0]
        assert [list(m) for m in re.findall(r'<a href="([^"]+)"[^>]*>([^<]+)</a>', menu)] == DEVICE_MENU

    @pytest.mark.parametrize("path", [p for p, _ in DEVICE_MENU])
    def test_device_toggle_is_active_on_each_device_page(self, path):
        # On each of the five paths: the Device toggle and that entry are marked active.
        html = nav_html(path)
        assert 'nav-dropdown-toggle active">Device</a>' in html
        assert f'<a href="{path}" class="active">' in html

    def test_device_toggle_is_not_active_elsewhere(self):
        # On the dashboard: the Device toggle is not active.
        assert 'nav-dropdown-toggle">Device</a>' in nav_html("/index.html")
