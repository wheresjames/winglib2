// Renderer-independent 3D math for the wl2:3d engine core.
//
// Pure CPU geometry: vectors, a column-major (OpenGL-style) 4x4 matrix, a
// calibrated pinhole camera, and the 2D<->3D mapping (project / unproject /
// ray-plane). This header carries no GPU dependency so the engine core builds
// and is unit-testable with the Magnum provider `off`.
#pragma once

#include <array>
#include <cmath>
#include <optional>

namespace wl2::three_d {

constexpr double kPi = 3.14159265358979323846;

inline double radians(double degrees) {
    return degrees * (kPi / 180.0);
}

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(Vec3 a) {
    const double len = length(a);
    return len > 0.0 ? a * (1.0 / len) : a;
}

// Column-major 4x4 matrix: element at (row r, col c) is m[c * 4 + r], matching
// the OpenGL memory layout so it composes directly with renderer conventions.
struct Mat4 {
    std::array<double, 16> m{};

    double at(int row, int col) const { return m[static_cast<size_t>(col) * 4 + row]; }
    double& at(int row, int col) { return m[static_cast<size_t>(col) * 4 + row]; }

    static Mat4 identity() {
        Mat4 out;
        out.m[0] = out.m[5] = out.m[10] = out.m[15] = 1.0;
        return out;
    }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 out;
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                sum += a.at(row, k) * b.at(k, col);
            }
            out.at(row, col) = sum;
        }
    }
    return out;
}

// Returns (x, y, z, w) of M * (v, w_in).
inline std::array<double, 4> transform(const Mat4& matrix, Vec3 v, double w_in) {
    std::array<double, 4> in{v.x, v.y, v.z, w_in};
    std::array<double, 4> out{0, 0, 0, 0};
    for (int row = 0; row < 4; ++row) {
        double sum = 0.0;
        for (int col = 0; col < 4; ++col) {
            sum += matrix.at(row, col) * in[static_cast<size_t>(col)];
        }
        out[static_cast<size_t>(row)] = sum;
    }
    return out;
}

// Right-handed perspective projection mapping z to clip [-near, far] -> [-1, 1].
inline Mat4 perspective(double fovYRadians, double aspect, double near, double far) {
    Mat4 out;
    const double f = 1.0 / std::tan(fovYRadians * 0.5);
    out.at(0, 0) = f / aspect;
    out.at(1, 1) = f;
    out.at(2, 2) = (far + near) / (near - far);
    out.at(2, 3) = (2.0 * far * near) / (near - far);
    out.at(3, 2) = -1.0;
    return out;
}

// Right-handed view matrix looking from `eye` toward `target`.
inline Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up) {
    const Vec3 forward = normalize(target - eye);
    const Vec3 side = normalize(cross(forward, up));
    const Vec3 trueUp = cross(side, forward);

    Mat4 out = Mat4::identity();
    out.at(0, 0) = side.x;     out.at(0, 1) = side.y;     out.at(0, 2) = side.z;
    out.at(1, 0) = trueUp.x;   out.at(1, 1) = trueUp.y;   out.at(1, 2) = trueUp.z;
    out.at(2, 0) = -forward.x; out.at(2, 1) = -forward.y; out.at(2, 2) = -forward.z;
    out.at(0, 3) = -dot(side, eye);
    out.at(1, 3) = -dot(trueUp, eye);
    out.at(2, 3) = dot(forward, eye);
    return out;
}

// General 4x4 inverse (cofactor method, OpenGL column-major). Returns nullopt
// for singular matrices.
inline std::optional<Mat4> inverse(const Mat4& src) {
    const std::array<double, 16>& m = src.m;
    std::array<double, 16> inv{};

    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
             m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
             m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
              m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
             m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
             m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
             m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
              m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
             m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
             m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
              m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
              m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
             m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
             m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
              m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
              m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::abs(det) < 1e-12) {
        return std::nullopt;
    }
    det = 1.0 / det;
    Mat4 out;
    for (int i = 0; i < 16; ++i) {
        out.m[static_cast<size_t>(i)] = inv[static_cast<size_t>(i)] * det;
    }
    return out;
}

struct Ray {
    Vec3 origin;
    Vec3 direction;  // normalized
};

// Infinite plane defined by a point and a (not necessarily unit) normal.
struct Plane {
    Vec3 point;
    Vec3 normal{0.0, 1.0, 0.0};
};

// Intersect a ray with a plane. Returns the world point, or nullopt when the
// ray is parallel to the plane or would hit it behind the origin.
inline std::optional<Vec3> intersect(const Ray& ray, const Plane& plane) {
    const double denom = dot(plane.normal, ray.direction);
    if (std::abs(denom) < 1e-9) {
        return std::nullopt;
    }
    const double t = dot(plane.normal, plane.point - ray.origin) / denom;
    if (t < 0.0) {
        return std::nullopt;
    }
    return ray.origin + ray.direction * t;
}

