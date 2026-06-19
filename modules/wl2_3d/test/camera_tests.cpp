// Spike test for the wl2:3d engine core: the unproject/project math that
// picking, detections, and overlays all depend on. Renderer-independent.
#include "../src/wl2_3d_detection.h"
#include "../src/wl2_3d_engine.h"
#include "../src/wl2_3d_math.h"
#include "../src/wl2_3d_platform.h"

#include <cmath>
#include <iostream>
#include <string>

using namespace wl2::three_d;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "camera test failed: " << message << '\n';
        ++failures;
    }
}

void checkNear(double a, double b, double tol, const std::string& message) {
    if (std::abs(a - b) > tol) {
        std::cerr << "camera test failed: " << message << " (" << a << " vs " << b << ")\n";
        ++failures;
    }
}

double distancePointToRay(Vec3 point, const Ray& ray) {
    const Vec3 toPoint = point - ray.origin;
    const double along = dot(toPoint, ray.direction);
    const Vec3 closest = ray.origin + ray.direction * along;
    return length(point - closest);
}

Camera makeCamera() {
    Camera cam;
    cam.eye = {3.0, 10.0, 18.0};
    cam.target = {0.0, 0.0, 0.0};
    cam.up = {0.0, 1.0, 0.0};
    cam.fovYRadians = radians(52.0);
    cam.aspect = 1280.0 / 720.0;
    cam.near = 0.1;
    cam.far = 5000.0;
    return cam;
}

// project then unproject: the original world point must lie on the recovered
// ray. This is the round-trip invariant every 2D<->3D feature relies on.
void testProjectUnprojectRoundTrip() {
    const Camera cam = makeCamera();
    const double width = 1280.0;
    const double height = 720.0;
    const Vec3 world{2.5, 1.0, -4.0};

    const ScreenPoint screen = project(cam, world, width, height);
    check(screen.onScreen, "test point should be on screen");

    const auto ray = unproject(cam, screen.x, screen.y, width, height);
    check(ray.has_value(), "unproject should succeed");
    if (ray) {
        checkNear(distancePointToRay(world, *ray), 0.0, 1e-6, "world point should lie on unprojected ray");
    }
}

// Center pixel of a camera aimed at the origin must unproject to a ray pointing
// from the eye toward the target.
void testCenterRayAimsAtTarget() {
    const Camera cam = makeCamera();
    const auto ray = unproject(cam, 640.0, 360.0, 1280.0, 720.0);
    check(ray.has_value(), "center unproject should succeed");
    if (ray) {
        const Vec3 expected = normalize(cam.target - cam.eye);
        checkNear(ray->direction.x, expected.x, 1e-3, "center ray x");
        checkNear(ray->direction.y, expected.y, 1e-3, "center ray y");
        checkNear(ray->direction.z, expected.z, 1e-3, "center ray z");
    }
}

// Unproject a pixel, intersect the ground plane, project the hit back: the
// screen coordinates must round-trip (the detection -> ground workflow).
void testGroundPlaneRoundTrip() {
    const Camera cam = makeCamera();
    const double width = 1280.0;
    const double height = 720.0;
    const double px = 800.0;
    const double py = 500.0;

    const auto ray = unproject(cam, px, py, width, height);
    check(ray.has_value(), "ground unproject should succeed");
    if (!ray) {
        return;
    }
    const auto hit = intersect(*ray, Plane{});
    check(hit.has_value(), "ray should hit the ground plane");
    if (!hit) {
        return;
    }
    checkNear(hit->y, 0.0, 1e-6, "ground hit should be on y=0");

    const ScreenPoint reprojected = project(cam, *hit, width, height);
    checkNear(reprojected.x, px, 1e-3, "ground hit should reproject to px");
    checkNear(reprojected.y, py, 1e-3, "ground hit should reproject to py");
}

