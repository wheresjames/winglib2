// Renderer-independent engine model for the wl2:3d core.
//
// Holds the scene graph (nodes, transforms, parenting, materials), attention
// animators, and a tween player. All state is CPU-side and advanced by tick(),
// so it builds and is testable with the Magnum provider `off`. The renderer (a
// renderer is a view over this model; nothing here touches the GPU.
#pragma once

#include "wl2_3d_math.h"
#include "wl2_3d_particles.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wl2::three_d {

struct Color {
    double r = 0.8;
    double g = 0.8;
    double b = 0.8;
    double a = 1.0;
};

struct MeshGeometry {
    std::vector<Vec3> vertices;
    std::vector<uint32_t> indices;
    std::vector<Vec3> normals;
    std::vector<double> uvs;
    bool dynamic = false;
};

// Generate per-vertex smooth normals by accumulating area-weighted face normals
// across shared vertices, then normalizing. Indexed geometry that shares vertices
// (e.g. a grid surface) comes out smooth; this is what lets the CPU rasterizer
// interpolate normals per pixel instead of shading flat per triangle. Called when
// the author supplies no explicit normals, including every frame a dynamic mesh's
// vertices change.
inline void recomputeSmoothNormals(MeshGeometry& mesh) {
    mesh.normals.assign(mesh.vertices.size(), Vec3{0.0, 0.0, 0.0});
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const uint32_t ia = mesh.indices[i];
        const uint32_t ib = mesh.indices[i + 1];
        const uint32_t ic = mesh.indices[i + 2];
        if (ia >= mesh.vertices.size() || ib >= mesh.vertices.size() ||
            ic >= mesh.vertices.size()) {
            continue;
        }
        const Vec3& a = mesh.vertices[ia];
        const Vec3& b = mesh.vertices[ib];
        const Vec3& c = mesh.vertices[ic];
        const Vec3 faceNormal = cross(b - a, c - a);  // magnitude == 2x triangle area
        mesh.normals[ia] = mesh.normals[ia] + faceNormal;
        mesh.normals[ib] = mesh.normals[ib] + faceNormal;
        mesh.normals[ic] = mesh.normals[ic] + faceNormal;
    }
    for (Vec3& n : mesh.normals) {
        const double len = length(n);
        n = len > 1e-12 ? n * (1.0 / len) : Vec3{0.0, 1.0, 0.0};
    }
}

enum class LightKind { Directional, Point };

struct Light {
    int64_t handle = 0;
    std::string id;
    LightKind kind = LightKind::Directional;
    Vec3 position;
    Vec3 direction{-0.5, 1.0, 0.6};
    Color color{1.0, 1.0, 1.0, 1.0};
    double intensity = 1.0;
    double range = 100.0;
    bool visible = true;
};

// Named attention behaviors (§13.3), advanced as derived state so they never
// fight the node's authored transform/material.
enum class AttentionKind { None, Rotate, Pulse, Glow, Ping, Bounce };

struct Attention {
    AttentionKind kind = AttentionKind::None;
    double hz = 1.0;
    Color color;
    bool hasColor = false;
    double phase = 0.0;      // cycles; wraps at 1.0
    double intensity = 0.0;  // derived [0,1] for pulse/glow/ping
    double spin = 0.0;       // derived radians for rotate
    double lift = 0.0;       // derived world-space offset for bounce
    double ring = 0.0;       // derived expansion amount for ping
};

struct Node {
    int64_t handle = 0;
    std::string kind = "node";  // node | asset | primitive | marker | detection
    std::string id;          // author-facing id (picking / upsert)
    std::string label;
    std::string model;       // optional asset reference
    std::string assetUrl;     // loaded wl2:/ resource, for authored content
    size_t resourceSize = 0;
    std::string primitive;    // cube/sphere/... for generated content
    std::map<std::string, double> primitiveOptions;
    std::optional<MeshGeometry> mesh;
    int64_t parent = 0;      // 0 = scene root
    Vec3 pivot;
    Vec3 position;
    Vec3 rotation;           // euler XYZ radians
    Vec3 scale{1.0, 1.0, 1.0};
    Color material;
    bool visible = true;
    bool billboard = false;
    double radius = 0.5;     // bounding sphere for picking
    Vec3 boundsMin{-0.5, -0.5, -0.5};
    Vec3 boundsMax{0.5, 0.5, 0.5};
    Attention attention;
    bool alive = true;
};

