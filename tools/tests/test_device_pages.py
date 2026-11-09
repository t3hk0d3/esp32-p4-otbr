"""Device pages as a whole (BDD: api.js nav, Pages). Static checks of the six pages and their scripts."""

import json
import re
import subprocess
from html.parser import HTMLParser

import pytest
from device_js import FRONTEND, REPO, STATIC, needs_node

API_JS = FRONTEND / "static" / "api.js"
DEVICE_API = REPO / "p4" / "components" / "device_api" / "device_api.c"
WEB_API = (
    FRONTEND.parent / "private_include" / "esp_br_web_api.h"
)  # Thread web GUI routes (/get_properties, ...)

DEVICE_MENU = [
    ["/radios.html", "Radios"],
    ["/ethernet.html", "Ethernet"],
    ["/firmware.html", "Firmware"],
    ["/diagnostics.html", "Diagnostics"],
    ["/capture.html", "Packet capture"],
]
THREAD_MENU = [
    ["/network.html", "Network"],
    ["/topology.html", "Topology"],
    ["/routing.html", "Routing"],
    ["/commission.html", "Commissioner"],
    ["/addresses.html", "Addresses"],
    ["/tools.html", "Tools"],
    ["/console.html", "Console"],
]

# page -> (script, endpoints its script names)
# Scripts of sections that share a page, loaded after the page's own script.
EXTRA_SCRIPTS = {"ethernet": ["names"]}
# Scripts of sections a page mounts, loaded before the page's own script (which starts the page as it loads).
SECTION_SCRIPTS = {"index": ["zigbee"]}


def scripts_of(name):
    """The page's own script, its section scripts and its extra scripts."""
    return (
        [PAGES[name][0]] + SECTION_SCRIPTS.get(name, []) + EXTRA_SCRIPTS.get(name, [])
    )


PAGES = {
    "radios": ("radios", ["/device/channels", "/device/status"]),
    "index": (
        "status",
        [
            "/device/status",
            "/device/diag",
            "/device/reboot",
            "/device/channels",
            "/device/zigbee",
            "/get_properties",
            "/ipaddr",
        ],
    ),
    "diagnostics": ("diagnostics", ["/device/diag"]),
    # /ping is the Thread web GUI's, registered in esp_br_web_api.h like /get_properties above.
    "topology": ("topology", ["/device/topology", "/ping"]),
    "ethernet": ("ethernet", ["/device/network", "/device/names"]),
    "routing": ("routing", ["/device/routing", "/device/status"]),
    "firmware": (
        "firmware",
        [
            "/device/status",
            "/device/ota",
            "/device/ota/url",
            "/device/radio/firmware",
            "/device/radio/recover",
            "/device/radio/log",
        ],
    ),
    "capture": (
        "capture",
        ["/device/sniffer.pcap", "/device/sniffer", "/device/status"],
    ),
}

KIT_IDS = ["trialBanner", "trialRemaining", "trialFill", "confirmBtn", "confirmMsg"]
KIT_ENDPOINTS = ["/device/network", "/device/network/confirm", "/device/diag"]
LIVE_ID = re.compile(r"Msg$|-note$|-offline$|-stale$")


class _Ancestors(HTMLParser):
    VOID = frozenset(
        {
            "area",
            "base",
            "br",
            "col",
            "embed",
            "hr",
            "img",
            "input",
            "link",
            "meta",
            "source",
            "track",
            "wbr",
        }
    )

    def __init__(self, target):
        super().__init__()
        self.target, self.stack, self.found = target, [], None

    def handle_starttag(self, tag, attrs):
        ident = dict(attrs).get("id")
        if ident == self.target and self.found is None:
            self.found = [i for _, i in self.stack if i]
        if tag not in self.VOID:
            self.stack.append((tag, ident))

    def handle_endtag(self, tag):
        while self.stack:
            if self.stack.pop()[0] == tag:
                break


def ancestor_ids(html, ident):
    """The ids of the elements that enclose #ident, outermost first."""
    parser = _Ancestors(ident)
    parser.feed(html)
    assert parser.found is not None, f"#{ident} not found"
    return parser.found


def html_of(name):
    return (FRONTEND / f"{name}.html").read_text()


def js_of(script):
    return (STATIC / f"{script}.js").read_text()


