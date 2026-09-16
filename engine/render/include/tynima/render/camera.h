#pragma once

#include <tynima/core/math.h>

namespace tynima::render {

// A pinhole camera posed in world space. The view matrix is the inverse of
// its pose; the projection is infinite reverse-Z (depth 1 at `near`, 0 at
// infinity), so pair it with a GREATER depth test and a depth clear of 0.
struct Camera {
    math::Vec3 position{0.0f, 0.0f, 0.0f};
    math::Quat rotation = math::Quat::identity(); // identity looks down -z
    float fov_y = math::radians(60.0f);
    float near = 0.05f; // metres; the smallest distance that still gets depth precision

    [[nodiscard]] math::Vec3 forward() const noexcept { return rotation.rotate(-math::Vec3::unit_z()); }
    [[nodiscard]] math::Vec3 right() const noexcept { return rotation.rotate(math::Vec3::unit_x()); }
    [[nodiscard]] math::Vec3 up() const noexcept { return rotation.rotate(math::Vec3::unit_y()); }

    // World -> view. Exact inverse of the rigid pose, no general matrix inverse needed.
    [[nodiscard]] math::Mat4 view() const noexcept {
        return math::rotation(math::conjugate(rotation)) * math::translation(-position);
    }
    // `jitter` shifts the image by that much in clip space (NDC units: two
    // across the frame) — temporal anti-aliasing's sub-pixel offset.
    [[nodiscard]] math::Mat4 projection(float aspect, math::Vec2 jitter = {}) const noexcept {
        math::Mat4 p = math::perspective_infinite_reverse_z(fov_y, aspect, near);
        // clip.w is -z, so clip.xy += jitter * clip.w is a -jitter on the z column.
        p(0, 2) -= jitter.x;
        p(1, 2) -= jitter.y;
        return p;
    }
    [[nodiscard]] math::Mat4 view_projection(float aspect, math::Vec2 jitter = {}) const noexcept {
        return projection(aspect, jitter) * view();
    }

    // Faces `target`, keeping the horizon level with `up`.
    void look_at(const math::Vec3& target, const math::Vec3& up = math::Vec3::unit_y()) noexcept;
};

} // namespace tynima::render
