# ytstream

An Android app that streams a camera to **YouTube Live over HLS**. You can pick a USB (UVC)
webcam, a built-in camera, or an external camera, and choose the audio input separately. The
app can also create the YouTube broadcast for you, with title, description, privacy, category,
tags, latency, DVR, made-for-kids and thumbnail.

Almost all of the app is C. The NDK code handles capture, the GStreamer media pipeline,
segmenting, HLS upload over HTTPS and the YouTube Live API. Java is used only where Android
offers no C API: the settings form, the USB permission prompt, Google sign-in, and the
foreground service.

```
 USB webcam ──libusb/libuvc──┐                      ┌─ RGBA appsink ─► preview (ANativeWindow)
 Camera2 (NDK) ─AImageReader─┴► appsrc ─► [jpegdec] ─► videoflip ─► scale/rate ─► tee
                                                                                    └─ H.264 (MediaCodec | x264 | OpenH264) ─┐
 Mic (AAudio, any input device) ─► appsrc ─┐                                                                                  ├─► splitmuxsink(mpegtsmux) ─► in-memory .ts segments
 or silent track (audiotestsrc) ───────────┴─► AAC (voaacenc) ──────────────────────────────────────────────────────────────┘
                                                                     segments ─► HLS uploader (libsoup, HTTPS PUT) ─► YouTube
```

## Features

- **Video sources:** any USB Video Class webcam (MJPEG preferred, YUYV also supported) through
  libusb + libuvc with no root needed, plus every Camera2 camera through the NDK camera API.
  That includes front and back cameras, and USB cameras on phones whose HAL exposes them as
  `EXTERNAL`.
