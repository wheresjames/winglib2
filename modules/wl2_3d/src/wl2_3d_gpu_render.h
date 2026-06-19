// GPU scene renderer for the wl2:3d engine model (Magnum / desktop GL).
//
// A second *view* over the same engine model as the CPU rasterizer
// (wl2_3d_render.h): it draws the scene into an offscreen, multisampled,
// sRGB framebuffer and reads the result back into an RGBA8 buffer that matches
// the FrameRing pixel contract (RGBA8, sRGB, top-left origin, explicit stride).
// It does not own the GL context — the caller makes a context current first
// (the headless probe in test/, or the scene's writer). Geometry is rebuilt per
// render() call here; buffer caching / dynamic re-upload is future optimization
// work. The GPU view covers indexed meshes plus the ground grid; sphere impostors
// and additive particles are drawn by the CPU view until added here.
//
// The whole file compiles to nothing unless WL2_3D_HAVE_MAGNUM is set, so it is
// safe to include unconditionally.
#pragma once

#include "wl2_3d_render.h"  // engine model + shared matrix helpers (translation/rotation/scale)

#if WL2_3D_HAVE_MAGNUM

#include <Corrade/Containers/ArrayView.h>
#include <Corrade/Containers/Reference.h>
#include <Magnum/Image.h>
#include <Magnum/PixelFormat.h>
#include <Magnum/Math/Color.h>
#include <Magnum/Math/Matrix3.h>
#include <Magnum/Math/Matrix4.h>
#include <Magnum/GL/AbstractShaderProgram.h>
#include <Magnum/GL/Attribute.h>
#include <Magnum/GL/Buffer.h>
#include <Magnum/GL/Framebuffer.h>
#include <Magnum/GL/Mesh.h>
#include <Magnum/GL/Renderbuffer.h>
#include <Magnum/GL/RenderbufferFormat.h>
#include <Magnum/GL/Renderer.h>
#include <Magnum/GL/Shader.h>
#include <Magnum/GL/Version.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef WL2_3D_GPU_SAMPLES
#define WL2_3D_GPU_SAMPLES 4
#endif

