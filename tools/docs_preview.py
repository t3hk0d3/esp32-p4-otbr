#!/usr/bin/env python3
"""Preview server for documentation screenshots: the web UI of this checkout with recorded board answers.

Serves the frontend directory on localhost. Every other GET is answered from a recording
(tools/docs_preview/recording.json). With --record URL the GETs are first passed to that board and recorded; only GETs
reach the board, GET /device/update is passed without `check`, and the GETs that start a capture or open a
WebSocket are never passed, so recording changes nothing on the board. Other
methods answer 409 `preview` (or the canned answers below), so pressing a button changes nothing.

Secrets are scrubbed while recording: network keys and PSKc in JSON, and the network key TLV of hex datasets, become
a fixed dummy key; global IPv6 addresses move into the documentation prefix 2001:db8::/32. --scrub rewrites an
existing recording the same way.

    python3 tools/docs_preview.py --record http://192.168.1.117   # record while clicking through the pages
    python3 tools/docs_preview.py                                  # replay: http://localhost:8766/
"""

import argparse
import base64
import json
import re
import threading
import urllib.error
import urllib.request
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
FRONTEND = REPO / "p4" / "components" / "esp_ot_br_server" / "frontend"
RECORDING = REPO / "tools" / "docs_preview" / "recording.json"
DUMMY_KEY = "00112233445566778899aabbccddeeff"
SECRET_KEY = re.compile(r"(network_?key|pskc)", re.IGNORECASE)
HEX = re.compile(r"^[0-9a-fA-F]{32,}$")
# GETs that start something on the board
NEVER_FORWARD = {"/device/sniffer.pcap", "/device/console/ws", "/zigbee"}
PING = {
    "error": 0,
    "message": "OK",
    "result": {
        "replies": [
            {"seq": i, "size": 32, "hlim": 64, "rtt": 20 + i} for i in range(3)
        ],
        "statistics": {
            "sent": 3,
            "received": 3,
            "loss_percent": 0.0,
            "rtt_min": 20,
            "rtt_avg": 21,
            "rtt_max": 22,
        },
    },
}


def scrub_tlvs(text):
    """Replaces the network key TLV (type 5) of a hex dataset with the dummy key."""
    try:
        raw = bytearray.fromhex(text.strip())
    except ValueError:
        return text
    i = 0
    while i + 2 <= len(raw):
        kind, size = raw[i], raw[i + 1]
        if kind == 5 and size == 16:
            raw[i + 2 : i + 18] = bytes.fromhex(DUMMY_KEY)
        i += 2 + size
    return raw.hex()


GUA = re.compile(r"\b[23][0-9a-fA-F]{3}:[0-9a-fA-F]{1,4}:")


def scrub_addresses(text):
    """Puts global IPv6 addresses into the documentation prefix 2001:db8::/32."""
    return GUA.sub("2001:db8:", text)


def scrub(value, key=""):
    if isinstance(value, dict):
        return {k: scrub(v, k) for k, v in value.items()}
    if isinstance(value, list):
        return [scrub(v, key) for v in value]
    if isinstance(value, str) and SECRET_KEY.search(key):
        return DUMMY_KEY
    if isinstance(value, str) and HEX.match(value) and "dataset" in key.lower():
        return scrub_tlvs(value)
    if isinstance(value, str):
        return scrub_addresses(value)
    return value


def scrub_body(content_type, body, path):
    if "json" in content_type:
        try:
            return json.dumps(scrub(json.loads(body))).encode()
        except ValueError:
            return body
    if path.startswith("/node/dataset") and HEX.match(
        body.decode(errors="replace").strip()
    ):
        return scrub_tlvs(body.decode()).encode()
    return body


