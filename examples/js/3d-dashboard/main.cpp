// 3D-UI dashboard: a self-contained executable that combines wl2:3d and
// wl2:slint. A windowed Slint UI hosts a live 3D viewport (the 3D engine renders
// into a FrameRing that the UI displays as an image), and the control panel
// drives the engine: camera moves, primitives, markers, attention cues,
// animation timelines, particles, overlays, picking, and detection tracking.
//
// The script and UI markup are embedded as wl2:/3d-dashboard resources, so the
// executable carries its UI with no runtime file access. The runtime grants only
// the capabilities the demo needs:
//   * allowUi          — open the dashboard window (windowed modes only)
//   * allowGraphics    — create the headless graphics context the renderer uses
//   * allowSharedMemory — the FrameRing bridge and the detection queue, scoped to
//                         the "/wl2_3d_dashboard_" name prefix
//
// Modes (script args):
//   --compile-only  Compile the UI and run the full engine feature demo
//                   headlessly with assertions; no window. (CI smoke test.)
//   --selftest      Open the window, drive a few callbacks from a timer, quit.
//   (no flag)       Interactive windowed dashboard.
#include "wl2/wl2.h"
#include "wl2/crash_report.h"
#include "wl2_3d/wl2_3d.h"
#include "wl2_membus/wl2_membus.h"
#include "wl2_slint/wl2_slint.h"

#include <iostream>

void wl2_register_embedded_resources(wl2::ResourceStore& store);

int main(int argc, char** argv) {
    wl2::crash::installFromArgs(argc, argv);

    wl2::RuntimeOptions options;
    options.allowFilesystem = false;
    options.allowUi = true;
    options.allowGraphics = true;
    options.allowSharedMemory = true;
    // The demo names every shared-memory object under this per-run prefix so the
    // capability stays narrowly scoped (the default-deny shared-memory gate).
    options.sharedMemoryAllowList.emplace_back("/wl2_3d_dashboard_");
    for (int i = 1; i < argc; ++i) {
        options.scriptArgs.emplace_back(argv[i]);
    }
    // wl2:3d requires wl2:membus; register it alongside the two feature modules.
    options.staticModules.push_back(wl2_membus_register_module);
    options.staticModules.push_back(wl2_slint_register_module);
    options.staticModules.push_back(wl2_3d_register_module);

    wl2::Runtime runtime(std::move(options));
    wl2_register_embedded_resources(runtime.resources());

    auto result = runtime.runModule("wl2:/3d-dashboard/scripts/main.js");
    if (!result) {
        std::cerr << result.error().code() << ": " << result.error().message() << '\n';
        return 1;
    }
    return result.value();
}