struct Overlay {
    int64_t handle = 0;
    std::string id;
    std::string label;
    int64_t anchorNode = 0;
    Vec3 anchorWorld;
    Vec3 offset;
    bool leaderLine = false;
};

struct OverlayState {
    int64_t handle = 0;
    std::string id;
    std::string label;
    Vec3 world;
    ScreenPoint screen;
    bool visible = false;
    bool leaderLine = false;
};

// A planar quad in world space bound to a UI FrameRing (UI-on-3D). uv (0,0) is
// `origin`; uAxis/vAxis span the quad to uv (1,0)/(0,1). vAxis points along the
// UI's downward (increasing-y) direction so uv maps to top-left UI pixels.
struct Surface {
    int64_t handle = 0;
    std::string id;
    std::string ring;       // bound UI FrameRing name
    Vec3 origin;            // world position of uv (0,0)
    Vec3 uAxis{1.0, 0.0, 0.0};
    Vec3 vAxis{0.0, -1.0, 0.0};
    int64_t pixelWidth = 0;
    int64_t pixelHeight = 0;
};

// Result of a surface pick: which surface, the barycentric uv, the derived UI
// pixel (top-left origin), and the world hit point.
struct SurfaceHit {
    int64_t handle = 0;
    double u = 0.0;
    double v = 0.0;
    double pixelX = 0.0;
    double pixelY = 0.0;
    Vec3 point;
};

enum class Ease { Linear, InQuad, OutQuad, InOutQuad, InCubic, OutCubic, InOutCubic };

inline double applyEase(Ease ease, double t) {
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    switch (ease) {
        case Ease::Linear: return t;
        case Ease::InQuad: return t * t;
        case Ease::OutQuad: return 1.0 - (1.0 - t) * (1.0 - t);
        case Ease::InOutQuad:
            return t < 0.5 ? 2.0 * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 2.0) / 2.0;
        case Ease::InCubic: return t * t * t;
        case Ease::OutCubic: return 1.0 - std::pow(1.0 - t, 3.0);
        case Ease::InOutCubic:
            return t < 0.5 ? 4.0 * t * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0;
    }
    return t;
}

inline std::optional<Ease> parseEase(const std::string& name) {
    if (name == "linear") return Ease::Linear;
    if (name == "inQuad") return Ease::InQuad;
    if (name == "outQuad") return Ease::OutQuad;
    if (name == "inOutQuad") return Ease::InOutQuad;
    if (name == "inCubic") return Ease::InCubic;
    if (name == "outCubic") return Ease::OutCubic;
    if (name == "inOutCubic") return Ease::InOutCubic;
    return std::nullopt;
}

// A single interpolation toward target property values, marshalled completion
// reported by handle so the binding layer can invoke the JS callback.
struct Tween {
    int64_t node = 0;
    Ease ease = Ease::OutCubic;
    double durationMs = 200.0;
    double elapsedMs = 0.0;
    bool started = false;

    std::optional<Vec3> toPosition;
    std::optional<Vec3> toScale;
    std::optional<Vec3> toRotation;
    std::optional<double> toOpacity;
    std::optional<Color> toColor;

    Vec3 fromPosition;
    Vec3 fromScale;
    Vec3 fromRotation;
    double fromOpacity = 1.0;
    Color fromColor;

    int64_t callbackId = 0;  // 0 = none
};

class Engine {
public:
    Camera camera;
    Plane ground;  // y = 0 by default
    Color ambientLight{0.28, 0.28, 0.28, 1.0};

    int64_t addNode(Node node) {
        node.handle = nextHandle_++;
        node.alive = true;
        const int64_t handle = node.handle;
        if (!node.id.empty()) {
            byId_[node.id] = handle;
        }
        nodes_.emplace(handle, std::move(node));
        return handle;
    }

    Node* node(int64_t handle) {
        auto it = nodes_.find(handle);
        return it == nodes_.end() ? nullptr : &it->second;
    }
    const Node* node(int64_t handle) const {
        auto it = nodes_.find(handle);
        return it == nodes_.end() ? nullptr : &it->second;
    }

    int64_t findById(const std::string& id) const {
        auto it = byId_.find(id);
        return it == byId_.end() ? 0 : it->second;
    }

