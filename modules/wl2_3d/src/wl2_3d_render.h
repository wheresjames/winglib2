// Software scene rasterizer for the wl2:3d FrameRing.
//
// A renderer is a *view* over the engine model (wl2_3d_engine.h); this is the
// CPU view. It draws the live scene — a ground grid, shaded geometry per
// primitive kind (cube/cylinder/cone/arrow/plane as triangle meshes, sphere and
// markers as smooth impostors), and additive particles — into an RGBA8 frame,
// honoring the shared pixel contract (R,G,B,A bytes, premultiplied/opaque,
// top-left origin, explicit stride). It uses exactly the engine camera's
// view/projection, so the rendered image registers with `scene.project(...)`
// overlays pixel-for-pixel.
//
// It has no GPU dependency, so it renders the same scene whether the Magnum
// provider is on or off. (A Magnum GPU mesh pipeline is the future view over the
// same model; until it lands this CPU rasterizer is what reaches the ring.)
#pragma once

#include "wl2_3d_engine.h"
#include "wl2_3d_math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace wl2::three_d {

using Tri = std::array<Vec3, 3>;
using Mesh = std::vector<Tri>;

inline Tri tri(Vec3 a, Vec3 b, Vec3 c) { return Tri{{a, b, c}}; }

// --- Unit meshes (half-extent ~1, centered on the origin) ------------------
// Generated once and reused; the per-node model matrix scales/orients them.

