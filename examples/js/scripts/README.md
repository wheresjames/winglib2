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

Run the windowed selftest under a display-capable environment:

```sh
wl2 run --allow ui,graphics,shared-memory:/wl2_morph3d_ examples/js/scripts/morph3d.js -- --selftest
```

FFmpeg/video encoding and stereo/color-key rendering are intentionally deferred.