    void removeNode(int64_t handle) {
        auto it = nodes_.find(handle);
        if (it == nodes_.end()) {
            return;
        }
        if (!it->second.id.empty()) {
            byId_.erase(it->second.id);
        }
        nodes_.erase(it);
        queues_.erase(handle);
    }

    bool setParent(int64_t child, int64_t parent, bool preserveWorld) {
        Node* node = this->node(child);
        if (!node || child == parent || (parent != 0 && !this->node(parent))) {
            return false;
        }
        const Vec3 world = worldPosition(child);
        node->parent = parent;
        if (preserveWorld) {
            node->position = parent != 0 ? world - worldPosition(parent) : world;
        }
        return true;
    }

    // World-space translation of a node, composing parent transforms.
    Vec3 worldPosition(int64_t handle) const {
        const Node* n = node(handle);
        if (!n) {
            return {};
        }
        Vec3 local = n->position;
        if (n->parent != 0) {
            const Vec3 parent = worldPosition(n->parent);
            return parent + local;
        }
        return local;
    }

    // Queue a tween on a node; tweens on the same node run in sequence.
    void enqueueTween(Tween tween) {
        queues_[tween.node].push_back(std::move(tween));
    }

    // Advance animators and tweens by dtMs; returns completed callback ids in
    // completion order so the caller can invoke them on the JS thread.
    std::vector<int64_t> tick(double dtMs) {
        const double dtSeconds = dtMs / 1000.0;
        std::vector<int64_t> completed;

        for (auto& [handle, node] : nodes_) {
            advanceAttention(node, dtSeconds);
        }

        for (auto& [handle, emitter] : emitters_) {
            emitter.advance(dtSeconds);
        }

        for (auto& [handle, queue] : queues_) {
            Node* target = node(handle);
            if (!target || queue.empty()) {
                continue;
            }
            double remaining = dtMs;
            while (!queue.empty() && remaining > 0.0) {
                Tween& tween = queue.front();
                if (!tween.started) {
                    captureStart(*target, tween);
                    tween.started = true;
                }
                const double step = std::min(remaining, tween.durationMs - tween.elapsedMs);
                tween.elapsedMs += step;
                remaining -= step;
                const double raw = tween.durationMs > 0.0 ? tween.elapsedMs / tween.durationMs : 1.0;
                applyTween(*target, tween, applyEase(tween.ease, raw));
                if (tween.elapsedMs >= tween.durationMs) {
                    if (tween.callbackId != 0) {
                        completed.push_back(tween.callbackId);
                    }
                    queue.pop_front();
                }
            }
        }
        return completed;
    }

    // Nearest visible node hit by a ray (picking / detection), or 0 on a miss.
    int64_t pick(const Ray& ray) const {
        int64_t best = 0;
        double bestT = 0.0;
        for (const auto& [handle, node] : nodes_) {
            if (!node.visible || node.kind == "light") {
                continue;
            }
            const Vec3 center = worldPosition(handle);
            const double scaled = node.radius * std::max({node.scale.x, node.scale.y, node.scale.z});
            auto t = intersect(ray, center, scaled);
            if (t && node.mesh) {
                std::optional<double> meshT;
                for (size_t i = 0; i + 2 < node.mesh->indices.size(); i += 3) {
                    const uint32_t ia = node.mesh->indices[i];
                    const uint32_t ib = node.mesh->indices[i + 1];
                    const uint32_t ic = node.mesh->indices[i + 2];
                    if (ia >= node.mesh->vertices.size() || ib >= node.mesh->vertices.size() ||
                        ic >= node.mesh->vertices.size()) {
                        continue;
                    }
                    const Vec3 a = transformNodeVertex(node, center, node.mesh->vertices[ia]);
                    const Vec3 b = transformNodeVertex(node, center, node.mesh->vertices[ib]);
                    const Vec3 c = transformNodeVertex(node, center, node.mesh->vertices[ic]);
                    if (auto triT = intersectTriangle(ray, a, b, c)) {
                        if (!meshT || *triT < *meshT) {
                            meshT = triT;
                        }
                    }
                }
                t = meshT;
            }
            if (t && (best == 0 || *t < bestT)) {
                best = handle;
                bestT = *t;
            }
        }
        return best;
    }

