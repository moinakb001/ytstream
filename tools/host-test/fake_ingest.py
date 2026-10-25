#!/usr/bin/env python3
"""Fake YouTube HLS ingestion endpoint that enforces the documented protocol rules.

PUT /http_upload_hls?cid=KEY&copy=0&file=NAME  -> stores NAME under OUT_DIR.
Checks: playlist sequence starts at 0 and never decreases; <= 5 outstanding segments;
segment names end in .ts, are unique, and match playlist entries; EXTINF <= TARGETDURATION.
Writes a JSON report to OUT_DIR/report.json on SIGTERM/SIGINT.
"""
import json
import os
import re
import signal
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

OUT = sys.argv[2] if len(sys.argv) > 2 else "out"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8089
os.makedirs(OUT, exist_ok=True)

state = {"errors": [], "playlists": 0, "segments": [], "received": set(), "last_seq": -1,
         "listed": {}, "user_agents": set(), "order": []}


def err(msg):
    state["errors"].append(msg)
    print("VIOLATION:", msg, flush=True)


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"  # persistent connections

    def log_message(self, *a):
        pass

    def do_PUT(self):
        u = urlparse(self.path)
        q = parse_qs(u.query, keep_blank_values=True)
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        state["user_agents"].add(self.headers.get("User-Agent"))
        if u.path != "/http_upload_hls" or q.get("copy") != ["0"] or not q.get("cid"):
            err(f"bad URL {self.path}")
            return self.reply(400)
        name = q.get("file", [""])[0]
        if not re.fullmatch(r"[A-Za-z0-9_/.\-]+", name):
            err(f"bad file name {name!r}")
            return self.reply(400)
        state["order"].append(name)
        with open(os.path.join(OUT, name.replace("/", "_")), "wb") as f:
            f.write(body)
        if name.endswith(".m3u8"):
            self.playlist(body.decode())
            return self.reply(200)
        if not name.endswith(".ts"):
            err(f"segment without .ts: {name}")
            return self.reply(400)
        if name in state["received"]:
            err(f"duplicate segment {name}")
        state["received"].add(name)
        state["segments"].append({"name": name, "bytes": len(body)})
        return self.reply(200 if name in state["listed"] else 202)

    def playlist(self, text):
        state["playlists"] += 1
        lines = text.strip().splitlines()
        if lines[0] != "#EXTM3U":
            err("playlist missing #EXTM3U")
        td = int(next(l for l in lines if l.startswith("#EXT-X-TARGETDURATION:")).split(":")[1])
        seq = int(next(l for l in lines if l.startswith("#EXT-X-MEDIA-SEQUENCE:")).split(":")[1])
        if state["playlists"] == 1 and seq != 0:
            err(f"first playlist media sequence {seq} != 0")
        if seq < state["last_seq"]:
            err(f"media sequence went backwards {state['last_seq']} -> {seq}")
        state["last_seq"] = seq
        names, durs = [], []
        for i, l in enumerate(lines):
            if l.startswith("#EXTINF:"):
                d = float(l[8:].rstrip(","))
                durs.append(d)
                names.append(lines[i + 1])
                if round(d) > td:
                    err(f"EXTINF {d} > TARGETDURATION {td}")
                if d > 5:
                    err(f"segment longer than 5s: {d}")
        for n, d in zip(names, durs):
            state["listed"][n] = d
        outstanding = [n for n in names if n not in state["received"]]
        if len(outstanding) > 5:
            err(f"{len(outstanding)} outstanding segments")

    def reply(self, code):
        self.send_response(code)
        self.send_header("Content-Length", "0")
        self.end_headers()


def dump(*_):
    rep = {"errors": state["errors"], "playlists": state["playlists"], "segments": state["segments"],
           "durations": state["listed"], "user_agents": sorted(a for a in state["user_agents"] if a),
           "order": state["order"][:8]}
    with open(os.path.join(OUT, "report.json"), "w") as f:
        json.dump(rep, f, indent=1)
    sys.exit(0)


signal.signal(signal.SIGTERM, dump)
signal.signal(signal.SIGINT, dump)
ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