def nav_html(pathname):
    """Run api.js under node with a fake nav element at `pathname`; return the nav markup it builds."""
    source = (
        "const vm = require('vm'); const fs = require('fs');"
        "const nav = {innerHTML: ''};"
        f"const ctx = {{document: {{getElementById: (id) => id === 'nav' ? nav : null}}, location: {{pathname: {json.dumps(pathname)}}}}};"
        f"vm.runInNewContext(fs.readFileSync({json.dumps(str(API_JS))}, 'utf8'), ctx);"
        "process.stdout.write(JSON.stringify(nav.innerHTML));"
    )
    out = subprocess.run(
        ["node", "-e", source], capture_output=True, text=True, check=True, timeout=20
    )
    return json.loads(out.stdout)


def nav_run(pathname, status=None, fetch_fails=False, cached=None):
    """api.js with a fake fetch answering /device/status with `status`; return [markup, #nav-thread hidden after the
    answer, the cache written]."""
    source = (
        "const vm = require('vm'); const fs = require('fs');"
        "const nav = {innerHTML: ''}; const thread = {hidden: false}; const store = {};"
        f"if ({json.dumps(cached)} !== null) store.navThreadHidden = {json.dumps(cached)};"
        "const ctx = {document: {getElementById: (id) => id === 'nav' ? nav : id === 'nav-thread' ? thread : null},"
        f" location: {{pathname: {json.dumps(pathname)}}},"
        " sessionStorage: {getItem: (k) => k in store ? store[k] : null, setItem: (k, v) => { store[k] = String(v); }},"
        f" fetch: () => {json.dumps(fetch_fails)} ? Promise.reject(new Error('x')) :"
        f" Promise.resolve({{ok: true, text: () => Promise.resolve({json.dumps(json.dumps(status))})}})}};"
        f"vm.runInNewContext(fs.readFileSync({json.dumps(str(API_JS))}, 'utf8'), ctx);"
        "setTimeout(() => process.stdout.write(JSON.stringify([nav.innerHTML, thread.hidden, store.navThreadHidden || null])), 50);"
    )
    out = subprocess.run(
        ["node", "-e", source], capture_output=True, text=True, check=True, timeout=20
    )
    return json.loads(out.stdout)


