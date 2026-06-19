// Headless GPU renderer probe / test.
//
// Creates a windowless EGL context, builds a known engine scene (a bumped
// surface-grid mesh + ground grid + lights), renders it once on the GPU and once
// with the CPU rasterizer, and writes both to PPM for visual A/B. It then asserts
// the GPU frame is non-blank, camera-dependent, and within a loose CPU/GPU parity
// tolerance. Run under `EGL_PLATFORM=surfaceless` +
// `MESA_LOADER_DRIVER_OVERRIDE=llvmpipe` in CI.

#include <Magnum/Platform/GLContext.h>
#include <Magnum/Platform/WindowlessEglApplication.h>

#include "wl2_3d_gpu_render.h"
#include "wl2_3d_render.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace td = wl2::three_d;

namespace {

constexpr int kWidth = 256;
constexpr int kHeight = 192;

void write_ppm(const std::string& path, const std::vector<unsigned char>& rgba, int w, int h) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return;
    }
    out << "P6\n" << w << " " << h << "\n255\n";
    for (int i = 0; i < w * h; ++i) {
        out.put(static_cast<char>(rgba[i * 4 + 0]));
        out.put(static_cast<char>(rgba[i * 4 + 1]));
        out.put(static_cast<char>(rgba[i * 4 + 2]));
    }
}

// Count pixels whose color differs from the top-left (background) pixel. A
// rendered scene must paint a meaningful fraction of foreground.
int foreground_pixels(const std::vector<unsigned char>& rgba, int w, int h) {
    const int br = rgba[0], bg = rgba[1], bb = rgba[2];
    int count = 0;
    for (int i = 0; i < w * h; ++i) {
        const int dr = std::abs(rgba[i * 4 + 0] - br);
        const int dg = std::abs(rgba[i * 4 + 1] - bg);
        const int db = std::abs(rgba[i * 4 + 2] - bb);
        if (dr + dg + db > 24) {
            ++count;
        }
    }
    return count;
}

int differing_pixels(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b,
                     int w, int h) {
    int count = 0;
    for (int i = 0; i < w * h; ++i) {
        const int dr = std::abs(a[i * 4 + 0] - b[i * 4 + 0]);
        const int dg = std::abs(a[i * 4 + 1] - b[i * 4 + 1]);
        const int db = std::abs(a[i * 4 + 2] - b[i * 4 + 2]);
        if (dr + dg + db > 24) {
            ++count;
        }
    }
    return count;
}

double mean_abs_rgb_error(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b,
                          int w, int h) {
    uint64_t sum = 0;
    const int totalChannels = w * h * 3;
    for (int i = 0; i < w * h; ++i) {
        sum += static_cast<uint64_t>(std::abs(a[i * 4 + 0] - b[i * 4 + 0]));
        sum += static_cast<uint64_t>(std::abs(a[i * 4 + 1] - b[i * 4 + 1]));
        sum += static_cast<uint64_t>(std::abs(a[i * 4 + 2] - b[i * 4 + 2]));
    }
    return static_cast<double>(sum) / static_cast<double>(totalChannels);
}

td::Color hex(double r, double g, double b) { return td::Color{r, g, b, 1.0}; }

// A bumped surface grid + ground grid + key/fill lights, echoing morph3d.js.
void build_scene(td::Engine& engine) {
    engine.camera.eye = {0.0, 6.0, 15.0};
    engine.camera.target = {0.0, 0.0, 0.0};
    engine.camera.up = {0.0, 1.0, 0.0};
    engine.camera.fovYRadians = td::radians(45.0);
    engine.camera.aspect = static_cast<double>(kWidth) / kHeight;
    engine.camera.near = 0.1;
    engine.camera.far = 1000.0;
    engine.ambientLight = hex(0.333, 0.333, 0.333);

    td::Light key;
    key.kind = td::LightKind::Directional;
    key.direction = {0.35, 0.8, 0.5};
    key.color = hex(1.0, 1.0, 1.0);
    key.intensity = 0.9;
    engine.addLight(key);

    td::Light fill;
    fill.kind = td::LightKind::Directional;
    fill.direction = {-0.6, 0.25, -0.5};
    fill.color = hex(0.416, 0.659, 1.0);
    fill.intensity = 0.35;
    engine.addLight(fill);

    const int cols = 24, rows = 24;
    td::MeshGeometry mesh;
    mesh.dynamic = true;
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < cols; ++x) {
            const double u = (x / static_cast<double>(cols - 1)) * 6.0 - 3.0;
            const double v = (y / static_cast<double>(rows - 1)) * 6.0 - 3.0;
            const double h = 1.4 * std::sin(u) * std::cos(v);
            mesh.vertices.push_back({u, h, v});
        }
    }
    for (int y = 0; y < rows - 1; ++y) {
        for (int x = 0; x < cols - 1; ++x) {
            const uint32_t a = y * cols + x;
            const uint32_t b = y * cols + x + 1;
            const uint32_t c = (y + 1) * cols + x + 1;
            const uint32_t d = (y + 1) * cols + x;
            mesh.indices.insert(mesh.indices.end(), {a, b, c, a, c, d});
        }
    }
    td::recomputeSmoothNormals(mesh);

    td::Node surface;
    surface.kind = "node";
    surface.id = "surface";
    surface.mesh = std::move(mesh);
    surface.material = hex(0.91, 0.94, 0.97);
    surface.rotation = {-0.25, 0.0, 0.0};
    surface.scale = {1.35, 1.35, 1.35};
    engine.addNode(std::move(surface));

    td::Node floor;
    floor.kind = "primitive";
    floor.id = "floor";
    floor.primitive = "grid";
    floor.primitiveOptions["size"] = 12.0;
    floor.primitiveOptions["divisions"] = 12.0;
    floor.position = {0.0, -4.0, 0.0};
    floor.material = hex(0.164, 0.203, 0.251);
    engine.addNode(std::move(floor));
}

}  // namespace

