#include <tynima/render/camera.h>

#include <cmath>

namespace tynima::render {

void Camera::look_at(const math::Vec3& target, const math::Vec3& up) noexcept {
    // math::look_at builds world->view; the camera's pose is the inverse, and
    // for a pure rotation the inverse is the transpose.
    const math::Mat4 view = math::look_at(position, target, up);
    const math::Mat3 world_rotation = math::transpose(view.upper3x3());
    // Column-major: the columns are the camera's world-space right, up, back axes.
    const math::Vec3 r = world_rotation.cols[0];
    const math::Vec3 u = world_rotation.cols[1];
    const math::Vec3 b = world_rotation.cols[2];
    // Rotation matrix -> quaternion (Shepperd's method, largest-diagonal branch).
    const float trace = r.x + u.y + b.z;
    math::Quat q;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(u.z - b.y) / s, (b.x - r.z) / s, (r.y - u.x) / s, 0.25f * s};
    } else if (r.x > u.y && r.x > b.z) {
        const float s = std::sqrt(1.0f + r.x - u.y - b.z) * 2.0f;
        q = {0.25f * s, (u.x + r.y) / s, (b.x + r.z) / s, (u.z - b.y) / s};
    } else if (u.y > b.z) {
        const float s = std::sqrt(1.0f + u.y - r.x - b.z) * 2.0f;
        q = {(u.x + r.y) / s, 0.25f * s, (b.y + u.z) / s, (b.x - r.z) / s};
    } else {
        const float s = std::sqrt(1.0f + b.z - r.x - u.y) * 2.0f;
        q = {(b.x + r.z) / s, (b.y + u.z) / s, 0.25f * s, (r.y - u.x) / s};
    }
    rotation = math::normalize(q);
}

} // namespace tynima::render