- **Audio sources, chosen separately:** every Android input device (built-in mic, a USB
  camera's mic, USB/wired/Bluetooth headsets, …) captured with AAudio, or a silent track.
- **Encoding:** hardware H.264 through MediaCodec (`amcvidenc`), with x264 or OpenH264 as a
  software fallback; AAC audio. You set the resolution, frame rate, bitrate, segment length
  (1–4 s), rotation and mirroring.
- **YouTube HLS ingestion** follows
  [Google's spec](https://developers.google.com/youtube/v3/live/guides/hls-ingestion):
  - closed-GOP MPEG-TS segments that each start with PAT/PMT and an IDR frame
  - HTTPS `PUT` over persistent connections
  - the media sequence starts at 0
  - a fresh playlist goes out before every segment, with no more than 5 outstanding segments
  - segment names are unique across restarts
  - 401 responses are treated as fatal, and other failures are retried with backoff
- **Two ways to go live:**
  - **YouTube account:** the app creates the HLS stream and the broadcast, binds them, sets the
    category, tags and thumbnail, shows the YouTube status, and ends the broadcast when you
    stop. Title and description can be edited while you're live.
  - **Stream key:** paste an HLS stream key from YouTube Studio. No Google Cloud setup is
    needed.
- Runs in a foreground service (with camera and microphone types) and holds wake and Wi-Fi
  locks. If you plug in a UVC camera, Android offers to open the app.

## Building

Requirements:

- Android SDK with platform 35, NDK `28.2.13676358` and CMake `3.31.6` (Gradle can install
  them).
- The **GStreamer Android SDK**: download
  `gstreamer-1.0-android-universal-<version>.tar.xz` from
  <https://gstreamer.freedesktop.org/data/pkg/android/> (tested with 1.28.8) and extract it.
- `pkg-config` on the build host (used by GStreamer's CMake integration).

```sh
git clone --recursive <this repo>     # or: git submodule update --init
cat >> local.properties <<EOF
sdk.dir=/path/to/android-sdk
gstreamer.dir=/path/to/gstreamer-1.0-android-universal   # contains arm64/, armv7/, x86_64/
EOF
./gradlew assembleDebug
```

You can also set `GSTREAMER_ROOT_ANDROID` instead of `gstreamer.dir`. The build compiles the
app's C code, libusb, libuvc and cJSON into the single `libgstreamer_android.so` that
GStreamer's CMake module produces, so GLib, GStreamer and libsoup are loaded exactly once.

The universal APK is about 66 MB because it ships three ABIs. To build just one ABI:
`./gradlew assembleDebug -Pandroid.injected.build.abi=arm64-v8a` (about 22 MB).

## YouTube account mode: one-time Google Cloud setup

Stream-key mode works without any of this. Account mode needs an OAuth client so the app can
call the YouTube Data API:

1. In the [Google Cloud console](https://console.cloud.google.com/), create a project and
   enable **YouTube Data API v3**.
2. Configure the OAuth consent screen and add your Google account as a test user.
3. Create an **OAuth client ID** of type **Android** with package `com.ytstream` and the SHA-1
   of your signing certificate (`./gradlew signingReport` shows the debug keystore's SHA-1).
   The app asks for the `https://www.googleapis.com/auth/youtube` scope through Play services,
   so no client secret is stored in the app.
4. Live streaming must be enabled on the channel (YouTube Studio → *Go live*). This can take
   up to 24 hours the first time.

For **stream-key mode**: in YouTube Studio, create a stream key whose type is **HLS** (not
RTMP) and paste the key into the app.

## Using it

1. Pick a **video source**. For a USB camera, accept the USB permission prompt. Pick an
   **audio source**: the USB camera's mic, the phone mic, a headset, or "None (silent
   track)".
2. Choose **YouTube account** (sign in) or **Stream key**.
3. Fill in the title, description, privacy, category, tags and so on, and optionally pick a
   thumbnail (long-press to clear it).
4. Set the resolution, frame rate and bitrate, then tap **Go live**. The status line shows the
   segment upload rate and YouTube's broadcast and stream health. **Open on YouTube** opens
   the watch page.

## Testing on a desktop

`tools/host-test/run.sh` builds the app's real `engine.c`, `hls_uploader.c` and `http.c`
against desktop GStreamer. It feeds them synthetic camera frames (raw I420, like the NDK camera
path, or MJPEG, like the UVC path) and streams to `fake_ingest.py`, a local server that applies
YouTube's HLS ingestion rules. It then checks each segment with ffprobe: PAT/PMT first, IDR
first, H.264 + AAC, and a clean decode of the concatenated stream.

```sh
sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libsoup-3.0-dev \
  gstreamer1.0-plugins-{good,bad,ugly} ffmpeg
tools/host-test/run.sh 12 mjpeg 90 0     # seconds, i420|mjpeg, rotation, mirror
```

## Source layout

| Path | What it does |
| --- | --- |
| `app/src/main/cpp/engine.c` | GStreamer pipelines (preview and live), encoder selection, in-memory segmenting |
| `app/src/main/cpp/uvc_source.c` | UVC capture: libusb wraps the fd from `UsbDeviceConnection`, libuvc picks the mode |
| `app/src/main/cpp/ndk_camera.c` | Camera2 capture through the NDK (`ACameraManager` + `AImageReader`) |
| `app/src/main/cpp/audio_capture.c` | AAudio input from any device id, timestamped on the pipeline clock |
| `app/src/main/cpp/hls_uploader.c` | YouTube HLS ingestion protocol (playlist window, retries, stats) |
| `app/src/main/cpp/youtube.c` | Live Streaming API: stream, broadcast, bind, video metadata, thumbnail, status, transitions |
| `app/src/main/cpp/session.c` | Go-live orchestration and status JSON |
| `app/src/main/cpp/jni.c` | JNI bridge (`com.ytstream.Native`) |
| `app/src/main/java/com/ytstream/` | UI, USB permission, Play services auth, foreground service |
| `app/src/main/cpp/third_party/` | Submodules: libusb 1.0.29, libuvc, cJSON 1.7.19 |

## Known limitations

- Changing rotation or mirroring while live renegotiates the pipeline, which can stall
  segment output for a couple of seconds.
- If a USB camera is unplugged while live, video stops until it's reconnected and selected
  again. The stream doesn't switch to a placeholder.
- UVC cameras that only offer H.264 or other frame-based formats aren't supported. Nearly all
  webcams also offer MJPEG.
- YouTube HLS ingestion has more latency than RTMP, which is inherent to segment-based
  ingest.