int main(int argc, char** argv) {
    const std::string outDir = argc > 1 ? std::string(argv[1]) : std::string(".");

    Magnum::Platform::WindowlessEglContext glContext{
        Magnum::Platform::WindowlessEglContext::Configuration{}};
    if (!glContext.makeCurrent()) {
        std::fprintf(stderr, "gpu probe: could not make a windowless EGL context current\n");
        return 1;
    }
    const char* magnumArgs[] = {"wl2_3d_gpu_probe", "--magnum-log", "quiet"};
    Magnum::Platform::GLContext context{Magnum::NoCreate, 3, magnumArgs};
    if (!context.tryCreate()) {
        std::fprintf(stderr, "gpu probe: could not create a Magnum GL context\n");
        return 1;
    }

    td::Engine engine;
    build_scene(engine);

    const int stride = kWidth * 4;
    std::vector<unsigned char> gpuFrame(static_cast<size_t>(stride) * kHeight, 0);
    std::vector<unsigned char> cpuFrame(static_cast<size_t>(stride) * kHeight, 0);

    td::MagnumRenderer renderer(kWidth, kHeight);
    renderer.render(engine);
    renderer.readInto(gpuFrame.data(), stride);

    td::render_scene_cpu(engine, cpuFrame.data(), kWidth, kHeight, stride);

    write_ppm(outDir + "/gpu.ppm", gpuFrame, kWidth, kHeight);
    write_ppm(outDir + "/cpu.ppm", cpuFrame, kWidth, kHeight);

    std::vector<unsigned char> gpuPerf(static_cast<size_t>(stride) * kHeight, 0);
    constexpr int kPerfFrames = 8;
    const auto perfStart = std::chrono::steady_clock::now();
    for (int i = 0; i < kPerfFrames; ++i) {
        renderer.render(engine);
        renderer.readInto(gpuPerf.data(), stride);
    }
    const auto perfEnd = std::chrono::steady_clock::now();
    const double avgMs =
        std::chrono::duration<double, std::milli>(perfEnd - perfStart).count() / kPerfFrames;

    // Move the camera and re-render to confirm the GPU output is view-dependent.
    engine.camera.eye = {9.0, 7.0, 9.0};
    std::vector<unsigned char> gpuMoved(static_cast<size_t>(stride) * kHeight, 0);
    renderer.render(engine);
    renderer.readInto(gpuMoved.data(), stride);
    write_ppm(outDir + "/gpu_moved.ppm", gpuMoved, kWidth, kHeight);

    const int total = kWidth * kHeight;
    const int fg = foreground_pixels(gpuFrame, kWidth, kHeight);
    const int moved = differing_pixels(gpuFrame, gpuMoved, kWidth, kHeight);
    const double parity = mean_abs_rgb_error(gpuFrame, cpuFrame, kWidth, kHeight);
    std::printf("gpu probe: samples=%d foreground=%d/%d moved=%d/%d mean_abs_rgb_error=%.2f avg_frame_ms=%.2f\n",
                renderer.samples(), fg, total, moved, total, parity, avgMs);

    if (fg < total / 50) {  // expect >2% of the frame to be painted geometry
        std::fprintf(stderr, "gpu probe: frame looks blank (foreground=%d)\n", fg);
        return 1;
    }
    if (moved < total / 100) {  // camera move must shift >1% of pixels
        std::fprintf(stderr, "gpu probe: camera move did not change the image (moved=%d)\n", moved);
        return 1;
    }
    if (parity > 90.0) {
        std::fprintf(stderr, "gpu probe: CPU/GPU parity error too high (mean_abs_rgb_error=%.2f)\n",
                     parity);
        return 1;
    }
    if (avgMs > 500.0) {
        std::fprintf(stderr, "gpu probe: software GL frame time too high (avg_frame_ms=%.2f)\n",
                     avgMs);
        return 1;
    }
    std::printf("wl2 3d gpu render ok\n");
    return 0;
}
