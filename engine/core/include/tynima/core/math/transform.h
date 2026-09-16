#pragma once

#include <tynima/core/math/mat.h>
#include <tynima/core/math/quat.h>
#include <tynima/core/math/trig.h>
#include <tynima/core/math/vec.h>

#include <cmath>

// Conventions, once, for the whole engine:
//   - world and view space are right-handed, y up; a camera looks down -z
//   - column-major matrices on column vectors: world = T * R * S * local
//   - clip space matches Metal, D3D12 and SDL GPU: y up, depth 0 (near) to 1 (far)
//   - angles are radians
namespace tynima::math {

[[nodiscard]] constexpr Mat4 translation(const Vec3& t) noexcept {
    Mat4 m = Mat4::identity();
    m.cols[3] = {t, 1.0f};
    return m;
}

[[nodiscard]] constexpr Mat4 scaling(const Vec3& s) noexcept {
    return {{s.x, 0.0f, 0.0f, 0.0f}, {0.0f, s.y, 0.0f, 0.0f}, {0.0f, 0.0f, s.z, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
}

// Right-hand rule about each axis: rotation_y(+90°) takes +x to -z.
[[nodiscard]] inline Mat4 rotation_x(float angle) noexcept {
    float s, c;
    sin_cos(angle, s, c);
    return Mat4::from_rows({1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, c, -s, 0.0f}, {0.0f, s, c, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f});
}
[[nodiscard]] inline Mat4 rotation_y(float angle) noexcept {
    float s, c;
    sin_cos(angle, s, c);
    return Mat4::from_rows({c, 0.0f, s, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {-s, 0.0f, c, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f});
}
[[nodiscard]] inline Mat4 rotation_z(float angle) noexcept {
    float s, c;
    sin_cos(angle, s, c);
    return Mat4::from_rows({c, -s, 0.0f, 0.0f}, {s, c, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f});
}
[[nodiscard]] constexpr Mat4 rotation(const Quat& q) noexcept {
    return q.to_mat4();
}

// Translate * Rotate * Scale: scale in local space first, then orient, then place.
[[nodiscard]] constexpr Mat4 trs(const Vec3& t, const Quat& r, const Vec3& s) noexcept {
    Mat4 m = Mat4::from_mat3(r.to_mat3() * Mat3::scaling(s));
    m.cols[3] = {t, 1.0f};
    return m;
}

// Point: translation applies (w = 1). Vector: it does not (w = 0).
[[nodiscard]] constexpr Vec3 transform_point(const Mat4& m, const Vec3& p) noexcept {
    return (m * Vec4{p, 1.0f}).xyz();
}
[[nodiscard]] constexpr Vec3 transform_vector(const Mat4& m, const Vec3& v) noexcept {
    return (m * Vec4{v, 0.0f}).xyz();
}
// Full projective transform with the perspective divide: clip -> NDC.
[[nodiscard]] constexpr Vec3 project_point(const Mat4& m, const Vec3& p) noexcept {
    const Vec4 clip = m * Vec4{p, 1.0f};
    return clip.w != 0.0f ? clip.xyz() / clip.w : clip.xyz();
}

// View matrix for a camera at `eye` looking at `target`. In view space the
// camera sits at the origin looking down -z with `up` roughly +y.
[[nodiscard]] inline Mat4 look_at(const Vec3& eye, const Vec3& target, const Vec3& up) noexcept {
    const Vec3 f = normalize(target - eye); // forward
    const Vec3 s = normalize(cross(f, up)); // right
    const Vec3 u = cross(s, f);             // true up
    return Mat4::from_rows({s.x, s.y, s.z, -dot(s, eye)}, {u.x, u.y, u.z, -dot(u, eye)},
                           {-f.x, -f.y, -f.z, dot(f, eye)}, {0.0f, 0.0f, 0.0f, 1.0f});
}

// Perspective projection, depth 0 at `near` and 1 at `far`. `fov_y` is the
// full vertical field of view in radians.
[[nodiscard]] inline Mat4 perspective(float fov_y, float aspect, float near, float far) noexcept {
    const float f = 1.0f / std::tan(0.5f * fov_y);
    return Mat4::from_rows({f / aspect, 0.0f, 0.0f, 0.0f}, {0.0f, f, 0.0f, 0.0f},
                           {0.0f, 0.0f, far / (near - far), (near * far) / (near - far)},
                           {0.0f, 0.0f, -1.0f, 0.0f});
}

// Reverse-Z with no far plane: depth 1 at `near`, approaching 0 at infinity.
// Float depth buffers keep far more precision this way — the renderer's
// default once it has a depth buffer. Pair it with a GREATER depth test and a
// clear value of 0.
[[nodiscard]] inline Mat4 perspective_infinite_reverse_z(float fov_y, float aspect, float near) noexcept {
    const float f = 1.0f / std::tan(0.5f * fov_y);
    return Mat4::from_rows({f / aspect, 0.0f, 0.0f, 0.0f}, {0.0f, f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, near},
                           {0.0f, 0.0f, -1.0f, 0.0f});
}

// Orthographic projection, depth 0 at `near` and 1 at `far`; for 2D and shadow maps.
[[nodiscard]] constexpr Mat4 orthographic(float left, float right, float bottom, float top, float near,
                                          float far) noexcept {
    return Mat4::from_rows({2.0f / (right - left), 0.0f, 0.0f, -(right + left) / (right - left)},
                           {0.0f, 2.0f / (top - bottom), 0.0f, -(top + bottom) / (top - bottom)},
                           {0.0f, 0.0f, 1.0f / (near - far), near / (near - far)}, {0.0f, 0.0f, 0.0f, 1.0f});
}

} // namespace tynima::math
