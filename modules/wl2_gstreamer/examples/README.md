# wl2:gstreamer Examples

These examples are specific to the `wl2:gstreamer` module. They use
`wl2:membus` only as the shared-memory transport that the GStreamer bridges read
from or write to.

Minimal pipeline:

```sh
wl2 run modules/wl2_gstreamer/examples/js/gst_minimal.js
```

Test pattern to `VideoBuffer`:

```sh
wl2 run --allow-shared-memory --shared-memory-allow /wl2_gst_ \
        modules/wl2_gstreamer/examples/js/gst_testpattern_to_membus.js
```

Tone to `AudioBuffer`:

```sh
wl2 run --allow-shared-memory --shared-memory-allow /wl2_gst_ \
        modules/wl2_gstreamer/examples/js/gst_tone_to_membus.js
```

Compressed packet relay:

```sh
wl2 run --allow-shared-memory --shared-memory-allow /wl2_gst_ \
        modules/wl2_gstreamer/examples/js/gst_compressed_packet_relay.js
```

Pipeline lab:

```sh
wl2 run --allow-shared-memory --shared-memory-allow /wl2_gst_ \
        --allow-filesystem-reads --filesystem-read-root /tmp \
        modules/wl2_gstreamer/examples/js/gst_pipeline_lab.js -- --output /tmp/wl2_gst_pipeline_lab.webm
```

Capture preview:

```sh
wl2 run --allow-shared-memory --shared-memory-allow /wl2_gst_ \
        modules/wl2_gstreamer/examples/js/gst_capture_preview.js
wl2 run --allow-shared-memory --shared-memory-allow /wl2_gst_ \
        modules/wl2_gstreamer/examples/js/gst_capture_preview.js -- --list-devices --play --device /dev/video0
```

Loopback RTP:

```sh
wl2 run --allow-shared-memory --shared-memory-allow /wl2_gst_ \
        --allow-network --network-allow 127.0.0.1:45596 \
        --allow-listen --listen-allow 127.0.0.1:45596 \
        modules/wl2_gstreamer/examples/js/gst_loopback_rtp.js
```

Advanced features (tee multi-sink, SharedQueue-driven overlay, snapshot,
latency and caps utilities):

```sh
wl2 run --allow-shared-memory --shared-memory-allow /wl2_gst_adv \
        modules/wl2_gstreamer/examples/js/gst_advanced_lab.js
```