    static Vec3 rotateEuler(Vec3 v, Vec3 euler) {
        const double cx = std::cos(euler.x), sx = std::sin(euler.x);
        const double cy = std::cos(euler.y), sy = std::sin(euler.y);
        const double cz = std::cos(euler.z), sz = std::sin(euler.z);
        Vec3 out = {v.x, v.y * cx - v.z * sx, v.y * sx + v.z * cx};
        out = {out.x * cy + out.z * sy, out.y, -out.x * sy + out.z * cy};
        out = {out.x * cz - out.y * sz, out.x * sz + out.y * cz, out.z};
        return out;
    }

    static Vec3 transformNodeVertex(const Node& node, Vec3 world, Vec3 local) {
        Vec3 scaled{local.x * node.scale.x, local.y * node.scale.y, local.z * node.scale.z};
        return world + rotateEuler(scaled, node.rotation);
    }

    const std::map<int64_t, Node>& nodes() const { return nodes_; }

    int64_t addLight(Light light) {
        light.handle = nextHandle_++;
        const int64_t handle = light.handle;
        lights_.emplace(handle, std::move(light));
        return handle;
    }

    Light* light(int64_t handle) {
        auto it = lights_.find(handle);
        return it == lights_.end() ? nullptr : &it->second;
    }

    void setLight(int64_t handle, Light light) {
        light.handle = handle;
        lights_[handle] = std::move(light);
    }

    const std::map<int64_t, Light>& lights() const { return lights_; }

    void refreshBounds(Node& node) {
        if (!node.mesh || node.mesh->vertices.empty()) {
            const double r = node.radius;
            node.boundsMin = {-r, -r, -r};
            node.boundsMax = {r, r, r};
            return;
        }
        Vec3 mn = node.mesh->vertices.front();
        Vec3 mx = node.mesh->vertices.front();
        double radius = 0.0;
        for (const Vec3& v : node.mesh->vertices) {
            mn.x = std::min(mn.x, v.x);
            mn.y = std::min(mn.y, v.y);
            mn.z = std::min(mn.z, v.z);
            mx.x = std::max(mx.x, v.x);
            mx.y = std::max(mx.y, v.y);
            mx.z = std::max(mx.z, v.z);
            radius = std::max(radius, length(v));
        }
        node.boundsMin = mn;
        node.boundsMax = mx;
        node.radius = std::max(0.001, radius);
    }

    // --- Overlays ---------------------------------------------------------
    int64_t addOverlay(Overlay overlay) {
        overlay.handle = nextHandle_++;
        const int64_t handle = overlay.handle;
        overlays_.emplace(handle, std::move(overlay));
        return handle;
    }

    Overlay* overlay(int64_t handle) {
        auto it = overlays_.find(handle);
        return it == overlays_.end() ? nullptr : &it->second;
    }

    std::optional<OverlayState> overlayState(int64_t handle, double width, double height) const {
        auto it = overlays_.find(handle);
        if (it == overlays_.end()) {
            return std::nullopt;
        }
        const Overlay& overlay = it->second;
        Vec3 world = overlay.anchorWorld;
        if (overlay.anchorNode != 0 && node(overlay.anchorNode)) {
            world = worldPosition(overlay.anchorNode);
        }
        world = world + overlay.offset;
        const ScreenPoint screen = project(camera, world, width, height);
        return OverlayState{handle, overlay.id, overlay.label, world, screen, screen.onScreen, overlay.leaderLine};
    }

    // --- Particle emitters (effects) -------------------------------------
    int64_t addEmitter(Emitter emitter) {
        emitter.handle = nextHandle_++;
        const int64_t handle = emitter.handle;
        emitters_.emplace(handle, std::move(emitter));
        return handle;
    }

    Emitter* emitter(int64_t handle) {
        auto it = emitters_.find(handle);
        return it == emitters_.end() ? nullptr : &it->second;
    }

    void removeEmitter(int64_t handle) { emitters_.erase(handle); }

    const std::map<int64_t, Emitter>& emitters() const { return emitters_; }

    size_t emitterCount() const { return emitters_.size(); }

    size_t particleCount() const {
        size_t total = 0;
        for (const auto& [handle, emitter] : emitters_) {
            total += emitter.particles.size();
        }
        return total;
    }

    // --- Surfaces (UI-on-3D) ---------------------------------------------
    int64_t addSurface(Surface surface) {
        surface.handle = nextHandle_++;
        const int64_t handle = surface.handle;
        surfaces_.emplace(handle, std::move(surface));
        return handle;
    }

