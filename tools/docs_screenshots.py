#!/usr/bin/env python3
"""Takes the documentation screenshots from the preview server (tools/docs_preview.py) into docs/images/.

Start the preview first (replaying the recording, or with --record against a board), then:

    uv run --with playwright --with pillow tools/docs_screenshots.py [--base http://localhost:8766] [name ...]

The pages see the board as esp-otbr-a1b2c3.local. It uses the system Chromium (/usr/bin/chromium; --chromium for another). Without names it takes every shot below.
"""

import argparse
import json
from pathlib import Path
from urllib.parse import urlparse

from playwright.sync_api import sync_playwright

REPO = Path(__file__).resolve().parents[1]
IMAGES = REPO / "docs" / "images"
WIDTH = 1200
# the address the pages show; every request to it is answered by the preview server
HOST = "http://esp-otbr-a1b2c3.local"


def page_shot(path, height=None, before=None):
    """A full-page screenshot of `path` (or the top `height` pixels), after the optional `before(page)`."""

    def shoot(page, out):
        page.goto(path)
        page.wait_for_load_state("networkidle")
        page.wait_for_timeout(1500)
        if before:
            before(page)
            page.wait_for_timeout(1500)
        if height:
            page.screenshot(
                path=out, clip={"x": 0, "y": 0, "width": WIDTH, "height": height}
            )
        else:
            page.screenshot(path=out, full_page=True)

    return shoot


def element_shot(path, selector, before=None):
    """A screenshot of one element of `path`, after the optional `before(page)`."""

    def shoot(page, out):
        page.goto(path)
        page.wait_for_load_state("networkidle")
        page.wait_for_timeout(1500)
        if before:
            before(page)
            page.wait_for_timeout(800)
        page.locator(selector).first.screenshot(path=out)

    return shoot


def open_dongle(page):
    page.locator("#rows tr", has_text="USB").get_by_role(
        "button", name="Configure"
    ).click()


def pick_thread(page):
    open_dongle(page)
    page.locator("#track").get_by_text("Thread border router", exact=True).first.click()


NAMES = REPO / "tools" / "docs_preview" / "topology-names.json"


def load_topology(page):
    page.locator("#t-load").click()


def load_topology_map(page):
    load_topology(page)
    page.wait_for_timeout(1500)
    page.evaluate(
        "window.scrollTo(0, document.getElementById('t-map-card').getBoundingClientRect().top + window.scrollY - 66)"
    )


def view_shot(path, before=None):
    """The browser viewport at `path`, for the landing page's carousel (HOME_VIEW, saved as WebP)."""

    def shoot(page, out):
        from PIL import Image

        page.goto(path)
        page.wait_for_load_state("networkidle")
        page.wait_for_timeout(1500)
        if before:
            before(page)
            page.wait_for_timeout(1500)
        png = out + ".png"
        page.screenshot(path=png)
        Image.open(png).save(out, quality=82, method=6)
        Path(png).unlink()

    shoot.view = HOME_VIEW
    return shoot


# the landing page's screenshots: 800 x 562 CSS pixels at twice the density, 1600 x 1125 images
HOME_VIEW = {"viewport": {"width": 800, "height": 562}, "device_scale_factor": 2}

SHOTS = {
    "dashboard.png": page_shot("/"),
    "radios.png": page_shot("/radios.html", 560),
    "radios-configure.png": element_shot("/radios.html", "#dlg", open_dongle),
    "radios-move.png": element_shot("/radios.html", "#dlg", pick_thread),
    "routing.png": page_shot("/routing.html"),
    "firmware.png": page_shot("/firmware.html"),
    "firmware-update.png": element_shot("/firmware.html", "#updCard"),
    "network.png": page_shot("/network.html"),
    "capture.png": page_shot("/capture.html"),
    "diagnostics.png": page_shot("/diagnostics.html"),
    "ethernet.png": page_shot("/ethernet.html", 900),
    "ethernet-trel.png": element_shot("/ethernet.html", "#trel"),
    "topology.png": page_shot("/topology.html", 1100, load_topology),
    "home/dashboard.webp": view_shot("/"),
    "home/topology.webp": view_shot("/topology.html", load_topology_map),
    "home/diagnostics.webp": view_shot("/diagnostics.html"),
    "home/ethernet.webp": view_shot("/ethernet.html"),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base", default="http://localhost:8766")
    parser.add_argument("--chromium", default="/usr/bin/chromium")
    parser.add_argument(
        "--out", default=str(IMAGES), help="directory (default: docs/images)"
    )
    parser.add_argument("names", nargs="*", help="shots to take (default: all)")
    args = parser.parse_args()
    unknown = [n for n in args.names if n not in SHOTS]
    if unknown:
        parser.error(f"unknown shots: {', '.join(unknown)}; known: {', '.join(SHOTS)}")
    names = args.names or list(SHOTS)
    with sync_playwright() as p:
        port = urlparse(args.base).port or 80
        rule = f"MAP {HOST[len('http://') :]}:80 127.0.0.1:{port}"
        browser = p.chromium.launch(
            executable_path=args.chromium, args=[f"--host-resolver-rules={rule}"]
        )
        try:
            for name in names:
                view = getattr(
                    SHOTS[name],
                    "view",
                    {
                        "viewport": {"width": WIDTH, "height": 800},
                        "device_scale_factor": 1,
                    },
                )
                context = browser.new_context(base_url=HOST, **view)
                # the sample mesh's device names, as the Topology page keeps them (localStorage)
                context.add_init_script(
                    "localStorage.setItem('topology:names', "
                    + json.dumps(NAMES.read_text())
                    + ")"
                )
                page = context.new_page()
                SHOTS[name](page, str(Path(args.out) / name))
                print(name)
                context.close()
        finally:
            browser.close()


if __name__ == "__main__":
    main()