// focal length + sensor height must yield the matching vertical FOV.
void testLensCalibration() {
    Camera cam;
    cam.setLens(50.0, 36.0, 24.0);
    const double expected = 2.0 * std::atan(24.0 / (2.0 * 50.0));
    checkNear(cam.fovYRadians, expected, 1e-9, "focal+sensor should set fovY");
    checkNear(cam.aspect, 36.0 / 24.0, 1e-9, "sensor should set aspect");
}

// Picking: a node placed in the world is hit by the ray through its projected
// pixel, and missed by a ray aimed elsewhere.
void testPicking() {
    Engine engine;
    engine.camera = makeCamera();
    Node node;
    node.id = "target";
    node.position = {1.0, 0.5, -2.0};
    node.radius = 0.6;
    const int64_t handle = engine.addNode(node);

    const ScreenPoint screen = project(engine.camera, node.position, 1280.0, 720.0);
    const auto hitRay = unproject(engine.camera, screen.x, screen.y, 1280.0, 720.0);
    check(hitRay.has_value(), "pick unproject should succeed");
    if (hitRay) {
        check(engine.pick(*hitRay) == handle, "ray through node should pick it");
    }

    const auto missRay = unproject(engine.camera, 10.0, 10.0, 1280.0, 720.0);
    check(missRay.has_value(), "miss unproject should succeed");
    if (missRay) {
        check(engine.pick(*missRay) == 0, "ray aimed at corner should miss the node");
    }
}

// Tween player: animateTo reaches its target after its duration and reports the
// completion callback id; sequenced tweens run in order.
void testTweenPlayer() {
    Engine engine;
    Node node;
    node.scale = {1.0, 1.0, 1.0};
    const int64_t handle = engine.addNode(node);

    Tween grow;
    grow.node = handle;
    grow.toScale = Vec3{1.4, 1.4, 1.4};
    grow.durationMs = 200.0;
    grow.ease = Ease::OutCubic;
    grow.callbackId = 42;
    engine.enqueueTween(grow);

    Tween move;
    move.node = handle;
    move.toPosition = Vec3{0.0, 5.0, 0.0};
    move.durationMs = 100.0;
    move.callbackId = 7;
    engine.enqueueTween(move);

    auto done = engine.tick(200.0);  // finishes grow exactly
    check(done.size() == 1 && done[0] == 42, "grow callback should fire first");
    checkNear(engine.node(handle)->scale.x, 1.4, 1e-6, "scale should reach target");

    done = engine.tick(100.0);  // finishes move
    check(done.size() == 1 && done[0] == 7, "move callback should fire second");
    checkNear(engine.node(handle)->position.y, 5.0, 1e-6, "position should reach target");
}

// Attention animator: ticking advances phase and keeps intensity in [0,1].
void testAttention() {
    Engine engine;
    Node node;
    node.attention.kind = AttentionKind::Pulse;
    node.attention.hz = 2.0;
    const int64_t handle = engine.addNode(node);

    const double before = engine.node(handle)->attention.phase;
    engine.tick(100.0);
    const Attention& a = engine.node(handle)->attention;
    check(a.phase != before, "attention phase should advance");
    check(a.intensity >= 0.0 && a.intensity <= 1.0, "attention intensity in range");
}