@needs_node
class TestNav:
    """api.js nav"""

    def test_top_bar_order_and_the_zigbee_slot(self):
        # On any page: Dashboard, Thread, the empty hidden Zigbee slot, Device, About, in that order.
        html = nav_html("/index.html")
        top = [
            m
            for m in re.findall(
                r'nav-dropdown-toggle[^>]*>(\w+)</a>|<li id="(nav-slot-zigbee)" hidden></li>|<a href="/(index|about)\.html"',
                html,
            )
        ]
        names = ["".join(t) for t in top]
        assert names == ["index", "Thread", "nav-slot-zigbee", "Device", "about"]
        assert '<li id="nav-slot-zigbee" hidden></li>' in html

    def test_thread_is_a_dropdown_with_its_pages_in_order(self):
        # On any page: the Thread toggle is followed by Network, Topology, Routing, Commissioner, Addresses, Tools, Console.
        html = nav_html("/index.html")
        menu = html.split('nav-dropdown-toggle">Thread</a>', 1)[1].split("</ul>", 1)[0]
        assert [
            list(m) for m in re.findall(r'<a href="([^"]+)"[^>]*>([^<]+)</a>', menu)
        ] == THREAD_MENU

    @pytest.mark.parametrize("path", [p for p, _ in THREAD_MENU])
    def test_thread_toggle_is_active_on_each_thread_page(self, path):
        # On each Thread page path: the Thread toggle and that entry are active, the Device toggle is not.
        html = nav_html(path)
        assert 'nav-dropdown-toggle active">Thread</a>' in html
        assert f'<a href="{path}" class="active">' in html
        assert 'nav-dropdown-toggle">Device</a>' in html

    def test_there_is_no_management_menu(self):
        # On any page: no "Management" label.
        assert "Management" not in nav_html("/index.html")

    def test_thread_menu_is_hidden_when_no_channel_runs_otbr(self):
        # On a status with otbr_channel null: #nav-thread is hidden and the choice is remembered.
        html, hidden, cached = nav_run("/index.html", {"otbr_channel": None})
        assert 'id="nav-thread"' in html
        assert hidden is True
        assert cached == "1"

    def test_thread_menu_shows_when_a_channel_runs_otbr(self):
        # On a status with otbr_channel c6 after a hidden visit: shown again, cache cleared.
        _html, hidden, cached = nav_run(
            "/index.html",
            {"otbr_channel": "builtin-c6", "mode": "NO_RADIO"},
            cached="1",
        )
        assert hidden is False
        assert cached == "0"

    def test_thread_menu_stays_for_an_unavailable_otbr_radio(self):
        # On NO_RADIO with a channel in otbr (unplugged, faulted): the menu stays.
        assert (
            nav_run("/index.html", {"otbr_channel": "u1", "mode": "NO_RADIO"})[1]
            is False
        )

    def test_thread_menu_is_left_alone_without_an_answer(self):
        # On a failed request and on an older board without otbr_channel: the menu keeps its state.
        assert nav_run("/index.html", fetch_fails=True)[1] is False
        assert nav_run("/index.html", {"mode": "BR_RUNNING"})[1] is False

    def test_remembered_hidden_state_is_in_the_first_markup(self):
        # On a page after a visit that hid the menu: the Thread item is hidden from the first paint.
        html = nav_run("/index.html", fetch_fails=True, cached="1")[0]
        assert '<li class="nav-dropdown" id="nav-thread" hidden>' in html

    def test_device_is_a_dropdown_with_its_pages_in_order(self):
        # On any page: the Device toggle is followed by its entries in order (the status is the Dashboard).
        html = nav_html("/index.html")
        menu = html.split('nav-dropdown-toggle">Device</a>', 1)[1].split("</ul>", 1)[0]
        assert [
            list(m) for m in re.findall(r'<a href="([^"]+)"[^>]*>([^<]+)</a>', menu)
        ] == DEVICE_MENU

    @pytest.mark.parametrize("path", ["/", "/index.html"])
    def test_dashboard_is_active_on_the_landing_page(self, path):
        # On / and /index.html: the Dashboard entry is active, the Device toggle is not.
        html = nav_html(path)
        assert '<a href="/index.html" class="active">Dashboard</a>' in html
        assert 'nav-dropdown-toggle">Device</a>' in html

    def test_there_is_no_separate_status_page(self):
        # On the frontend: no device.html (the Dashboard is the status page), and no nav entry for it.
        assert not (FRONTEND / "device.html").exists()
        assert "/device.html" not in API_JS.read_text()

    @pytest.mark.parametrize("path", [p for p, _ in DEVICE_MENU])
    def test_device_toggle_is_active_on_each_device_page(self, path):
        # On each device page path: the Device toggle and that entry are marked active.
        html = nav_html(path)
        assert 'nav-dropdown-toggle active">Device</a>' in html
        assert f'<a href="{path}" class="active">' in html

    def test_device_toggle_is_not_active_elsewhere(self):
        # On the dashboard: the Device toggle is not active.
        assert 'nav-dropdown-toggle">Device</a>' in nav_html("/index.html")