// Nearest intersection distance of a ray with a sphere, or nullopt if it misses
// (or only intersects behind the origin).
inline std::optional<double> intersect(const Ray& ray, Vec3 center, double radius) {
    const Vec3 oc = ray.origin - center;
    const double b = dot(oc, ray.direction);
    const double c = dot(oc, oc) - radius * radius;
    const double disc = b * b - c;
    if (disc < 0.0) {
        return std::nullopt;
    }
    const double sqrtDisc = std::sqrt(disc);
    double t = -b - sqrtDisc;
    if (t < 0.0) {
        t = -b + sqrtDisc;
    }
    if (t < 0.0) {
        return std::nullopt;
    }
    return t;
}

inline std::optional<double> intersectTriangle(const Ray& ray, Vec3 a, Vec3 b, Vec3 c) {
    const Vec3 edge1 = b - a;
    const Vec3 edge2 = c - a;
    const Vec3 pvec = cross(ray.direction, edge2);
    const double det = dot(edge1, pvec);
    if (std::abs(det) < 1e-9) {
        return std::nullopt;
    }
    const double invDet = 1.0 / det;
    const Vec3 tvec = ray.origin - a;
    const double u = dot(tvec, pvec) * invDet;
    if (u < 0.0 || u > 1.0) {
        return std::nullopt;
    }
    const Vec3 qvec = cross(tvec, edge1);
    const double v = dot(ray.direction, qvec) * invDet;
    if (v < 0.0 || u + v > 1.0) {
        return std::nullopt;
    }
    const double t = dot(edge2, qvec) * invDet;
    return t >= 0.0 ? std::optional<double>{t} : std::nullopt;
}

// A calibrated pinhole camera: real intrinsics (FOV or focal+sensor), aspect,
// near/far, plus a pose. Distortion is left for a follow-up (pinhole only).
struct Camera {
    Vec3 eye{0.0, 8.0, 16.0};
    Vec3 target{0.0, 0.0, 0.0};
    Vec3 up{0.0, 1.0, 0.0};
    double fovYRadians = radians(52.0);
    double aspect = 16.0 / 9.0;
    double near = 0.1;
    double far = 5000.0;

    // Derive vertical FOV from focal length and sensor height (mm), the legacy
    // SetLens(focal, ccdW, ccdH) calibration.
    void setLens(double focalMm, double sensorWidthMm, double sensorHeightMm) {
        if (focalMm > 0.0 && sensorHeightMm > 0.0) {
            fovYRadians = 2.0 * std::atan(sensorHeightMm / (2.0 * focalMm));
        }
        if (sensorHeightMm > 0.0 && sensorWidthMm > 0.0) {
            aspect = sensorWidthMm / sensorHeightMm;
        }
    }

    Mat4 view() const { return lookAt(eye, target, up); }
    Mat4 projection() const { return perspective(fovYRadians, aspect, near, far); }
    Mat4 viewProjection() const { return projection() * view(); }
};

struct ScreenPoint {
    double x = 0.0;       // pixels, top-left origin
    double y = 0.0;
    double depth = 0.0;   // normalized device depth [-1, 1]
    bool onScreen = false;
};

// Forward projection: world point -> screen pixel (top-left origin).
inline ScreenPoint project(const Camera& camera, Vec3 world, double width, double height) {
    const auto clip = transform(camera.viewProjection(), world, 1.0);
    ScreenPoint out;
    if (std::abs(clip[3]) < 1e-12) {
        return out;
    }
    const double invW = 1.0 / clip[3];
    const double ndcX = clip[0] * invW;
    const double ndcY = clip[1] * invW;
    const double ndcZ = clip[2] * invW;
    out.x = (ndcX * 0.5 + 0.5) * width;
    out.y = (0.5 - ndcY * 0.5) * height;  // flip to top-left origin
    out.depth = ndcZ;
    out.onScreen = clip[3] > 0.0 && ndcX >= -1.0 && ndcX <= 1.0 && ndcY >= -1.0 &&
                   ndcY <= 1.0 && ndcZ >= -1.0 && ndcZ <= 1.0;
    return out;
}

// Inverse projection: screen pixel (top-left origin) -> world ray from the eye.
inline std::optional<Ray> unproject(const Camera& camera, double px, double py, double width,
                                    double height) {
    const auto inv = inverse(camera.viewProjection());
    if (!inv) {
        return std::nullopt;
    }
    const double ndcX = (px / width) * 2.0 - 1.0;
    const double ndcY = 1.0 - (py / height) * 2.0;  // top-left origin -> NDC

    const auto nearH = transform(*inv, Vec3{ndcX, ndcY, -1.0}, 1.0);
    const auto farH = transform(*inv, Vec3{ndcX, ndcY, 1.0}, 1.0);
    if (std::abs(nearH[3]) < 1e-12 || std::abs(farH[3]) < 1e-12) {
        return std::nullopt;
    }
    const Vec3 nearP{nearH[0] / nearH[3], nearH[1] / nearH[3], nearH[2] / nearH[3]};
    const Vec3 farP{farH[0] / farH[3], farH[1] / farH[3], farH[2] / farH[3]};
    return Ray{nearP, normalize(farP - nearP)};
}

}  // namespace wl2::three_d