// Surface picking: a ray through the center of a bound quad yields uv (0.5,0.5)
// and the matching top-left UI pixel; a ray off the quad misses.
void testSurfacePick() {
    Engine engine;
    engine.camera = makeCamera();
    Surface s;
    s.id = "kiosk";
    s.ring = "/ui/frames";
    s.origin = {-2.0, 4.0, 0.0};   // uv (0,0): top-left
    s.uAxis = {4.0, 0.0, 0.0};     // 4 units wide
    s.vAxis = {0.0, -3.0, 0.0};    // 3 units tall, downward
    s.pixelWidth = 200;
    s.pixelHeight = 100;
    engine.addSurface(s);

    const Vec3 center = s.origin + s.uAxis * 0.5 + s.vAxis * 0.5;
    const ScreenPoint screen = project(engine.camera, center, 1280.0, 720.0);
    const auto ray = unproject(engine.camera, screen.x, screen.y, 1280.0, 720.0);
    check(ray.has_value(), "surface unproject should succeed");
    if (ray) {
        const auto hit = engine.pickSurface(*ray);
        check(hit.has_value(), "ray through surface center should hit");
        if (hit) {
            checkNear(hit->u, 0.5, 1e-4, "surface u should be 0.5");
            checkNear(hit->v, 0.5, 1e-4, "surface v should be 0.5");
            checkNear(hit->pixelX, 100.0, 1e-2, "surface pixelX should be width/2");
            checkNear(hit->pixelY, 50.0, 1e-2, "surface pixelY should be height/2");
        }
    }
    const auto miss = unproject(engine.camera, 5.0, 5.0, 1280.0, 720.0);
    if (miss) {
        check(!engine.pickSurface(*miss).has_value(), "corner ray should miss the surface");
    }
}

// Detection schema: encode/decode round-trips, and a strict decoder that
// rejects truncated, wrong-magic, wrong-version, and garbage payloads.
void testDetectionCodec() {
    Detection d;
    d.cameraId = 7;
    d.id = 42;
    d.sourceFrameSeq = 1234;
    d.ts = 12.5;
    d.px = 640.0;
    d.py = 360.0;
    d.imageWidth = 1280;
    d.imageHeight = 720;
    d.confidence = 0.9f;
    d.coord = DetectionCoord::PixelTopLeft;
    d.hasBbox = true;
    d.bbox[0] = 1.0f;
    d.bbox[2] = 10.0f;
    d.klass = "car";

    const std::string bytes = encodeDetection(d);
    const auto decoded = decodeDetection(bytes);
    check(decoded.has_value(), "valid detection should decode");
    if (decoded) {
        check(decoded->cameraId == 7 && decoded->id == 42, "ids should round-trip");
        check(decoded->sourceFrameSeq == 1234, "sourceFrameSeq should round-trip");
        checkNear(decoded->px, 640.0, 1e-9, "px should round-trip");
        check(decoded->klass == "car", "class should round-trip");
        check(decoded->hasBbox && std::abs(decoded->bbox[2] - 10.0f) < 1e-6, "bbox should round-trip");
    }

    // Truncation at every length must be rejected, never crash.
    for (size_t n = 0; n < bytes.size(); ++n) {
        check(!decodeDetection(bytes.substr(0, n)).has_value(), "truncated payload should be rejected");
    }
    // Wrong magic.
    std::string badMagic = bytes;
    badMagic[0] = 'X';
    check(!decodeDetection(badMagic).has_value(), "wrong magic should be rejected");
    // Garbage of various sizes.
    check(!decodeDetection(std::string(4, '\0')).has_value(), "zero garbage should be rejected");
    check(!decodeDetection(std::string(128, '\xAB')).has_value(), "byte garbage should be rejected");
    check(!decodeDetection("").has_value(), "empty payload should be rejected");

    // Zero image dims must be rejected (avoids divide-by-zero downstream).
    Detection bad = d;
    bad.imageWidth = 0;
    check(!decodeDetection(encodeDetection(bad)).has_value(), "zero imageWidth should be rejected");

    // Coordinate mapping: normalized vs pixel both land at viewport center.
    Detection norm;
    norm.coord = DetectionCoord::NormalizedTopLeft;
    norm.px = 0.5;
    norm.py = 0.5;
    norm.imageWidth = 1;
    norm.imageHeight = 1;
    double vx = 0, vy = 0;
    detectionToViewport(norm, 1280.0, 720.0, vx, vy);
    checkNear(vx, 640.0, 1e-9, "normalized x maps to viewport center");
    checkNear(vy, 360.0, 1e-9, "normalized y maps to viewport center");
}

