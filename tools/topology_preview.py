#!/usr/bin/env python3
"""Dev preview of the Topology page without a board.

Serves the frontend directory on localhost and answers the page's requests with canned data:
GET and POST /device/topology a "done" state with the result of the test fixture (tools/tests/test_topology_page.py,
evaluated with node: it has another border router and a child that didn't report its addresses), POST /ping a ping
result, GET /device/status BR_RUNNING and GET /device/network no trial.
With --running N the scan answers `running`, with growing progress, to the POST and the next N GETs, then `done`;
the last GETs show the child address step. Every POST starts it over.

    python3 tools/topology_preview.py --port 8765                # then open http://localhost:8765/topology.html
    python3 tools/topology_preview.py --port 8765 --running 8    # to see the loading view
"""

import argparse
import json
import re
import subprocess
import threading
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
FRONTEND = REPO / "p4" / "components" / "esp_ot_br_server" / "frontend"
TOPOLOGY_JS = FRONTEND / "static" / "topology.js"
FIXTURE_FILE = REPO / "tools" / "tests" / "test_topology_page.py"

PING = {
    "error": 0,
    "message": "OK",
    "result": {
        "replies": [
            {"seq": i, "size": 32, "hlim": 64, "rtt": 20 + i} for i in range(3)
        ],
        "statistics": {
            "sent": 4,
            "received": 3,
            "loss_percent": 25.0,
            "rtt_min": 20,
            "rtt_avg": 21,
            "rtt_max": 22,
        },
    },
}


def fixture_result():
    """The fixture's result object R, built by node from the JS literal in the test file."""
    fixture = re.search(
        r'FIXTURE = r"""(.*?)"""', FIXTURE_FILE.read_text(), re.DOTALL
    ).group(1)
    source = f"const L = require({json.dumps(str(TOPOLOGY_JS))});{fixture}process.stdout.write(JSON.stringify(R));"
    out = subprocess.run(
        ["node", "-e", source], capture_output=True, text=True, check=True, timeout=20
    )
    return json.loads(out.stdout)


def asked_children(result):
    """The children the scan asks for their addresses: rx-on ones with no addresses."""
    return [
        c
        for r in result["routers"]
        for c in r.get("children", [])
        if c.get("mode", {}).get("rx_on_when_idle") and not c.get("addresses")
    ]


class Scan:
    """The canned scan: `running` for the POST and the next `steps` GETs (routers found after the first quarter,
    then queried one by one, then the rx-on children with no addresses asked in the last quarter), then `done`."""

    def __init__(self, steps, result):
        self.steps = steps
        self.left = steps
        self.result = result
        self.lock = threading.Lock()

    def state(self, restart):
        with self.lock:
            if restart:
                self.left = self.steps
            if self.left <= 0:
                return {"state": "done", "age_s": 5, "result": self.result}
            step = self.steps - self.left
            if not restart:
                self.left -= 1
        routers = len(self.result["routers"])
        found = self.steps // 4
        asking = self.steps - max(1, self.steps // 4)
        kids = max(1, len(asked_children(self.result)))
        children = {"total": 0, "queried": 0}
        if step < found:
            queried = 0
        elif step < asking:
            queried = min(routers, (step - found) * routers // max(1, asking - found))
        else:
            queried = routers
            children = {
                "total": kids,
                "queried": (step - asking) * kids // max(1, self.steps - asking),
            }
        progress = {
            "routers": 0 if step < found else routers,
            "queried": queried,
            "children": children,
        }
        return {"state": "running", "progress": progress}


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, answers, scan, **kwargs):
        self.answers = answers
        self.scan = scan
        super().__init__(*args, directory=str(FRONTEND), **kwargs)

    def reply(self, status, body):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/device/topology":
            self.reply(200, self.scan.state(restart=False))
        elif ("GET", path) in self.answers:
            self.reply(*self.answers[("GET", path)])
        else:
            super().do_GET()

    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length") or 0))
        path = self.path.split("?", 1)[0]
        if path == "/device/topology":
            self.reply(202, self.scan.state(restart=True))
            return
        self.reply(*self.answers.get(("POST", path), (404, {"error": "not_found"})))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument(
        "--running",
        type=int,
        default=0,
        metavar="N",
        help="answer `running` to the POST and the next N GETs before `done`",
    )
    args = parser.parse_args()
    scan = Scan(args.running, fixture_result())
    answers = {
        ("POST", "/ping"): (200, PING),
        ("GET", "/device/status"): (200, {"mode": "BR_RUNNING"}),
        ("GET", "/device/network"): (200, {"trial": None}),
    }
    server = ThreadingHTTPServer(
        ("127.0.0.1", args.port), partial(Handler, answers=answers, scan=scan)
    )
    print(f"http://localhost:{args.port}/topology.html")
    server.serve_forever()


if __name__ == "__main__":
    main()
