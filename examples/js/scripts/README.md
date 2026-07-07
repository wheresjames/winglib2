# JavaScript Script Examples

## morph3d.js

`morph3d.js` is a single-file 3D + Slint demo. The JavaScript builds a dynamic
25x25 morphing mesh, renders it through `wl2:3d` into a FrameRing, compiles its
Slint UI from an inline template literal, and displays the frame in that window.

Run the windowed demo:

```sh
wl2 run --allow ui,graphics,shared-memory:/wl2_morph3d_ examples/js/scripts/morph3d.js
```

Run the display-free smoke path:

```sh
wl2 run --allow graphics,shared-memory:/wl2_morph3d_ examples/js/scripts/morph3d.js -- --compile-only
```

Save one full geometry loop:

```sh
wl2 run --allow graphics,shared-memory:/wl2_morph3d_ examples/js/scripts/morph3d.js -- --output /tmp/morph3d.avi
```

Run the windowed selftest under a display-capable environment:

```sh
wl2 run --allow ui,graphics,shared-memory:/wl2_morph3d_ examples/js/scripts/morph3d.js -- --selftest
```

Stereo/color-key rendering is intentionally deferred.

## http_video_client.js

A browser UI that live-streams an arbitrary network stream over WebRTC, with UI
options to save the stream to a `.webm` file and to save screen captures (`.png`).
It accepts RTSP (`rtsp://`), HLS (`http://…m3u8`), DASH (`http://…mpd`), MJPEG
(`http://…/mjpeg`), and SRT (`srt://`) URLs. The server side is thin:
`wl2:http` serves the page, `wl2:webrtc` carries the video track, and
`wl2:gstreamer` builds a per-session ingest pipeline into `vp8enc` →
`rtpvp8pay`. Recording and screenshots happen entirely in the browser
(`MediaRecorder` on the received `MediaStream`, and a `<video>`→`<canvas>` frame
grab), so the server never writes to your filesystem — the Save buttons trigger
browser downloads.

Run from the repository root:

```sh
./build/bin/wl2 run --allow-declared \
  examples/js/scripts/http_video_client.js -- --port=8080
```

Then open the printed `http://127.0.0.1:8080/` URL in a browser on the same
machine, enter a stream URL (e.g. `rtsp://127.0.0.1:8554/stream` or
`http://127.0.0.1:8080/hls/stream.m3u8` or `http://127.0.0.1:8080/mjpeg`), and click Start. Use the Test Pattern
source to try it without a protocol output. `--url=<url>` (alias `--rtsp=`)
changes the default URL hint the page offers; `--width/--height/--fps` set the
encoded stream size and frame rate. The player UI includes a Client buffer
seconds setting for HLS/DASH URLs; it defaults to 6 seconds and intentionally
adds relay-side latency to smooth segmented playback before WebRTC encoding.

Element readiness is scheme-aware: `/api/status?url=…` uses
`wl2:gstreamer.requiredElementsForUri()` to report exactly which source elements
the entered URL needs (`rtspsrc` for RTSP, `souphttpsrc`+`hlsdemux` for HLS,
`dashdemux` for DASH, `souphttpsrc`+`multipartdemux`+`jpegdec` for MJPEG,
`srtsrc` for SRT) and which are missing; `vp8enc`/
`rtpvp8pay` are always required for the WebRTC track. This is a manual,
browser-driven example (like `http_video_server.js`); it is not in CTest
because it needs a real browser.

## Local full circle (http_video_server.js protocol output)

`http_video_server.js` has a **Protocol output** panel that re-streams
its selected source over a protocol chosen from a dropdown, with Start/Stop
buttons and a copy-to-clipboard **Stream URL**:

| Protocol | Stream URL | Consumer |
|---|---|---|
| RTSP | `rtsp://127.0.0.1:<port>/stream` | `http_video_client.js`, VLC, `gst-launch` |
| HLS | `http://127.0.0.1:<httpport>/hls/stream.m3u8` | same (segment-bound latency) |
| DASH | `http://127.0.0.1:<httpport>/dash/stream.mpd` | same (segment-bound latency) |
| SRT | `srt://127.0.0.1:<port>` | same (low latency) |
| MJPEG | `http://127.0.0.1:<httpport>/mjpeg` | `http_video_client.js`, a browser tab / `<img>` |
| WebRTC | `http://127.0.0.1:<httpport>/` | a browser (the page itself; not `http_video_client.js`) |

RTSP is a tiny in-process RTSP/RTP server (pure JS over `wl2:asio`,
TCP-interleaved, VP8). HLS/DASH re-encode to H.264 with helper-built
`hlssink`/`dashsink` pipelines writing into a per-run temp directory served at
`/hls` and `/dash` (created with `wl2:fs.mkdtemp`, wiped on Stop). SRT uses
`srtsink mode=listener` (H.264 over MPEG-TS). MJPEG serves JPEG frames over a
long-lived `multipart/x-mixed-replace` response (`wl2:http` `routeStream`);
`http_video_client.js` consumes it with `souphttpsrc ! multipartdemux ! jpegdec`.
WebRTC is the page's existing direct browser path — Start just surfaces the
copyable page URL, and Stop never takes the page down. One protocol output runs
at a time; pipeline errors surface through `/api/stream/status` via
`Pipeline.watchBus()` and stop the server.

```sh
# Terminal 1: capture a source and serve it over the chosen protocol.
./build/bin/wl2 run --trust-declared \
  examples/js/scripts/http_video_server.js -- \
  --port=8080 --serve-stream --stream-protocol=rtsp --rtsp-port=8554

# Terminal 2: consume the copied URL and serve it to the browser over WebRTC.
./build/bin/wl2 run --trust-declared \
  examples/js/scripts/http_video_client.js -- \
  --port=8090 --url=rtsp://127.0.0.1:8554/stream
```

Then open the Terminal 2 URL in a browser and click Start. The Terminal 1 UI
can also start/stop any protocol on demand without `--serve-stream`; it polls
`/api/stream/status` so it stays in sync if the server stops on its own (e.g. a
source error). To prove a link without a browser:

```sh
gst-launch-1.0 uridecodebin uri=<copied-url> ! videoconvert ! fakesink
```

Flags: `--serve-stream` (alias `--serve-rtsp`) starts the server at startup;
`--stream-protocol=rtsp|hls|dash|srt|mjpeg` picks its protocol.
`--rtsp-source=` is `test|camera|file` (default `test`); pair it with
`--rtsp-camera=<dev>` or `--rtsp-file=<path>`. `--rtsp-host`/`--rtsp-port`/
`--rtsp-path` configure RTSP, `--srt-port` (default `7001`) configures SRT, and
`--out-dir=` overrides the HLS/DASH output root. The browser UI exposes a
Buffer seconds setting for HLS/DASH; the default is 12 seconds, favoring smooth
playback over latency.

Notes: RTSP is TCP-interleaved transport only, one RTSP client at a time. A
real camera can't be opened by both the protocol output and a WebRTC browser
session at once — use one consumer at a time (the test pattern and file sources
have no such limitation). HLS/DASH re-encode to H.264 (`x264enc`), so they need
gst-plugins with `x264enc`, `hlssink`/`dashsink`; SRT needs `srtsink`; MJPEG
needs `jpegenc` — `/api/stream/status` reports per-protocol availability. No
external media server (`MediaMTX`/`nginx-rtmp`/`vlc`) is needed — everything is
in-process.