// Particle system: emission rate, integration, retirement at end of life, and
// color/size interpolation over a particle's life.
void testParticles() {
    Engine engine;
    Emitter e;
    e.position = {0.0, 0.0, 0.0};
    e.velocity = {0.0, 2.0, 0.0};
    e.velocityJitter = {0.0, 0.0, 0.0};  // deterministic
    e.rate = 100.0;       // 100/sec
    e.lifetime = 0.5;     // half a second
    e.sizeStart = 1.0;
    e.sizeEnd = 0.0;
    e.colorStart = ParticleColor{1.0, 0.0, 0.0, 1.0};
    e.colorEnd = ParticleColor{0.0, 0.0, 1.0, 0.0};
    const int64_t handle = engine.addEmitter(e);

    // 0.1s at 100/sec -> ~10 particles emitted, none dead yet.
    engine.tick(100.0);
    const Emitter* live = engine.emitter(handle);
    check(live->particles.size() >= 9 && live->particles.size() <= 11,
          "should emit ~10 particles in 0.1s");
    check(engine.particleCount() == live->particles.size(), "particleCount should total emitters");

    // The oldest particle has risen (velocity.y * time) and aged.
    const Particle& oldest = live->particles.front();
    check(oldest.position.y > 0.0, "particle should rise under +y velocity");
    const double t = live->lifeFraction(oldest);
    check(t > 0.0 && t < 1.0, "oldest particle should be partway through life");
    const ParticleColor c = live->colorAt(t);
    check(c.r < 1.0 && c.b > 0.0, "color should interpolate red->blue");
    check(live->sizeAt(t) < 1.0 && live->sizeAt(t) > 0.0, "size should shrink over life");

    // Advance well past lifetime with emission stopped: all particles retire.
    engine.emitter(handle)->emitting = false;
    engine.tick(1000.0);
    check(engine.emitter(handle)->particles.empty(), "all particles should retire past lifetime");
}

// Portable FrameRing transport (the WASM web-backend fallback): write a frame,
// commit, read it back honoring stride; sequence advances per commit.
void testInMemoryTransport() {
    InMemoryFrameTransport transport;
    FrameDesc desc;
    desc.width = 8;
    desc.height = 4;
    check(transport.create(desc, 3), "transport should create");
    check(transport.desc().stride >= 8 * 4, "stride should be at least width*4");
    check(transport.sequence() == 0, "sequence starts at 0");

    uint8_t* slot = transport.writeSlot();
    check(slot != nullptr, "write slot should be available");
    for (int32_t y = 0; y < desc.height; ++y) {
        uint8_t* row = slot + static_cast<size_t>(y) * transport.desc().stride;
        for (int32_t x = 0; x < desc.width; ++x) {
            row[x * 4 + 0] = static_cast<uint8_t>(x * 16);
            row[x * 4 + 3] = 255;
        }
    }
    transport.commit();
    check(transport.sequence() == 1, "commit advances sequence");

    int64_t seq = 0;
    const uint8_t* read = transport.readSlot(&seq);
    check(read != nullptr && seq == 1, "read slot reflects the committed frame");
    if (read) {
        check(read[7 * 4 + 0] == 7 * 16 && read[3] == 255, "frame bytes round-trip with stride");
    }
}

}  // namespace

int main() {
    testProjectUnprojectRoundTrip();
    testCenterRayAimsAtTarget();
    testGroundPlaneRoundTrip();
    testLensCalibration();
    testPicking();
    testTweenPlayer();
    testAttention();
    testSurfacePick();
    testDetectionCodec();
    testParticles();
    testInMemoryTransport();
    if (failures == 0) {
        std::cout << "wl2_3d camera/engine ok\n";
        return 0;
    }
    std::cerr << failures << " camera/engine assertion(s) failed\n";
    return 1;
}
