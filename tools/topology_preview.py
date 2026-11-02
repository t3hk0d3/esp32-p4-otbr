#!/usr/bin/env python3
"""Dev preview of the Topology page without a board.

Serves the frontend directory on localhost and answers the page's requests with canned data:
GET and POST /device/topology a "done" state with the result of the test fixture (tools/tests/test_topology_page.py,
evaluated with node), POST /ping a ping result, GET /device/status BR_RUNNING and GET /device/network no trial.

    python3 tools/topology_preview.py --port 8765    # then open http://localhost:8765/topology.html
"""

import argparse
import json
import re
import subprocess
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
        "replies": [{"seq": i, "size": 32, "hlim": 64, "rtt": 20 + i} for i in range(3)],
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
    fixture = re.search(r'FIXTURE = r"""(.*?)"""', FIXTURE_FILE.read_text(), re.S).group(1)
    source = f"const L = require({json.dumps(str(TOPOLOGY_JS))});{fixture}process.stdout.write(JSON.stringify(R));"
    out = subprocess.run(
        ["node", "-e", source], capture_output=True, text=True, check=True, timeout=20
    )
    return json.loads(out.stdout)


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, answers, **kwargs):
        self.answers = answers
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
        if ("GET", path) in self.answers:
            self.reply(*self.answers[("GET", path)])
        else:
            super().do_GET()

    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length") or 0))
        path = self.path.split("?", 1)[0]
        self.reply(*self.answers.get(("POST", path), (404, {"error": "not_found"})))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    done = {"state": "done", "age_s": 5, "result": fixture_result()}
    answers = {
        ("GET", "/device/topology"): (200, done),
        ("POST", "/device/topology"): (202, done),
        ("POST", "/ping"): (200, PING),
        ("GET", "/device/status"): (200, {"mode": "BR_RUNNING"}),
        ("GET", "/device/network"): (200, {"trial": None}),
    }
    server = ThreadingHTTPServer(
        ("127.0.0.1", args.port), partial(Handler, answers=answers)
    )
    print(f"http://localhost:{args.port}/topology.html")
    server.serve_forever()


if __name__ == "__main__":
    main()
