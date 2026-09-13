#pragma once

#include <tynima/core/math/scalar.h>

#include <cmath>
#include <cstddef>

namespace tynima::math {

// Plain float vectors. Members are public and the layout is exactly N floats,
// so a Vec3 can be memcpy'd into a vertex buffer. GPU-side alignment (a float3
// in a Metal/HLSL constant buffer takes 16 bytes) is the uniform struct's job,
// not the vector's.

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    constexpr Vec2() noexcept = default;
    constexpr Vec2(float x_, float y_) noexcept : x(x_), y(y_) {}
    constexpr explicit Vec2(float v) noexcept : x(v), y(v) {}

    [[nodiscard]] static constexpr Vec2 zero() noexcept { return {0.0f, 0.0f}; }
    [[nodiscard]] static constexpr Vec2 one() noexcept { return {1.0f, 1.0f}; }
    [[nodiscard]] static constexpr Vec2 unit_x() noexcept { return {1.0f, 0.0f}; }
    [[nodiscard]] static constexpr Vec2 unit_y() noexcept { return {0.0f, 1.0f}; }

    constexpr float& operator[](std::size_t i) noexcept { return i == 0 ? x : y; }
    constexpr float operator[](std::size_t i) const noexcept { return i == 0 ? x : y; }

    constexpr Vec2& operator+=(const Vec2& o) noexcept { x += o.x; y += o.y; return *this; }
    constexpr Vec2& operator-=(const Vec2& o) noexcept { x -= o.x; y -= o.y; return *this; }
    constexpr Vec2& operator*=(float s) noexcept { x *= s; y *= s; return *this; }
    constexpr Vec2& operator/=(float s) noexcept { x /= s; y /= s; return *this; }
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr Vec3() noexcept = default;
    constexpr Vec3(float x_, float y_, float z_) noexcept : x(x_), y(y_), z(z_) {}
    constexpr explicit Vec3(float v) noexcept : x(v), y(v), z(v) {}
    constexpr Vec3(const Vec2& xy, float z_) noexcept : x(xy.x), y(xy.y), z(z_) {}

    [[nodiscard]] static constexpr Vec3 zero() noexcept { return {0.0f, 0.0f, 0.0f}; }
    [[nodiscard]] static constexpr Vec3 one() noexcept { return {1.0f, 1.0f, 1.0f}; }
    [[nodiscard]] static constexpr Vec3 unit_x() noexcept { return {1.0f, 0.0f, 0.0f}; }
    [[nodiscard]] static constexpr Vec3 unit_y() noexcept { return {0.0f, 1.0f, 0.0f}; }
    [[nodiscard]] static constexpr Vec3 unit_z() noexcept { return {0.0f, 0.0f, 1.0f}; }

    [[nodiscard]] constexpr Vec2 xy() const noexcept { return {x, y}; }

    constexpr float& operator[](std::size_t i) noexcept { return i == 0 ? x : (i == 1 ? y : z); }
    constexpr float operator[](std::size_t i) const noexcept { return i == 0 ? x : (i == 1 ? y : z); }

    constexpr Vec3& operator+=(const Vec3& o) noexcept { x += o.x; y += o.y; z += o.z; return *this; }
    constexpr Vec3& operator-=(const Vec3& o) noexcept { x -= o.x; y -= o.y; z -= o.z; return *this; }
    constexpr Vec3& operator*=(float s) noexcept { x *= s; y *= s; z *= s; return *this; }
    constexpr Vec3& operator/=(float s) noexcept { x /= s; y /= s; z /= s; return *this; }
};

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    constexpr Vec4() noexcept = default;
    constexpr Vec4(float x_, float y_, float z_, float w_) noexcept : x(x_), y(y_), z(z_), w(w_) {}
    constexpr explicit Vec4(float v) noexcept : x(v), y(v), z(v), w(v) {}
    constexpr Vec4(const Vec3& xyz, float w_) noexcept : x(xyz.x), y(xyz.y), z(xyz.z), w(w_) {}

    [[nodiscard]] static constexpr Vec4 zero() noexcept { return {0.0f, 0.0f, 0.0f, 0.0f}; }
    [[nodiscard]] static constexpr Vec4 one() noexcept { return {1.0f, 1.0f, 1.0f, 1.0f}; }

    [[nodiscard]] constexpr Vec3 xyz() const noexcept { return {x, y, z}; }

    constexpr float& operator[](std::size_t i) noexcept { return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w)); }
    constexpr float operator[](std::size_t i) const noexcept { return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w)); }

    constexpr Vec4& operator+=(const Vec4& o) noexcept { x += o.x; y += o.y; z += o.z; w += o.w; return *this; }
    constexpr Vec4& operator-=(const Vec4& o) noexcept { x -= o.x; y -= o.y; z -= o.z; w -= o.w; return *this; }
    constexpr Vec4& operator*=(float s) noexcept { x *= s; y *= s; z *= s; w *= s; return *this; }
    constexpr Vec4& operator/=(float s) noexcept { x /= s; y /= s; z /= s; w /= s; return *this; }
};

