// Renderer-independent particle simulation for wl2:3d effects (§13.2).
//
// The CPU simulation (emitters; per-particle lifetime, velocity, color/size over
// life; additive/alpha blend) lives here and is unit-testable with the renderer
// off. GPU rendering — instanced quads, additive blending, and the glow/outline
// post pass, all WebGL2-safe (no compute/geometry shaders) — is the renderer's
// view over this state, gated behind the Magnum provider.
#pragma once

#include "wl2_3d_math.h"

#include <cstdint>
#include <vector>

namespace wl2::three_d {

struct ParticleColor {
    double r = 1.0;
    double g = 1.0;
    double b = 1.0;
    double a = 1.0;
};

enum class BlendMode { Additive, Alpha };

struct Particle {
    Vec3 position;
    Vec3 velocity;
    double age = 0.0;
    double lifetime = 1.0;
};

// Deterministic xorshift so jittered emission is reproducible in tests.
class Rng {
public:
    explicit Rng(uint64_t seed) : state_(seed ? seed : 0x9e3779b97f4a7c15ull) {}
    // Uniform in [-1, 1].
    double signedUnit() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 7;
        state_ ^= state_ << 17;
        return (static_cast<double>(state_ >> 11) / static_cast<double>(1ull << 53)) * 2.0 - 1.0;
    }

private:
    uint64_t state_;
};

struct Emitter {
    int64_t handle = 0;
    Vec3 position;
    Vec3 velocity{0.0, 1.0, 0.0};
    Vec3 velocityJitter;
    Vec3 gravity;
    double rate = 20.0;        // particles per second
    double lifetime = 1.0;
    ParticleColor colorStart;
    ParticleColor colorEnd{1.0, 1.0, 1.0, 0.0};
    double sizeStart = 0.2;
    double sizeEnd = 0.0;
    BlendMode blend = BlendMode::Additive;
    int maxParticles = 4096;
    bool emitting = true;

    double accumulator = 0.0;
    std::vector<Particle> particles;
    Rng rng{0x1234567};

    // Color/size at a particle's current life fraction (drives the renderer).
    double lifeFraction(const Particle& p) const {
        return p.lifetime > 0.0 ? p.age / p.lifetime : 1.0;
    }
    ParticleColor colorAt(double t) const {
        return {colorStart.r + (colorEnd.r - colorStart.r) * t,
                colorStart.g + (colorEnd.g - colorStart.g) * t,
                colorStart.b + (colorEnd.b - colorStart.b) * t,
                colorStart.a + (colorEnd.a - colorStart.a) * t};
    }
    double sizeAt(double t) const { return sizeStart + (sizeEnd - sizeStart) * t; }

    void emitOne() {
        if (static_cast<int>(particles.size()) >= maxParticles) {
            return;
        }
        Particle p;
        p.position = position;
        p.velocity = {velocity.x + velocityJitter.x * rng.signedUnit(),
                      velocity.y + velocityJitter.y * rng.signedUnit(),
                      velocity.z + velocityJitter.z * rng.signedUnit()};
        p.lifetime = lifetime;
        particles.push_back(p);
    }

    // Advance the simulation by dtSeconds: emit, integrate, retire dead.
    void advance(double dtSeconds) {
        if (emitting) {
            accumulator += rate * dtSeconds;
            while (accumulator >= 1.0) {
                emitOne();
                accumulator -= 1.0;
            }
        }
        size_t live = 0;
        for (auto& p : particles) {
            p.age += dtSeconds;
            if (p.age >= p.lifetime) {
                continue;  // retire
            }
            p.velocity = p.velocity + gravity * dtSeconds;
            p.position = p.position + p.velocity * dtSeconds;
            particles[live++] = p;
        }
        particles.resize(live);
    }
};

}  // namespace wl2::three_d
