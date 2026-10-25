#!/usr/bin/env bash
# Builds the host harness against desktop GStreamer and streams to the fake ingestion server.
# Needs: libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libsoup-3.0-dev,
#        gstreamer1.0-plugins-{good,bad,ugly}, ffprobe.
#   tools/host-test/run.sh [seconds] [i420|mjpeg] [rotation] [mirror]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CPP="$HERE/../../app/src/main/cpp"
OUT="${OUT:-$(mktemp -d)}"
SECONDS_TO_RUN="${1:-12}"
MODE="${2:-i420}"
PORT=8089

cc -O1 -g -Wall -I"$HERE/shim" -I"$CPP" \
  "$HERE/main.c" "$HERE/audio_stub.c" "$CPP/engine.c" "$CPP/hls_uploader.c" "$CPP/http.c" \
  $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 gio-2.0 libsoup-3.0) -lm \
  -o "$OUT/host-test"

python3 "$HERE/fake_ingest.py" $PORT "$OUT/ingest" &
SERVER=$!
sleep 0.5
"$OUT/host-test" "http://127.0.0.1:$PORT/http_upload_hls?cid=test-key&copy=0&file=" \
  "$SECONDS_TO_RUN" "$MODE" "${3:-0}" "${4:-0}" 2> "$OUT/host-test.log" || { tail -30 "$OUT/host-test.log"; kill $SERVER; exit 1; }
kill -TERM $SERVER; wait $SERVER || true

python3 - "$OUT/ingest" <<'PY'
import json, subprocess, sys, os
d = sys.argv[1]
rep = json.load(open(os.path.join(d, "report.json")))
print("protocol violations:", rep["errors"] or "none")
print("playlists:", rep["playlists"], "segments:", len(rep["segments"]), "UA:", rep["user_agents"])
print("first requests:", rep["order"][:4])
ok = not rep["errors"] and len(rep["segments"]) > 0
total = 0.0
for s in rep["segments"]:
    p = os.path.join(d, s["name"])
    raw = open(p, "rb").read(188 * 2)
    pat_first = raw[0] == 0x47 and ((raw[1] & 0x1f) << 8 | raw[2]) == 0
    probe = json.loads(subprocess.run(
        ["ffprobe", "-v", "error", "-show_streams", "-show_format", "-of", "json", p],
        capture_output=True, text=True).stdout or "{}")
    frames = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v", "-show_frames", "-read_intervals", "%+#1",
         "-show_entries", "frame=key_frame,pict_type", "-of", "csv=p=0", p],
        capture_output=True, text=True).stdout.split()
    codecs = sorted(st["codec_name"] for st in probe.get("streams", []))
    dur = float(probe.get("format", {}).get("duration", 0))
    total += rep["durations"].get(s["name"], 0)
    starts_idr = bool(frames) and frames[0].startswith("1")
    good = pat_first and codecs == ["aac", "h264"] and starts_idr
    ok &= good
    print(f'{s["name"]}: {s["bytes"]:7d} B  EXTINF={rep["durations"].get(s["name"])}  probe={dur:.2f}s  '
          f'{codecs}  PAT-first={pat_first}  starts-with-IDR={starts_idr}  {"OK" if good else "BAD"}')
print(f"total listed duration: {total:.2f}s")
# Whole-stream continuity: concatenate and decode.
cat = os.path.join(d, "all.ts")
with open(cat, "wb") as f:
    for s in rep["segments"]:
        f.write(open(os.path.join(d, s["name"]), "rb").read())
dec = subprocess.run(["ffmpeg", "-v", "error", "-i", cat, "-f", "null", "-"], capture_output=True, text=True)
print("concatenated decode errors:", dec.stderr.strip() or "none")
ok &= not dec.stderr.strip()
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
PY
