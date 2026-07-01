# WL2 FFmpeg Replay Lab

Interactive human demo for `wl2:ffmpeg`.

The window is meant to exercise the module directly: load a media file, generate
a synthetic source, play/pause, scrub, seek by frame/time, switch seek modes,
probe video colorspace conversions, probe audio sample conversions, run optional
FilterGraph scaling, transcode, remux, move compressed packets through the
packet queue, export stills/clips, and inspect timestamp diagnostics.

Every action that writes a file — generate source, export test video, transcode,
remux, packet-queue round trip, export still, export clip — first asks the user
for a destination through the native save dialog. After a write succeeds an
**Open / Verify** bar appears beneath the preview that loads the produced file
back into the lab so the result can be played or inspected. Nothing is silently
dropped into `/tmp`.

The **Export Test Video** action writes a test video of the requested length with
a burned-in clock and frame counter and, when `tone` support is enabled, synced
tone audio. This makes it a quick fixture for playback, seeking, A/V sync,
transcoding, and conversion tests.

`Open File`, the save dialogs, and the folder picker all use the native Slint
dialogs, so this example enables filesystem access in its runtime options.

Build:

```sh
cmake -S . -B build -DWL2_ENABLE_FFMPEG=ON -DWL2_ENABLE_SLINT=ON -DWL2_BUILD_EXAMPLES=ON
cmake --build build --target wl2_ffmpeg_replay_lab_example
```

Run:

```sh
build/bin/wl2_ffmpeg_replay_lab_example
```

Compile-only smoke:

```sh
build/bin/wl2_ffmpeg_replay_lab_example --compile-only
```

Headless self-test (runs real fixture export, demux, decode, audio conversion,
timestamp diagnostics, packet queue profiling, and A/V plan, then prints a JSON
metrics report):

```sh
build/bin/wl2_ffmpeg_replay_lab_example --selftest
```