// ---------------------------------------------------------------- operators

[[nodiscard]] constexpr Vec2 operator+(const Vec2& a, const Vec2& b) noexcept { return {a.x + b.x, a.y + b.y}; }
[[nodiscard]] constexpr Vec2 operator-(const Vec2& a, const Vec2& b) noexcept { return {a.x - b.x, a.y - b.y}; }
[[nodiscard]] constexpr Vec2 operator-(const Vec2& a) noexcept { return {-a.x, -a.y}; }
[[nodiscard]] constexpr Vec2 operator*(const Vec2& a, float s) noexcept { return {a.x * s, a.y * s}; }
[[nodiscard]] constexpr Vec2 operator*(float s, const Vec2& a) noexcept { return a * s; }
[[nodiscard]] constexpr Vec2 operator*(const Vec2& a, const Vec2& b) noexcept { return {a.x * b.x, a.y * b.y}; }
[[nodiscard]] constexpr Vec2 operator/(const Vec2& a, float s) noexcept { return {a.x / s, a.y / s}; }
[[nodiscard]] constexpr bool operator==(const Vec2& a, const Vec2& b) noexcept { return a.x == b.x && a.y == b.y; }
[[nodiscard]] constexpr bool operator!=(const Vec2& a, const Vec2& b) noexcept { return !(a == b); }