@pytest.mark.parametrize("name", sorted(PAGES))
class TestPages:
    """every device page and its script"""

    def test_page_loads_style_nav_api_common_and_its_script_in_order(self, name):
        # On the page: shared stylesheet, nav placeholder, then api.js, device_common.js, its own script and the
        # scripts of its extra sections (the Ethernet page's names section).
        html = html_of(name)
        script = PAGES[name][0]
        assert '<link rel="stylesheet" href="/static/style.css">' in html
        assert '<nav class="nav" id="nav"></nav>' in html
        assert '<body class="device-page">' in html
        tags = re.findall(r'<script src="([^"]+)"></script>', html)
        assert tags == (
            ["/static/api.js", "/static/device_common.js"]
            + [f"/static/{section}.js" for section in SECTION_SCRIPTS.get(name, [])]
            + [f"/static/{script}.js"]
            + [f"/static/{extra}.js" for extra in EXTRA_SCRIPTS.get(name, [])]
        )

    def test_page_has_the_trial_banner_and_the_source_link(self, name):
        # On the page: the shared banner ids and the AGPL source link in the footer.
        html = html_of(name)
        for ident in KIT_IDS:
            assert f'id="{ident}"' in html, ident
        assert 'href="https://github.com/t3hk0d3/esp32-p4-otbr">Source code</a>' in html

    def test_trial_bar_sits_inside_the_banner(self, name):
        # On the page: #trialFill is inside #trialBanner, so hiding the banner hides the bar too.
        assert ancestor_ids(html_of(name), "trialFill").count("trialBanner") == 1

    def test_script_never_inserts_markup(self, name):
        # On the script: no innerHTML/outerHTML/insertAdjacentHTML/document.write.
        for script in scripts_of(name):
            assert not re.search(
                r"innerHTML|outerHTML|insertAdjacentHTML|document\.write",
                js_of(script),
            ), script

    def test_page_and_script_load_nothing_remote(self, name):
        # On both: no script/link/src/href to a remote host, no import (a plain <a> link is navigation).
        for text in (html_of(name), js_of(PAGES[name][0])):
            text = re.sub(r"<a\s[^>]*>", "", text)
            assert not re.search(r"(?:src|href)=[\"']?(?:https?:)?//", text)
            assert "import " not in text

    def test_every_id_the_script_names_exists_in_the_page(self, name):
        # On the script: each id given to $, setText, show or note is an element of the page.
        # A whole quoted id only: an id built from a prefix ('len-' + s) is checked by the page's own tests.
        used = set(
            re.findall(
                r"(?:\$|setText|show|note)\('([A-Za-z0-9_-]+)'\s*[,)]",
                "".join(
                    js_of(s) for s in [PAGES[name][0]] + SECTION_SCRIPTS.get(name, [])
                ),
            )
        )
        present = set(re.findall(r'id="([^"]+)"', html_of(name)))
        assert used
        assert used <= present, used - present

    def test_message_areas_are_live_regions(self, name):
        # On the page: every message, note, offline and stale area carries aria-live.
        html = html_of(name)
        ids = [i for i in re.findall(r'id="([^"]+)"', html) if LIVE_ID.search(i)]
        assert ids
        for ident in ids:
            assert re.search(rf'id="{ident}"[^>]*aria-live="polite"', html), ident

    def test_each_endpoint_is_named_by_the_script_and_registered(self, name):
        # On the endpoints of the page: quoted in its script and registered (device_api, or the Thread web GUI).
        scripts = scripts_of(name)
        source, routes = "".join(js_of(s) for s in scripts), DEVICE_API.read_text()
        for endpoint in PAGES[name][1]:
            assert re.search(rf"['\"]{re.escape(endpoint)}['\"?]", source), endpoint
            assert (
                f'"{endpoint}"' in routes or f'"{endpoint}"' in WEB_API.read_text()
            ), endpoint


class TestShared:
    """device_common.js and the frontend directory"""

    def test_every_html_page_that_builds_the_nav_gets_it_from_api_js(self):
        # On every page of the frontend: the nav is built by api.js only, so no page needs its own link.
        for page in FRONTEND.glob("*.html"):
            text = page.read_text()
            if '<nav class="nav" id="nav">' in text:
                assert "/static/api.js" in text, page.name

    @pytest.mark.parametrize("name", sorted(PAGES))
    def test_countdown_banner_is_not_a_live_region(self, name):
        # On each page: the trial banner (its countdown ticks every second) carries no role or aria-live.
        tag = re.search(r'<div[^>]*id="trialBanner"[^>]*>', html_of(name)).group(0)
        assert "role=" not in tag and "aria-live" not in tag

    def test_common_names_the_endpoints_it_uses_and_they_are_registered(self):
        # On device_common.js: the trial and reboot-wait endpoints are quoted and registered.
        source, routes = js_of("device_common"), DEVICE_API.read_text()
        for endpoint in KIT_ENDPOINTS:
            assert f"'{endpoint}'" in source, endpoint
            assert f'"{endpoint}"' in routes, endpoint

    def test_common_writes_text_only(self):
        # On device_common.js: text goes in through textContent, never as markup.
        source = js_of("device_common")
        assert "textContent" in source
        assert not re.search(
            r"innerHTML|outerHTML|insertAdjacentHTML|document\.write", source
        )


TOP_LEVEL_ROUTES = {
    "/metrics"
}  # device_api routes outside /device/ (Prometheus scrapes /metrics by default)


class TestRouting:
    """device_api URI table against the static file handler"""

    def test_registered_device_routes_are_exact_paths_under_device_slash(self):
        # On the route table: every URI is exact (no wildcard) and under /device/ or one of the allowed top-level
        # paths, so the page files (/index.html, /ethernet.html, ...) reach the /* file handler.
        uris = re.findall(r'\{"(/[^"]*)", HTTP_[A-Z]+, ', DEVICE_API.read_text())
        assert uris
        assert all("*" not in u for u in uris)
        assert all(u.startswith("/device/") or u in TOP_LEVEL_ROUTES for u in uris)

    @pytest.mark.parametrize("name", sorted(PAGES))
    def test_no_page_file_is_shadowed_by_a_route(self, name):
        # On each page path: no device_api route takes it.
        assert f'"/{name}.html"' not in DEVICE_API.read_text()