namespace wl2::three_d {

namespace MnGL = Magnum::GL;
using Magnum::Color4;
using Magnum::Float;
using Magnum::Int;
using Magnum::Matrix3;
using Magnum::Matrix4;
using Magnum::Range2Di;
using Magnum::UnsignedInt;
using Magnum::Vector2i;
using Magnum::Vector3;
using Magnum::Vector4;

// sRGB-encoded [0,1] -> linear, matching the CPU renderer's color management so
// GPU and CPU output land close under A/B comparison.
inline Float gpu_srgb_to_linear(double c) {
    return static_cast<Float>(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
}

inline Matrix4 gpu_to_matrix4(const Mat4& m) {
    const auto& d = m.m;  // column-major, same layout as Magnum
    return Matrix4{Vector4{(Float)d[0], (Float)d[1], (Float)d[2], (Float)d[3]},
                   Vector4{(Float)d[4], (Float)d[5], (Float)d[6], (Float)d[7]},
                   Vector4{(Float)d[8], (Float)d[9], (Float)d[10], (Float)d[11]},
                   Vector4{(Float)d[12], (Float)d[13], (Float)d[14], (Float)d[15]}};
}

// Lit/unlit shader: the GLSL port of shadeSurface/shadeLit from the CPU view
// (linear-light ambient + per-light Blinn-Phong, two-sided, fixed specular).
class SurfaceShader : public MnGL::AbstractShaderProgram {
public:
    typedef MnGL::Attribute<0, Vector3> Position;
    typedef MnGL::Attribute<1, Vector3> Normal;
    static constexpr int MaxLights = 8;

    explicit SurfaceShader() {
        MnGL::Shader vert{MnGL::Version::GL330, MnGL::Shader::Type::Vertex};
        MnGL::Shader frag{MnGL::Version::GL330, MnGL::Shader::Type::Fragment};
        vert.addSource(vertexSource());
        frag.addSource(fragmentSource());
        if (!MnGL::Shader::compile({vert, frag})) {
            throw std::runtime_error("wl2_3d GPU shader compile failed");
        }
        attachShaders({vert, frag});
        if (!link()) {
            throw std::runtime_error("wl2_3d GPU shader link failed");
        }

        uViewProjection_ = uniformLocation("uViewProjection");
        uModel_ = uniformLocation("uModel");
        uNormalMatrix_ = uniformLocation("uNormalMatrix");
        uBaseColorLin_ = uniformLocation("uBaseColorLin");
        uAmbientLin_ = uniformLocation("uAmbientLin");
        uCameraEye_ = uniformLocation("uCameraEye");
        uBoost_ = uniformLocation("uBoost");
        uUnlit_ = uniformLocation("uUnlit");
        uLightCount_ = uniformLocation("uLightCount");
        uLightVec_ = uniformLocation("uLightVec");
        uLightColorLin_ = uniformLocation("uLightColorLin");
        uLightIntensity_ = uniformLocation("uLightIntensity");
        uLightIsPoint_ = uniformLocation("uLightIsPoint");
        uLightRange_ = uniformLocation("uLightRange");
    }

    SurfaceShader& setViewProjection(const Matrix4& m) { setUniform(uViewProjection_, m); return *this; }
    SurfaceShader& setModel(const Matrix4& m) { setUniform(uModel_, m); return *this; }
    SurfaceShader& setNormalMatrix(const Matrix3& m) { setUniform(uNormalMatrix_, m); return *this; }
    SurfaceShader& setBaseColorLin(const Vector3& c) { setUniform(uBaseColorLin_, c); return *this; }
    SurfaceShader& setAmbientLin(const Vector3& c) { setUniform(uAmbientLin_, c); return *this; }
    SurfaceShader& setCameraEye(const Vector3& e) { setUniform(uCameraEye_, e); return *this; }
    SurfaceShader& setBoost(Float b) { setUniform(uBoost_, b); return *this; }
    SurfaceShader& setUnlit(bool u) { setUniform(uUnlit_, u ? 1 : 0); return *this; }

    SurfaceShader& setLights(const std::vector<Vector3>& vec, const std::vector<Vector3>& colorLin,
                             const std::vector<Float>& intensity, const std::vector<Int>& isPoint,
                             const std::vector<Float>& range) {
        const int count = static_cast<int>(vec.size());
        setUniform(uLightCount_, count);
        if (count > 0) {
            setUniform(uLightVec_, Corrade::Containers::arrayView(vec.data(), vec.size()));
            setUniform(uLightColorLin_, Corrade::Containers::arrayView(colorLin.data(), colorLin.size()));
            setUniform(uLightIntensity_, Corrade::Containers::arrayView(intensity.data(), intensity.size()));
            setUniform(uLightIsPoint_, Corrade::Containers::arrayView(isPoint.data(), isPoint.size()));
            setUniform(uLightRange_, Corrade::Containers::arrayView(range.data(), range.size()));
        }
        return *this;
    }

private:
    // Magnum's Shader{Version::GL330, ...} prepends the #version directive, so
    // the sources below must not repeat it.
    static std::string vertexSource() {
        return
            "layout(location=0) in vec3 aPosition;\n"
            "layout(location=1) in vec3 aNormal;\n"
            "uniform mat4 uViewProjection;\n"
            "uniform mat4 uModel;\n"
            "uniform mat3 uNormalMatrix;\n"
            "out vec3 vWorldPos;\n"
            "out vec3 vNormal;\n"
            "void main() {\n"
            "    vec4 wp = uModel * vec4(aPosition, 1.0);\n"
            "    vWorldPos = wp.xyz;\n"
            "    vNormal = uNormalMatrix * aNormal;\n"
            "    gl_Position = uViewProjection * wp;\n"
            "}\n";
    }
    static std::string fragmentSource() {
        return
            "#define MAX_LIGHTS 8\n"
            "in vec3 vWorldPos;\n"
            "in vec3 vNormal;\n"
            "uniform vec3 uBaseColorLin;\n"
            "uniform vec3 uAmbientLin;\n"
            "uniform vec3 uCameraEye;\n"
            "uniform float uBoost;\n"
            "uniform int uUnlit;\n"
            "uniform int uLightCount;\n"
            "uniform vec3 uLightVec[MAX_LIGHTS];\n"
            "uniform vec3 uLightColorLin[MAX_LIGHTS];\n"
            "uniform float uLightIntensity[MAX_LIGHTS];\n"
            "uniform int uLightIsPoint[MAX_LIGHTS];\n"
            "uniform float uLightRange[MAX_LIGHTS];\n"
            "out vec4 fragColor;\n"
            "const float kSpecStrength = 0.3;\n"
            "const float kShininess = 32.0;\n"
            "void main() {\n"
            "    if(uUnlit == 1) { fragColor = vec4(uBaseColorLin, 1.0); return; }\n"
            "    vec3 viewDir = normalize(uCameraEye - vWorldPos);\n"
            "    vec3 n = normalize(vNormal);\n"
            "    if(dot(n, viewDir) < 0.0) n = -n;\n"
            "    vec3 col = uBaseColorLin * uAmbientLin;\n"
            "    for(int i = 0; i < uLightCount; ++i) {\n"
            "        vec3 toLight = vec3(0.0); float atten = 1.0;\n"
            "        if(uLightIsPoint[i] == 1) {\n"
            "            vec3 d = uLightVec[i] - vWorldPos;\n"
            "            float dist = length(d);\n"
            "            if(dist <= 1e-6 || dist > uLightRange[i]) continue;\n"
            "            toLight = d / dist;\n"
            "            atten = 1.0 - min(1.0, dist / max(1e-6, uLightRange[i]));\n"
            "            atten *= atten;\n"
            "        } else {\n"
            "            toLight = normalize(uLightVec[i]);\n"
            "        }\n"
            "        float ndl = dot(n, toLight);\n"
            "        if(ndl <= 0.0) continue;\n"
            "        float diff = ndl * uLightIntensity[i] * atten;\n"
            "        col += uBaseColorLin * uLightColorLin[i] * diff;\n"
            "        vec3 h = normalize(toLight + viewDir);\n"
            "        float spec = pow(max(0.0, dot(n, h)), kShininess) * kSpecStrength * uLightIntensity[i] * atten;\n"
            "        col += uLightColorLin[i] * spec;\n"
            "    }\n"
            "    col += vec3(uBoost);\n"
            "    fragColor = vec4(col, 1.0);\n"
            "}\n";
    }

    Int uViewProjection_, uModel_, uNormalMatrix_, uBaseColorLin_, uAmbientLin_, uCameraEye_;
    Int uBoost_, uUnlit_, uLightCount_, uLightVec_, uLightColorLin_, uLightIntensity_;
    Int uLightIsPoint_, uLightRange_;
};

// Renders the engine model to an offscreen sRGB framebuffer and reads it back.
// Construct with a GL context already current. Sample count is clamped to the
// driver maximum; pass 1 to disable MSAA.
class MagnumRenderer {
public:
    explicit MagnumRenderer(int width, int height, int samples = WL2_3D_GPU_SAMPLES)
        : width_(width), height_(height) {
        if (width <= 0 || height <= 0) {
            throw std::runtime_error("wl2_3d GPU renderer requires a positive frame size");
        }
        const Vector2i size{width, height};
        samples_ = std::max(1, std::min(samples, MnGL::Renderbuffer::maxSamples()));

        colorMs_.setStorageMultisample(samples_, MnGL::RenderbufferFormat::SRGB8Alpha8, size);
        depthMs_.setStorageMultisample(samples_, MnGL::RenderbufferFormat::DepthComponent24, size);
        fbMs_ = MnGL::Framebuffer{Range2Di{{}, size}};
        fbMs_.attachRenderbuffer(MnGL::Framebuffer::ColorAttachment{0}, colorMs_);
        fbMs_.attachRenderbuffer(MnGL::Framebuffer::BufferAttachment::Depth, depthMs_);

        colorResolve_.setStorage(MnGL::RenderbufferFormat::SRGB8Alpha8, size);
        fbResolve_ = MnGL::Framebuffer{Range2Di{{}, size}};
        fbResolve_.attachRenderbuffer(MnGL::Framebuffer::ColorAttachment{0}, colorResolve_);
    }

    int width() const { return width_; }
    int height() const { return height_; }
    int samples() const { return samples_; }

    // Draw the scene, then resolve MSAA into the readback target.
    void render(const Engine& engine) {
        const Vector2i size{width_, height_};
        MnGL::Renderer::enable(MnGL::Renderer::Feature::DepthTest);
        MnGL::Renderer::disable(MnGL::Renderer::Feature::FaceCulling);  // two-sided, like the CPU view
        MnGL::Renderer::enable(MnGL::Renderer::Feature::FramebufferSrgb);

        // Solid background near the CPU sky mid-tone. Clear color is treated as
        // linear and sRGB-encoded into the framebuffer.
        MnGL::Renderer::setClearColor(Color4{gpu_srgb_to_linear(0.07), gpu_srgb_to_linear(0.10),
                                             gpu_srgb_to_linear(0.15), 1.0f});
        fbMs_.bind();
        fbMs_.clear(MnGL::FramebufferClear::Color | MnGL::FramebufferClear::Depth);

        const Matrix4 viewProjection = gpu_to_matrix4(engine.camera.viewProjection());
        shader_.setViewProjection(viewProjection)
            .setCameraEye(Vector3{(Float)engine.camera.eye.x, (Float)engine.camera.eye.y,
                                  (Float)engine.camera.eye.z});

        std::vector<Vector3> lv, lc;
        std::vector<Float> li, lr;
        std::vector<Int> lp;
        collectLights(engine, lv, lc, li, lp, lr);
        shader_.setLights(lv, lc, li, lp, lr)
            .setAmbientLin(Vector3{gpu_srgb_to_linear(engine.ambientLight.r),
                                   gpu_srgb_to_linear(engine.ambientLight.g),
                                   gpu_srgb_to_linear(engine.ambientLight.b)});

        for (const auto& [handle, node] : engine.nodes()) {
            if (!node.visible || node.kind == "light" || node.primitive == "skydome") {
                continue;
            }
            if (node.primitive == "grid") {
                drawGrid(engine, node);
            } else if (node.mesh) {
                drawMesh(engine, node);
            }
            // Non-mesh primitives / impostors / particles: CPU view for now.
        }

        MnGL::Framebuffer::blit(fbMs_, fbResolve_, Range2Di{{}, size}, MnGL::FramebufferBlit::Color);
    }

    // Copy the resolved frame into an RGBA8 buffer with the FrameRing contract:
    // top-left origin (GL is bottom-left, so flip rows) and explicit stride.
    void readInto(unsigned char* dst, int stride) {
        if (!dst || stride < width_ * 4) {
            return;
        }
        Magnum::Image2D image{Magnum::PixelFormat::RGBA8Unorm};
        fbResolve_.read(Range2Di{{}, Vector2i{width_, height_}}, image);
        const auto bytes = image.data();
        const int rowBytes = width_ * 4;
        for (int y = 0; y < height_; ++y) {
            const int srcY = height_ - 1 - y;  // vertical flip
            std::memcpy(dst + static_cast<size_t>(y) * stride,
                        bytes.data() + static_cast<size_t>(srcY) * rowBytes, rowBytes);
        }
    }

private:
    static void collectLights(const Engine& engine, std::vector<Vector3>& vec,
                              std::vector<Vector3>& colorLin, std::vector<Float>& intensity,
                              std::vector<Int>& isPoint, std::vector<Float>& range) {
        for (const auto& [handle, light] : engine.lights()) {
            if (!light.visible || light.intensity <= 0.0) {
                continue;
            }
            if (static_cast<int>(vec.size()) >= SurfaceShader::MaxLights) {
                break;
            }
            const bool point = light.kind == LightKind::Point;
            const Vec3 v = point ? light.position : light.direction;
            vec.push_back(Vector3{(Float)v.x, (Float)v.y, (Float)v.z});
            colorLin.push_back(Vector3{gpu_srgb_to_linear(light.color.r), gpu_srgb_to_linear(light.color.g),
                                       gpu_srgb_to_linear(light.color.b)});
            intensity.push_back((Float)light.intensity);
            isPoint.push_back(point ? 1 : 0);
            range.push_back((Float)light.range);
        }
        if (vec.empty()) {  // CPU empty-lights fallback: one white key at 0.72
            vec.push_back(Vector3{-0.5f, 1.0f, 0.6f});
            colorLin.push_back(Vector3{1.0f, 1.0f, 1.0f});
            intensity.push_back(0.72f);
            isPoint.push_back(0);
            range.push_back(100.0f);
        }
    }

    static Float attentionBoost(const Attention& att) {
        if (att.kind == AttentionKind::Pulse || att.kind == AttentionKind::Glow ||
            att.kind == AttentionKind::Ping || att.kind == AttentionKind::Bounce) {
            return static_cast<Float>(0.6 * att.intensity);
        }
        if (att.kind == AttentionKind::Rotate) {
            return 0.2f;
        }
        return 0.0f;
    }

    void drawMesh(const Engine& engine, const Node& node) {
        const MeshGeometry& mesh = *node.mesh;
        if (mesh.vertices.empty() || mesh.indices.empty()) {
            return;
        }
        const bool haveNormals = mesh.normals.size() == mesh.vertices.size();
        std::vector<Float> interleaved;
        interleaved.reserve(mesh.vertices.size() * 6);
        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
            const Vec3& p = mesh.vertices[i];
            const Vec3 n = haveNormals ? mesh.normals[i] : Vec3{0.0, 1.0, 0.0};
            interleaved.insert(interleaved.end(), {(Float)p.x, (Float)p.y, (Float)p.z,
                                                   (Float)n.x, (Float)n.y, (Float)n.z});
        }
        std::vector<UnsignedInt> indices(mesh.indices.begin(), mesh.indices.end());

        MnGL::Buffer vbo, ibo;
        vbo.setData(Corrade::Containers::arrayView(interleaved.data(), interleaved.size()));
        ibo.setData(Corrade::Containers::arrayView(indices.data(), indices.size()));
        MnGL::Mesh glMesh;
        glMesh.setPrimitive(MnGL::MeshPrimitive::Triangles)
            .setCount(static_cast<Int>(indices.size()))
            .addVertexBuffer(vbo, 0, SurfaceShader::Position{}, SurfaceShader::Normal{})
            .setIndexBuffer(ibo, 0, MnGL::MeshIndexType::UnsignedInt);

        Vec3 world = engine.worldPosition(node.handle);
        world.y += node.attention.lift * 0.35;  // bounce cue, matching the CPU view
        Vec3 spin = node.rotation;
        if (node.attention.kind == AttentionKind::Rotate) {
            spin.y += node.attention.spin;
        }
        const Mat4 model = translation_matrix(world) * rotation_matrix(spin) * scale_matrix(node.scale);
        const Matrix4 m = gpu_to_matrix4(model);

        shader_.setModel(m)
            .setNormalMatrix(m.rotationScaling())
            .setUnlit(false)
            .setBoost(attentionBoost(node.attention))
            .setBaseColorLin(Vector3{gpu_srgb_to_linear(node.material.r), gpu_srgb_to_linear(node.material.g),
                                     gpu_srgb_to_linear(node.material.b)})
            .draw(glMesh);
    }

    void drawGrid(const Engine& engine, const Node& node) {
        auto opt = [&](const char* key, double fallback) {
            auto it = node.primitiveOptions.find(key);
            return it == node.primitiveOptions.end() ? fallback : it->second;
        };
        const double size = opt("size", 20.0);
        const int divisions = std::max(1, static_cast<int>(opt("divisions", 10.0)));
        const double half = size * 0.5, step = size / divisions;
        const Vec3 origin = engine.worldPosition(node.handle);

        std::vector<Float> verts;  // position + dummy normal per endpoint
        auto line = [&](Vec3 a, Vec3 b) {
            verts.insert(verts.end(), {(Float)(origin.x + a.x), (Float)(origin.y + a.y),
                                       (Float)(origin.z + a.z), 0.0f, 1.0f, 0.0f});
            verts.insert(verts.end(), {(Float)(origin.x + b.x), (Float)(origin.y + b.y),
                                       (Float)(origin.z + b.z), 0.0f, 1.0f, 0.0f});
        };
        for (int i = 0; i <= divisions; ++i) {
            const double c = -half + i * step;
            line({c, 0.0, -half}, {c, 0.0, half});
            line({-half, 0.0, c}, {half, 0.0, c});
        }
        if (verts.empty()) {
            return;
        }
        MnGL::Buffer vbo;
        vbo.setData(Corrade::Containers::arrayView(verts.data(), verts.size()));
        MnGL::Mesh glMesh;
        glMesh.setPrimitive(MnGL::MeshPrimitive::Lines)
            .setCount(static_cast<Int>(verts.size() / 6))
            .addVertexBuffer(vbo, 0, SurfaceShader::Position{}, SurfaceShader::Normal{});

        shader_.setModel(Matrix4{})  // identity; grid verts are already in world space
            .setNormalMatrix(Matrix3{})
            .setUnlit(true)
            .setBoost(0.0f)
            .setBaseColorLin(Vector3{gpu_srgb_to_linear(node.material.r * 0.7),
                                     gpu_srgb_to_linear(node.material.g * 0.7),
                                     gpu_srgb_to_linear(node.material.b * 0.7)})
            .draw(glMesh);
    }

    int width_, height_, samples_ = 1;
    SurfaceShader shader_;
    MnGL::Renderbuffer colorMs_, depthMs_, colorResolve_;
    MnGL::Framebuffer fbMs_{Magnum::NoCreate};
    MnGL::Framebuffer fbResolve_{Magnum::NoCreate};
};

}  // namespace wl2::three_d

#endif  // WL2_3D_HAVE_MAGNUM