[[nodiscard]] constexpr Vec3 operator+(const Vec3& a, const Vec3& b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
[[nodiscard]] constexpr Vec3 operator-(const Vec3& a, const Vec3& b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
[[nodiscard]] constexpr Vec3 operator-(const Vec3& a) noexcept { return {-a.x, -a.y, -a.z}; }
[[nodiscard]] constexpr Vec3 operator*(const Vec3& a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
[[nodiscard]] constexpr Vec3 operator*(float s, const Vec3& a) noexcept { return a * s; }
[[nodiscard]] constexpr Vec3 operator*(const Vec3& a, const Vec3& b) noexcept { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
[[nodiscard]] constexpr Vec3 operator/(const Vec3& a, float s) noexcept { return {a.x / s, a.y / s, a.z / s}; }
[[nodiscard]] constexpr bool operator==(const Vec3& a, const Vec3& b) noexcept { return a.x == b.x && a.y == b.y && a.z == b.z; }
[[nodiscard]] constexpr bool operator!=(const Vec3& a, const Vec3& b) noexcept { return !(a == b); }

[[nodiscard]] constexpr Vec4 operator+(const Vec4& a, const Vec4& b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
[[nodiscard]] constexpr Vec4 operator-(const Vec4& a, const Vec4& b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
[[nodiscard]] constexpr Vec4 operator-(const Vec4& a) noexcept { return {-a.x, -a.y, -a.z, -a.w}; }
[[nodiscard]] constexpr Vec4 operator*(const Vec4& a, float s) noexcept { return {a.x * s, a.y * s, a.z * s, a.w * s}; }
[[nodiscard]] constexpr Vec4 operator*(float s, const Vec4& a) noexcept { return a * s; }
[[nodiscard]] constexpr Vec4 operator*(const Vec4& a, const Vec4& b) noexcept { return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w}; }
[[nodiscard]] constexpr Vec4 operator/(const Vec4& a, float s) noexcept { return {a.x / s, a.y / s, a.z / s, a.w / s}; }
[[nodiscard]] constexpr bool operator==(const Vec4& a, const Vec4& b) noexcept { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }
[[nodiscard]] constexpr bool operator!=(const Vec4& a, const Vec4& b) noexcept { return !(a == b); }

// ---------------------------------------------------------------- functions

[[nodiscard]] constexpr float dot(const Vec2& a, const Vec2& b) noexcept { return a.x * b.x + a.y * b.y; }
[[nodiscard]] constexpr float dot(const Vec3& a, const Vec3& b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
[[nodiscard]] constexpr float dot(const Vec4& a, const Vec4& b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

// Right-hand rule: cross(unit_x, unit_y) == unit_z.
[[nodiscard]] constexpr Vec3 cross(const Vec3& a, const Vec3& b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

[[nodiscard]] constexpr float length_squared(const Vec2& v) noexcept { return dot(v, v); }
[[nodiscard]] constexpr float length_squared(const Vec3& v) noexcept { return dot(v, v); }
[[nodiscard]] constexpr float length_squared(const Vec4& v) noexcept { return dot(v, v); }

[[nodiscard]] inline float length(const Vec2& v) noexcept { return std::sqrt(length_squared(v)); }
[[nodiscard]] inline float length(const Vec3& v) noexcept { return std::sqrt(length_squared(v)); }
[[nodiscard]] inline float length(const Vec4& v) noexcept { return std::sqrt(length_squared(v)); }

[[nodiscard]] inline float distance(const Vec2& a, const Vec2& b) noexcept { return length(a - b); }
[[nodiscard]] inline float distance(const Vec3& a, const Vec3& b) noexcept { return length(a - b); }

// A zero vector normalizes to zero rather than NaN: the cheap, predictable
// choice for a game engine, where a degenerate direction is common and a NaN
// spreads through a whole frame.
[[nodiscard]] inline Vec2 normalize(const Vec2& v) noexcept {
    const float len = length(v);
    return len > 0.0f ? v / len : Vec2::zero();
}
[[nodiscard]] inline Vec3 normalize(const Vec3& v) noexcept {
    const float len = length(v);
    return len > 0.0f ? v / len : Vec3::zero();
}
[[nodiscard]] inline Vec4 normalize(const Vec4& v) noexcept {
    const float len = length(v);
    return len > 0.0f ? v / len : Vec4::zero();
}

[[nodiscard]] constexpr Vec2 lerp(const Vec2& a, const Vec2& b, float t) noexcept { return a + (b - a) * t; }
[[nodiscard]] constexpr Vec3 lerp(const Vec3& a, const Vec3& b, float t) noexcept { return a + (b - a) * t; }
[[nodiscard]] constexpr Vec4 lerp(const Vec4& a, const Vec4& b, float t) noexcept { return a + (b - a) * t; }

[[nodiscard]] constexpr Vec3 min(const Vec3& a, const Vec3& b) noexcept {
    return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z};
}
[[nodiscard]] constexpr Vec3 max(const Vec3& a, const Vec3& b) noexcept {
    return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z};
}

// Reflect an incident direction about a unit normal.
[[nodiscard]] constexpr Vec3 reflect(const Vec3& incident, const Vec3& normal) noexcept {
    return incident - normal * (2.0f * dot(incident, normal));
}

[[nodiscard]] constexpr bool approx_equal(const Vec2& a, const Vec2& b, float epsilon = kEpsilon) noexcept {
    return approx_equal(a.x, b.x, epsilon) && approx_equal(a.y, b.y, epsilon);
}
[[nodiscard]] constexpr bool approx_equal(const Vec3& a, const Vec3& b, float epsilon = kEpsilon) noexcept {
    return approx_equal(a.x, b.x, epsilon) && approx_equal(a.y, b.y, epsilon) && approx_equal(a.z, b.z, epsilon);
}
[[nodiscard]] constexpr bool approx_equal(const Vec4& a, const Vec4& b, float epsilon = kEpsilon) noexcept {
    return approx_equal(a.x, b.x, epsilon) && approx_equal(a.y, b.y, epsilon) && approx_equal(a.z, b.z, epsilon) &&
           approx_equal(a.w, b.w, epsilon);
}

} // namespace tynima::math