class Recording:
    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()
        self.entries = json.loads(path.read_text()) if path.exists() else {}

    def get(self, key):
        entry = self.entries.get(key)
        if entry is None:
            return None
        return entry["status"], entry["type"], base64.b64decode(entry["body"])

    def put(self, key, status, content_type, body):
        with self.lock:
            self.entries[key] = {
                "status": status,
                "type": content_type,
                "body": base64.b64encode(body).decode(),
            }
            self.path.parent.mkdir(parents=True, exist_ok=True)
            self.path.write_text(
                json.dumps(self.entries, indent=1, sort_keys=True) + "\n"
            )


def request_key(path):
    """The recording key of a GET: the path, with the query except `check` and cache busters."""
    if "?" not in path:
        return path
    base, query = path.split("?", 1)
    keep = [
        p for p in query.split("&") if p and not p.startswith(("check=", "_=", "t="))
    ]
    return base + ("?" + "&".join(keep) if keep else "")


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, recording, board, **kwargs):
        self.recording = recording
        self.board = board
        super().__init__(*args, directory=str(FRONTEND), **kwargs)

    def log_message(self, *args):
        pass

    def send(self, status, content_type, body):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def is_static(self, path):
        return path == "/" or (FRONTEND / path.lstrip("/")).is_file()

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if self.is_static(path):
            super().do_GET()
            return
        key = request_key(self.path)
        if path in NEVER_FORWARD:
            self.send(
                409,
                "application/json",
                b'{"error":"preview","detail":"not available in the preview"}',
            )
            return
        if self.board:
            req = urllib.request.Request(
                self.board + key, headers={"Accept": self.headers.get("Accept", "*/*")}
            )
            try:
                with urllib.request.urlopen(req, timeout=10) as r:
                    status, ctype, body = (
                        r.status,
                        r.headers.get("Content-Type", ""),
                        r.read(),
                    )
            except urllib.error.HTTPError as e:
                status, ctype, body = (
                    e.code,
                    e.headers.get("Content-Type", ""),
                    e.read(),
                )
            except (urllib.error.URLError, TimeoutError) as e:
                detail = json.dumps({"error": "board_unreachable", "detail": str(e)})
                self.send(502, "application/json", detail.encode())
                return
            body = scrub_body(ctype, body, path)
            accept = "text" if "text/plain" in self.headers.get("Accept", "") else ""
            self.recording.put(key + ("#text" if accept else ""), status, ctype, body)
            self.send(status, ctype, body)
            return
        accept = "#text" if "text/plain" in self.headers.get("Accept", "") else ""
        hit = self.recording.get(key + accept) or self.recording.get(key)
        if hit is None:
            self.send(
                404,
                "application/json",
                b'{"error":"not_found","detail":"not in the recording"}',
            )
        else:
            self.send(*hit)

    def answer_other(self):
        self.rfile.read(int(self.headers.get("Content-Length") or 0))
        if self.path.split("?", 1)[0] == "/ping":
            self.send(200, "application/json", json.dumps(PING).encode())
            return
        self.send(
            409,
            "application/json",
            b'{"error":"preview","detail":"this is a preview without a board"}',
        )

    do_POST = do_PUT = do_DELETE = answer_other


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=8766)
    parser.add_argument(
        "--record", metavar="URL", help="pass GETs to this board and record them"
    )
    parser.add_argument(
        "--scrub", action="store_true", help="scrub the recording again and exit"
    )
    args = parser.parse_args()
    if args.record and not args.record.startswith(("http://", "https://")):
        parser.error("--record needs an http:// or https:// URL")
    if args.scrub:
        rec = Recording(RECORDING)
        for key, entry in list(rec.entries.items()):
            body = scrub_body(
                entry["type"], base64.b64decode(entry["body"]), key.split("?", 1)[0]
            )
            rec.put(key, entry["status"], entry["type"], body)
        return
    handler = partial(
        Handler, recording=Recording(RECORDING), board=(args.record or "").rstrip("/")
    )
    server = ThreadingHTTPServer(("127.0.0.1", args.port), handler)
    print(
        f"http://localhost:{args.port}/"
        + (f" (recording {args.record})" if args.record else "")
    )
    server.serve_forever()


if __name__ == "__main__":
    main()