    Surface* surface(int64_t handle) {
        auto it = surfaces_.find(handle);
        return it == surfaces_.end() ? nullptr : &it->second;
    }

    // Nearest surface hit by a ray, with uv and the derived UI pixel. The uv is
    // solved on the (possibly non-rectangular) parallelogram, then mapped to a
    // top-left-origin pixel for input injection.
    std::optional<SurfaceHit> pickSurface(const Ray& ray) const {
        std::optional<SurfaceHit> best;
        double bestT = 0.0;
        for (const auto& [handle, s] : surfaces_) {
            const Vec3 normal = cross(s.uAxis, s.vAxis);
            const auto hit = intersect(ray, Plane{s.origin, normal});
            if (!hit) {
                continue;
            }
            const Vec3 d = *hit - s.origin;
            const double a = dot(s.uAxis, s.uAxis);
            const double b = dot(s.uAxis, s.vAxis);
            const double c = dot(s.vAxis, s.vAxis);
            const double det = a * c - b * b;
            if (std::abs(det) < 1e-12) {
                continue;
            }
            const double du = dot(d, s.uAxis);
            const double dv = dot(d, s.vAxis);
            const double u = (c * du - b * dv) / det;
            const double v = (a * dv - b * du) / det;
            if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0) {
                continue;
            }
            const double t = length(*hit - ray.origin);
            if (!best || t < bestT) {
                best = SurfaceHit{handle, u, v, u * static_cast<double>(s.pixelWidth),
                                  v * static_cast<double>(s.pixelHeight), *hit};
                bestT = t;
            }
        }
        return best;
    }

private:
    void advanceAttention(Node& node, double dtSeconds) {
        Attention& a = node.attention;
        if (a.kind == AttentionKind::None) {
            return;
        }
        a.phase += a.hz * dtSeconds;
        a.phase -= std::floor(a.phase);
        const double wave = 0.5 * (1.0 + std::sin(2.0 * kPi * a.phase));
        switch (a.kind) {
            case AttentionKind::Rotate:
                a.spin = a.phase * 2.0 * kPi;
                a.intensity = wave;
                break;
            case AttentionKind::Pulse:
            case AttentionKind::Glow:
                a.intensity = wave;
                break;
            case AttentionKind::Ping:
                a.intensity = 1.0 - a.phase;
                a.ring = a.phase;
                break;
            case AttentionKind::Bounce:
                a.intensity = wave;
                a.lift = std::abs(std::sin(2.0 * kPi * a.phase));
                break;
            case AttentionKind::None:
                break;
        }
    }

    void captureStart(const Node& node, Tween& tween) {
        tween.fromPosition = node.position;
        tween.fromScale = node.scale;
        tween.fromRotation = node.rotation;
        tween.fromOpacity = node.material.a;
        tween.fromColor = node.material;
    }

    static Vec3 lerp(Vec3 a, Vec3 b, double t) { return a + (b - a) * t; }
    static double lerp(double a, double b, double t) { return a + (b - a) * t; }

    void applyTween(Node& node, const Tween& tween, double t) {
        if (tween.toPosition) node.position = lerp(tween.fromPosition, *tween.toPosition, t);
        if (tween.toScale) node.scale = lerp(tween.fromScale, *tween.toScale, t);
        if (tween.toRotation) node.rotation = lerp(tween.fromRotation, *tween.toRotation, t);
        if (tween.toOpacity) node.material.a = lerp(tween.fromOpacity, *tween.toOpacity, t);
        if (tween.toColor) {
            node.material.r = lerp(tween.fromColor.r, tween.toColor->r, t);
            node.material.g = lerp(tween.fromColor.g, tween.toColor->g, t);
            node.material.b = lerp(tween.fromColor.b, tween.toColor->b, t);
        }
    }

    std::map<int64_t, Node> nodes_;
    std::map<std::string, int64_t> byId_;
    std::map<int64_t, std::deque<Tween>> queues_;
    std::map<int64_t, Surface> surfaces_;
    std::map<int64_t, Emitter> emitters_;
    std::map<int64_t, Overlay> overlays_;
    std::map<int64_t, Light> lights_;
    int64_t nextHandle_ = 1;
};

}  // namespace wl2::three_d
