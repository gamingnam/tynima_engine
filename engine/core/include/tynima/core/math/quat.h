#pragma once

#include <tynima/core/math/mat.h>
#include <tynima/core/math/trig.h>
#include <tynima/core/math/vec.h>

#include <cmath>

namespace tynima::math {

// Unit quaternion for rotations, stored {x, y, z, w} with w the scalar part —
// glTF's order, so imported rotations copy straight in. Hamilton product:
// (a * b).rotate(v) == a.rotate(b.rotate(v)), the same composition order as
// matrices. Only unit quaternions rotate correctly; normalize() after
// accumulating many products.
struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;

    constexpr Quat() noexcept = default;
    constexpr Quat(float x_, float y_, float z_, float w_) noexcept : x(x_), y(y_), z(z_), w(w_) {}

    [[nodiscard]] static constexpr Quat identity() noexcept { return {0.0f, 0.0f, 0.0f, 1.0f}; }

    // `axis` must be unit length; `angle` in radians, right-hand rule.
    [[nodiscard]] static Quat from_axis_angle(const Vec3& axis, float angle) noexcept {
        float s, c;
        sin_cos(0.5f * angle, s, c); // the deterministic pair: the same quaternion on every platform
        return {axis.x * s, axis.y * s, axis.z * s, c};
    }

    // The rotation taking unit vector `from` to unit vector `to`, by the shortest arc.
    [[nodiscard]] static Quat from_to(const Vec3& from, const Vec3& to) noexcept;

    [[nodiscard]] constexpr Vec3 vector_part() const noexcept { return {x, y, z}; }

    // Rotates v by this (unit) quaternion: q v q*, in the cheaper expanded form.
    [[nodiscard]] constexpr Vec3 rotate(const Vec3& v) const noexcept {
        const Vec3 u{x, y, z};
        const Vec3 t = cross(u, v) * 2.0f;
        return v + t * w + cross(u, t);
    }

    [[nodiscard]] constexpr Mat3 to_mat3() const noexcept {
        const float xx = x * x, yy = y * y, zz = z * z;
        const float xy = x * y, xz = x * z, yz = y * z;
        const float wx = w * x, wy = w * y, wz = w * z;
        return Mat3::from_rows({1.0f - 2.0f * (yy + zz), 2.0f * (xy - wz), 2.0f * (xz + wy)},
                               {2.0f * (xy + wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz - wx)},
                               {2.0f * (xz - wy), 2.0f * (yz + wx), 1.0f - 2.0f * (xx + yy)});
    }
    [[nodiscard]] constexpr Mat4 to_mat4() const noexcept { return Mat4::from_mat3(to_mat3()); }
};

[[nodiscard]] constexpr Quat operator*(const Quat& a, const Quat& b) noexcept {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
[[nodiscard]] constexpr Quat operator*(const Quat& q, float s) noexcept { return {q.x * s, q.y * s, q.z * s, q.w * s}; }
[[nodiscard]] constexpr Quat operator+(const Quat& a, const Quat& b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
[[nodiscard]] constexpr Quat operator-(const Quat& q) noexcept { return {-q.x, -q.y, -q.z, -q.w}; }
[[nodiscard]] constexpr bool operator==(const Quat& a, const Quat& b) noexcept {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}
[[nodiscard]] constexpr bool operator!=(const Quat& a, const Quat& b) noexcept { return !(a == b); }

[[nodiscard]] constexpr float dot(const Quat& a, const Quat& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}
[[nodiscard]] constexpr float length_squared(const Quat& q) noexcept { return dot(q, q); }
[[nodiscard]] inline float length(const Quat& q) noexcept { return std::sqrt(length_squared(q)); }

[[nodiscard]] inline Quat normalize(const Quat& q) noexcept {
    const float len = length(q);
    return len > 0.0f ? q * (1.0f / len) : Quat::identity();
}

[[nodiscard]] constexpr Quat conjugate(const Quat& q) noexcept { return {-q.x, -q.y, -q.z, q.w}; }

// For a unit quaternion the inverse is the conjugate; this handles the
// general case so a slightly drifted quaternion still inverts correctly.
[[nodiscard]] constexpr Quat inverse(const Quat& q) noexcept {
    const float ls = length_squared(q);
    return ls > 0.0f ? conjugate(q) * (1.0f / ls) : Quat::identity();
}

// Normalized linear interpolation: cheap, commutative, not constant-speed.
// Good enough for small angles and for blending many rotations.
[[nodiscard]] inline Quat nlerp(const Quat& a, const Quat& b, float t) noexcept {
    const Quat to = dot(a, b) < 0.0f ? -b : b; // take the short way round
    return normalize(a + (to + (-a)) * t);
}

// Spherical interpolation: constant angular speed along the shortest arc.
// Falls back to nlerp when the quaternions are nearly parallel, where the
// sin() denominator loses precision.
[[nodiscard]] inline Quat slerp(const Quat& a, const Quat& b, float t) noexcept {
    float cos_theta = dot(a, b);
    Quat to = b;
    if (cos_theta < 0.0f) {
        to = -b;
        cos_theta = -cos_theta;
    }
    if (cos_theta > 0.9995f) {
        return nlerp(a, to, t);
    }
    const float theta = std::acos(cos_theta);
    const float sin_theta = std::sin(theta);
    const float wa = std::sin((1.0f - t) * theta) / sin_theta;
    const float wb = std::sin(t * theta) / sin_theta;
    return a * wa + to * wb;
}

inline Quat Quat::from_to(const Vec3& from, const Vec3& to) noexcept {
    const float d = dot(from, to);
    if (d < -0.999999f) {
        // Opposite vectors: any axis perpendicular to `from` will do.
        Vec3 axis = cross(Vec3::unit_x(), from);
        if (length_squared(axis) < 1e-6f) {
            axis = cross(Vec3::unit_y(), from);
        }
        return from_axis_angle(normalize(axis), kPi);
    }
    const Vec3 c = cross(from, to);
    return normalize(Quat{c.x, c.y, c.z, 1.0f + d});
}

[[nodiscard]] constexpr bool approx_equal(const Quat& a, const Quat& b, float epsilon = kEpsilon) noexcept {
    return approx_equal(a.x, b.x, epsilon) && approx_equal(a.y, b.y, epsilon) && approx_equal(a.z, b.z, epsilon) &&
           approx_equal(a.w, b.w, epsilon);
}

} // namespace tynima::math