inline const Mesh& cube_mesh() {
    static const Mesh mesh = [] {
        const Vec3 c[8] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                           {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
        const int faces[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
                                 {2, 3, 7, 6}, {1, 2, 6, 5}, {0, 4, 7, 3}};
        Mesh m;
        for (auto& f : faces) {
            m.push_back(tri(c[f[0]], c[f[1]], c[f[2]]));
            m.push_back(tri(c[f[0]], c[f[2]], c[f[3]]));
        }
        return m;
    }();
    return mesh;
}

inline const Mesh& plane_mesh() {
    static const Mesh mesh = {tri({-1, 0, -1}, {-1, 0, 1}, {1, 0, 1}),
                              tri({-1, 0, -1}, {1, 0, 1}, {1, 0, -1})};
    return mesh;
}

// A tube/cone band: ring of radius r0 at y0 to ring of radius r1 at y1, plus end
// caps. r1 == 0 makes a cone apex. Appends to `m`.
inline void add_revolved(Mesh& m, double r0, double y0, double r1, double y1, int slices,
                         bool capLow, bool capHigh) {
    for (int i = 0; i < slices; ++i) {
        const double a0 = (double)i / slices * 2.0 * kPi;
        const double a1 = (double)(i + 1) / slices * 2.0 * kPi;
        const Vec3 lo0{std::cos(a0) * r0, y0, std::sin(a0) * r0};
        const Vec3 lo1{std::cos(a1) * r0, y0, std::sin(a1) * r0};
        const Vec3 hi0{std::cos(a0) * r1, y1, std::sin(a0) * r1};
        const Vec3 hi1{std::cos(a1) * r1, y1, std::sin(a1) * r1};
        if (r1 == 0.0) {
            m.push_back(tri(lo0, lo1, {0, y1, 0}));  // cone side to apex
        } else {
            m.push_back(tri(lo0, lo1, hi1));
            m.push_back(tri(lo0, hi1, hi0));
        }
        if (capLow && r0 > 0.0) {
            m.push_back(tri({0, y0, 0}, lo1, lo0));
        }
        if (capHigh && r1 > 0.0) {
            m.push_back(tri({0, y1, 0}, hi0, hi1));
        }
    }
}

inline const Mesh& cylinder_mesh() {
    static const Mesh mesh = [] {
        Mesh m;
        add_revolved(m, 1.0, -1.0, 1.0, 1.0, 20, true, true);
        return m;
    }();
    return mesh;
}

inline const Mesh& cone_mesh() {
    static const Mesh mesh = [] {
        Mesh m;
        add_revolved(m, 1.0, -1.0, 0.0, 1.0, 20, true, false);
        return m;
    }();
    return mesh;
}

inline const Mesh& arrow_mesh() {
    static const Mesh mesh = [] {
        Mesh m;
        add_revolved(m, 0.16, -1.0, 0.16, 0.25, 16, true, true);  // shaft
        add_revolved(m, 0.45, 0.25, 0.0, 1.0, 16, true, false);   // head
        return m;
    }();
    return mesh;
}

// --- Matrix helpers --------------------------------------------------------
inline Mat4 translation_matrix(Vec3 t) {
    Mat4 m = Mat4::identity();
    m.at(0, 3) = t.x;
    m.at(1, 3) = t.y;
    m.at(2, 3) = t.z;
    return m;
}
inline Mat4 scale_matrix(Vec3 s) {
    Mat4 m = Mat4::identity();
    m.at(0, 0) = s.x;
    m.at(1, 1) = s.y;
    m.at(2, 2) = s.z;
    return m;
}
inline Mat4 rotation_matrix(Vec3 euler) {
    const double cx = std::cos(euler.x), sx = std::sin(euler.x);
    const double cy = std::cos(euler.y), sy = std::sin(euler.y);
    const double cz = std::cos(euler.z), sz = std::sin(euler.z);
    Mat4 rx = Mat4::identity();
    rx.at(1, 1) = cx; rx.at(1, 2) = -sx; rx.at(2, 1) = sx; rx.at(2, 2) = cx;
    Mat4 ry = Mat4::identity();
    ry.at(0, 0) = cy; ry.at(0, 2) = sy; ry.at(2, 0) = -sy; ry.at(2, 2) = cy;
    Mat4 rz = Mat4::identity();
    rz.at(0, 0) = cz; rz.at(0, 1) = -sz; rz.at(1, 0) = sz; rz.at(1, 1) = cz;
    return ry * rx * rz;
}

inline void render_scene_cpu(const Engine& engine, unsigned char* data, int width, int height,
                             int stride) {
    if (!data || width <= 0 || height <= 0 || stride < width * 4) {
        return;
    }

    const auto toByte = [](double v) -> unsigned char {
        v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
        return static_cast<unsigned char>(v * 255.0 + 0.5);
    };
    const auto pixelAt = [&](int x, int y) -> unsigned char* {
        return data + static_cast<size_t>(y) * static_cast<size_t>(stride) +
               static_cast<size_t>(x) * 4;
    };

    // --- Background: a vertical sky gradient so empty scenes are never blank. -
    const double topR = 0.043, topG = 0.063, topB = 0.098;
    const double botR = 0.094, botG = 0.137, botB = 0.196;
    for (int y = 0; y < height; ++y) {
        const double t = height > 1 ? static_cast<double>(y) / (height - 1) : 0.0;
        const unsigned char r = toByte(topR + (botR - topR) * t);
        const unsigned char g = toByte(topG + (botG - topG) * t);
        const unsigned char b = toByte(topB + (botB - topB) * t);
        unsigned char* row = pixelAt(0, y);
        for (int x = 0; x < width; ++x) {
            row[x * 4 + 0] = r;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = b;
            row[x * 4 + 3] = 255;
        }
    }

    // Per-pixel depth (camera-space distance; smaller is nearer).
    std::vector<double> depth(static_cast<size_t>(width) * static_cast<size_t>(height), 1e30);

    const Camera& cam = engine.camera;
    const Mat4 vp = cam.viewProjection();
    const Mat4 view = cam.view();
    const double focalPx = (height * 0.5) / std::tan(cam.fovYRadians * 0.5);
    const Vec3 lightScreen = normalize(Vec3{-0.4, 0.65, 0.65});  // for impostors (+y up)

    // Color management: shade in linear light, then encode to sRGB for display.
    // Material/light colors arrive as sRGB-encoded [0,1], so linearize them before
    // they enter the lighting sum and gamma-encode the result on the way out.
    const auto toLinear = [](double c) {
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    const auto toSrgb = [](double c) {
        c = c < 0.0 ? 0.0 : c;
        return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    };

    // Blinn-Phong over the engine lights, in linear space. `lr/lg/lb` is the
    // linearized base color; the returned Color is sRGB-encoded and ready for
    // toByte. A fixed specular term stands in until materials carry roughness.
    constexpr double kSpecStrength = 0.3;
    constexpr double kShininess = 32.0;
    const auto shadeLit = [&](Vec3 normal, Vec3 position, double lr, double lg, double lb,
                              double boost) -> Color {
        const Vec3 viewDir = normalize(cam.eye - position);
        if (dot(normal, viewDir) < 0.0) {
            normal = normal * -1.0;  // two-sided
        }
        double r = lr * toLinear(engine.ambientLight.r);
        double g = lg * toLinear(engine.ambientLight.g);
        double b = lb * toLinear(engine.ambientLight.b);
        const auto accumulate = [&](Vec3 toLight, double lcr, double lcg, double lcb,
                                    double intensity) {
            const double ndl = dot(normal, toLight);
            if (ndl <= 0.0) {
                return;
            }
            const double diffuse = ndl * intensity;
            r += lr * lcr * diffuse;
            g += lg * lcg * diffuse;
            b += lb * lcb * diffuse;
            const Vec3 halfway = normalize(toLight + viewDir);
            const double spec = std::pow(std::max(0.0, dot(normal, halfway)), kShininess) *
                                kSpecStrength * intensity;
            r += lcr * spec;
            g += lcg * spec;
            b += lcb * spec;
        };
        if (engine.lights().empty()) {
            accumulate(normalize(Vec3{-0.5, 1.0, 0.6}), 1.0, 1.0, 1.0, 0.72);
        } else {
            for (const auto& [handle, light] : engine.lights()) {
                if (!light.visible || light.intensity <= 0.0) {
                    continue;
                }
                Vec3 toLight;
                double attenuation = 1.0;
                if (light.kind == LightKind::Point) {
                    toLight = light.position - position;
                    const double d = length(toLight);
                    if (d <= 1e-6 || d > light.range) {
                        continue;
                    }
                    toLight = toLight * (1.0 / d);
                    attenuation = 1.0 - std::min(1.0, d / std::max(1e-6, light.range));
                    attenuation *= attenuation;
                } else {
                    toLight = normalize(light.direction);
                }
                accumulate(toLight, toLinear(light.color.r), toLinear(light.color.g),
                           toLinear(light.color.b), light.intensity * attenuation);
            }
        }
        return Color{toSrgb(r + boost), toSrgb(g + boost), toSrgb(b + boost), 1.0};
    };

    struct Proj {
        double x = 0.0, y = 0.0, dist = 0.0;
        bool front = false;
    };
    const auto projectPoint = [&](Vec3 world) -> Proj {
        const auto clip = transform(vp, world, 1.0);
        Proj p;
        if (clip[3] <= 1e-6) {
            return p;
        }
        const double inv = 1.0 / clip[3];
        p.x = (clip[0] * inv * 0.5 + 0.5) * width;
        p.y = (0.5 - clip[1] * inv * 0.5) * height;
        const auto v = transform(view, world, 1.0);
        p.dist = -v[2];
        p.front = p.dist > 1e-4;
        return p;
    };

    const auto setDepthPixel = [&](int px, int py, double dist, double r, double g, double b) {
        if (px < 0 || py < 0 || px >= width || py >= height) {
            return;
        }
        double& zb = depth[static_cast<size_t>(py) * width + px];
        if (dist < zb) {
            zb = dist;
            unsigned char* q = pixelAt(px, py);
            q[0] = toByte(r);
            q[1] = toByte(g);
            q[2] = toByte(b);
            q[3] = 255;
        }
    };

    // --- Ground grid -------------------------------------------------------
    const auto drawSegment = [&](Vec3 a, Vec3 b, double r, double g, double bl) {
        const double worldLen = length(b - a);
        const int steps = std::max(2, std::min(256, static_cast<int>(worldLen * 6.0)));
        Proj prev = projectPoint(a);
        for (int i = 1; i < steps; ++i) {
            const double t = static_cast<double>(i) / (steps - 1);
            const Proj cur = projectPoint(a + (b - a) * t);
            if (prev.front && cur.front) {
                const double dx = cur.x - prev.x, dy = cur.y - prev.y;
                const int n = std::max(1, static_cast<int>(std::max(std::abs(dx), std::abs(dy))));
                for (int k = 0; k <= n; ++k) {
                    const double f = static_cast<double>(k) / n;
                    setDepthPixel(static_cast<int>(prev.x + dx * f + 0.5),
                                  static_cast<int>(prev.y + dy * f + 0.5),
                                  prev.dist + (cur.dist - prev.dist) * f, r, g, bl);
                }
            }
            prev = cur;
        }
    };
    for (const auto& [handle, node] : engine.nodes()) {
        if (!node.visible || node.primitive != "grid") {
            continue;
        }
        auto opt = [&](const char* key, double fallback) {
            auto it = node.primitiveOptions.find(key);
            return it == node.primitiveOptions.end() ? fallback : it->second;
        };
        const double size = opt("size", 20.0);
        const int divisions = std::max(1, static_cast<int>(opt("divisions", 10.0)));
        const double half = size * 0.5, step = size / divisions;
        const double gr = node.material.r * 0.7, gg = node.material.g * 0.7, gb = node.material.b * 0.7;
        for (int i = 0; i <= divisions; ++i) {
            const double c = -half + i * step;
            drawSegment({c, 0.0, -half}, {c, 0.0, half}, gr, gg, gb);
            drawSegment({-half, 0.0, c}, {half, 0.0, c}, gr, gg, gb);
        }
    }

    // --- Triangle mesh rasterizer (flat-shaded, two-sided) -----------------
    const auto drawMesh = [&](const Mesh& mesh, const Mat4& model, double cr, double cg, double cb,
                              double boost) {
        const double lr = toLinear(cr), lg = toLinear(cg), lb = toLinear(cb);
        for (const auto& tri : mesh) {
            const auto a4 = transform(model, tri[0], 1.0);
            const auto b4 = transform(model, tri[1], 1.0);
            const auto c4 = transform(model, tri[2], 1.0);
            const Vec3 wa{a4[0], a4[1], a4[2]}, wb{b4[0], b4[1], b4[2]}, wc{c4[0], c4[1], c4[2]};
            Vec3 normal = normalize(cross(wb - wa, wc - wa));
            const Vec3 center = (wa + wb + wc) * (1.0 / 3.0);
            const Color shaded = shadeLit(normal, center, lr, lg, lb, boost);

            const Proj pa = projectPoint(wa), pb = projectPoint(wb), pc = projectPoint(wc);
            if (!pa.front || !pb.front || !pc.front) {
                continue;  // skip triangles crossing behind the camera
            }
            const double minX = std::min({pa.x, pb.x, pc.x}), maxX = std::max({pa.x, pb.x, pc.x});
            const double minY = std::min({pa.y, pb.y, pc.y}), maxY = std::max({pa.y, pb.y, pc.y});
            const int x0 = std::max(0, static_cast<int>(std::floor(minX)));
            const int x1 = std::min(width - 1, static_cast<int>(std::ceil(maxX)));
            const int y0 = std::max(0, static_cast<int>(std::floor(minY)));
            const int y1 = std::min(height - 1, static_cast<int>(std::ceil(maxY)));
            const double area = (pb.x - pa.x) * (pc.y - pa.y) - (pb.y - pa.y) * (pc.x - pa.x);
            if (std::abs(area) < 1e-9) {
                continue;
            }
            const double invArea = 1.0 / area;
            for (int py = y0; py <= y1; ++py) {
                for (int px = x0; px <= x1; ++px) {
                    const double fx = px + 0.5, fy = py + 0.5;
                    double w0 = ((pb.x - fx) * (pc.y - fy) - (pb.y - fy) * (pc.x - fx)) * invArea;
                    double w1 = ((pc.x - fx) * (pa.y - fy) - (pc.y - fy) * (pa.x - fx)) * invArea;
                    double w2 = 1.0 - w0 - w1;
                    if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) {
                        continue;
                    }
                    const double dist = w0 * pa.dist + w1 * pb.dist + w2 * pc.dist;
                    setDepthPixel(px, py, dist, shaded.r, shaded.g, shaded.b);
                }
            }
        }
    };
    const auto drawIndexedMesh = [&](const MeshGeometry& mesh, const Mat4& model, double cr, double cg,
                                     double cb, double boost) {
        const double lr = toLinear(cr), lg = toLinear(cg), lb = toLinear(cb);
        // Smooth shading when the mesh carries per-vertex normals: interpolate the
        // normal across the triangle and shade per pixel. Otherwise fall back to a
        // single flat face normal so degenerate/normal-less meshes still draw.
        const bool smooth = mesh.normals.size() == mesh.vertices.size();
        const auto worldNormal = [&](uint32_t idx) {
            const auto n4 = transform(model, mesh.normals[idx], 0.0);  // direction; ignore translation
            return normalize(Vec3{n4[0], n4[1], n4[2]});
        };
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const uint32_t ia = mesh.indices[i];
            const uint32_t ib = mesh.indices[i + 1];
            const uint32_t ic = mesh.indices[i + 2];
            if (ia >= mesh.vertices.size() || ib >= mesh.vertices.size() || ic >= mesh.vertices.size()) {
                continue;
            }
            const auto a4 = transform(model, mesh.vertices[ia], 1.0);
            const auto b4 = transform(model, mesh.vertices[ib], 1.0);
            const auto c4 = transform(model, mesh.vertices[ic], 1.0);
            const Vec3 wa{a4[0], a4[1], a4[2]}, wb{b4[0], b4[1], b4[2]}, wc{c4[0], c4[1], c4[2]};
            const Vec3 faceNormal = normalize(cross(wb - wa, wc - wa));
            const Vec3 na = smooth ? worldNormal(ia) : faceNormal;
            const Vec3 nb = smooth ? worldNormal(ib) : faceNormal;
            const Vec3 nc = smooth ? worldNormal(ic) : faceNormal;
            const Vec3 center = (wa + wb + wc) * (1.0 / 3.0);
            const Color flat = smooth ? Color{} : shadeLit(faceNormal, center, lr, lg, lb, boost);

            const Proj pa = projectPoint(wa), pb = projectPoint(wb), pc = projectPoint(wc);
            if (!pa.front || !pb.front || !pc.front) {
                continue;
            }
            const double minX = std::min({pa.x, pb.x, pc.x}), maxX = std::max({pa.x, pb.x, pc.x});
            const double minY = std::min({pa.y, pb.y, pc.y}), maxY = std::max({pa.y, pb.y, pc.y});
            const int x0 = std::max(0, static_cast<int>(std::floor(minX)));
            const int x1 = std::min(width - 1, static_cast<int>(std::ceil(maxX)));
            const int y0 = std::max(0, static_cast<int>(std::floor(minY)));
            const int y1 = std::min(height - 1, static_cast<int>(std::ceil(maxY)));
            const double area = (pb.x - pa.x) * (pc.y - pa.y) - (pb.y - pa.y) * (pc.x - pa.x);
            if (std::abs(area) < 1e-9) {
                continue;
            }
            const double invArea = 1.0 / area;
            for (int py = y0; py <= y1; ++py) {
                for (int px = x0; px <= x1; ++px) {
                    const double fx = px + 0.5, fy = py + 0.5;
                    double w0 = ((pb.x - fx) * (pc.y - fy) - (pb.y - fy) * (pc.x - fx)) * invArea;
                    double w1 = ((pc.x - fx) * (pa.y - fy) - (pc.y - fy) * (pa.x - fx)) * invArea;
                    double w2 = 1.0 - w0 - w1;
                    if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) {
                        continue;
                    }
                    const double dist = w0 * pa.dist + w1 * pb.dist + w2 * pc.dist;
                    if (smooth) {
                        const Vec3 n = normalize(na * w0 + nb * w1 + nc * w2);
                        const Vec3 pos = wa * w0 + wb * w1 + wc * w2;
                        const Color sh = shadeLit(n, pos, lr, lg, lb, boost);
                        setDepthPixel(px, py, dist, sh.r, sh.g, sh.b);
                    } else {
                        setDepthPixel(px, py, dist, flat.r, flat.g, flat.b);
                    }
                }
            }
        }
    };

    // --- Smooth sphere impostor --------------------------------------------
    const auto drawSphere = [&](Vec3 world, double worldRadius, double cr, double cg, double cb,
                                double boost) {
        const Proj p = projectPoint(world);
        if (!p.front) {
            return;
        }
        double screenRadius = worldRadius * focalPx / p.dist;
        if (screenRadius < 1.5) screenRadius = 1.5;
        if (screenRadius > height) screenRadius = height;
        const int cx = static_cast<int>(p.x + 0.5), cy = static_cast<int>(p.y + 0.5);
        const int r0 = std::max(0, static_cast<int>(cy - screenRadius));
        const int r1 = std::min(height - 1, static_cast<int>(cy + screenRadius));
        const int c0 = std::max(0, static_cast<int>(cx - screenRadius));
        const int c1 = std::min(width - 1, static_cast<int>(cx + screenRadius));
        const double invR = 1.0 / screenRadius;
        for (int py = r0; py <= r1; ++py) {
            for (int px = c0; px <= c1; ++px) {
                const double nx = (px - cx) * invR, ny = (py - cy) * invR;
                const double d2 = nx * nx + ny * ny;
                if (d2 > 1.0) continue;
                const double nz = std::sqrt(1.0 - d2);
                double lambert = nx * lightScreen.x + (-ny) * lightScreen.y + nz * lightScreen.z;
                if (lambert < 0.0) lambert = 0.0;
                const double rim = std::pow(1.0 - nz, 3.0) * 0.25;
                const double shade = 0.22 + 0.78 * lambert + boost + rim;
                setDepthPixel(px, py, p.dist - nz * worldRadius, cr * shade, cg * shade, cb * shade);
            }
        }
    };

    // --- Nodes -------------------------------------------------------------
    for (const auto& [handle, node] : engine.nodes()) {
        if (!node.visible || node.kind == "light" || node.primitive == "grid" || node.primitive == "skydome") {
            continue;
        }
        Vec3 world = engine.worldPosition(handle);
        world.y += node.attention.lift * 0.35;  // bounce cue

        const Attention& att = node.attention;
        double boost = 0.0;
        if (att.kind == AttentionKind::Pulse || att.kind == AttentionKind::Glow ||
            att.kind == AttentionKind::Ping || att.kind == AttentionKind::Bounce) {
            boost = 0.6 * att.intensity;
        } else if (att.kind == AttentionKind::Rotate) {
            boost = 0.2;
        }
        const double cr = node.material.r, cg = node.material.g, cb = node.material.b;
        const double maxScale = std::max({node.scale.x, node.scale.y, node.scale.z});
        const double worldRadius = std::max(0.05, node.radius * maxScale);

        // Pick geometry by primitive kind; spheres/markers/detections stay smooth
        // impostors, authored geometry uses meshes.
        const std::string& k = node.primitive;
        const Mesh* mesh = nullptr;
        if (k == "cube") mesh = &cube_mesh();
        else if (k == "cylinder") mesh = &cylinder_mesh();
        else if (k == "cone") mesh = &cone_mesh();
        else if (k == "arrow") mesh = &arrow_mesh();
        else if (k == "plane") mesh = &plane_mesh();
        else if (node.kind == "asset") mesh = &cube_mesh();

        if (node.mesh) {
            Vec3 spin = node.rotation;
            if (att.kind == AttentionKind::Rotate) {
                spin.y += att.spin;
            }
            const Mat4 model = translation_matrix(world) * rotation_matrix(spin) * scale_matrix(node.scale);
            drawIndexedMesh(*node.mesh, model, cr, cg, cb, boost);
        } else if (mesh) {
            Vec3 spin = node.rotation;
            if (att.kind == AttentionKind::Rotate) {
                spin.y += att.spin;
            }
            const Mat4 model = translation_matrix(world) * rotation_matrix(spin) *
                               scale_matrix({node.radius * node.scale.x, node.radius * node.scale.y,
                                             node.radius * node.scale.z});
            drawMesh(*mesh, model, cr, cg, cb, boost);
        } else {
            drawSphere(world, worldRadius, cr, cg, cb, boost);
        }
    }

    // --- Particles: additive sprites, depth-tested but not depth-writing ----
    for (const auto& [handle, emitter] : engine.emitters()) {
        for (const auto& particle : emitter.particles) {
            const double t = emitter.lifeFraction(particle);
            const ParticleColor col = emitter.colorAt(t);
            const double size = std::max(0.0, emitter.sizeAt(t));
            const Proj p = projectPoint(particle.position);
            if (!p.front || col.a <= 0.0) {
                continue;
            }
            double screenRadius = std::max(1.0, size * focalPx / p.dist * 0.5);
            if (screenRadius > 64.0) screenRadius = 64.0;
            const int cx = static_cast<int>(p.x + 0.5), cy = static_cast<int>(p.y + 0.5);
            const int r0 = std::max(0, static_cast<int>(cy - screenRadius));
            const int r1 = std::min(height - 1, static_cast<int>(cy + screenRadius));
            const int c0 = std::max(0, static_cast<int>(cx - screenRadius));
            const int c1 = std::min(width - 1, static_cast<int>(cx + screenRadius));
            const double invR = 1.0 / screenRadius;
            for (int py = r0; py <= r1; ++py) {
                for (int px = c0; px <= c1; ++px) {
                    const double nx = (px - cx) * invR, ny = (py - cy) * invR;
                    const double d2 = nx * nx + ny * ny;
                    if (d2 > 1.0) continue;
                    if (p.dist >= depth[static_cast<size_t>(py) * width + px]) continue;
                    const double falloff = (1.0 - std::sqrt(d2)) * col.a;
                    unsigned char* q = pixelAt(px, py);
                    q[0] = toByte(q[0] / 255.0 + col.r * falloff);
                    q[1] = toByte(q[1] / 255.0 + col.g * falloff);
                    q[2] = toByte(q[2] / 255.0 + col.b * falloff);
                    q[3] = 255;
                }
            }
        }
    }
}

}  // namespace wl2::three_d
