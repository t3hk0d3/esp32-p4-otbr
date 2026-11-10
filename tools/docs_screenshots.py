#!/usr/bin/env python3
"""Takes the documentation screenshots from the preview server (tools/docs_preview.py) into docs/images/.

Start the preview first (replaying the recording, or with --record against a board), then:

    uv run --with playwright tools/docs_screenshots.py [--base http://localhost:8766] [name ...]

The pages see the board as esp-otbr-a1b2c3.local. It uses the system Chromium (/usr/bin/chromium; --chromium for another). Without names it takes every shot below.
"""

import argparse
from pathlib import Path
from urllib.parse import urlparse

from playwright.sync_api import sync_playwright

REPO = Path(__file__).resolve().parents[1]
IMAGES = REPO / "docs" / "images"
WIDTH = 1200
# the address the pages show; every request to it is answered by the preview server
HOST = "http://esp-otbr-a1b2c3.local"


def page_shot(path, height=None):
    """A full-page screenshot of `path` (or the top `height` pixels)."""

    def shoot(page, out):
        page.goto(path)
        page.wait_for_load_state("networkidle")
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
                context = browser.new_context(
                    base_url=HOST,
                    viewport={"width": WIDTH, "height": 800},
                    device_scale_factor=1,
                )
                page = context.new_page()
                SHOTS[name](page, str(Path(args.out) / name))
                print(name)
                context.close()
        finally:
            browser.close()


if __name__ == "__main__":
    main()
