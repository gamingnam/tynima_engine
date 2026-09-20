#pragma once

#include <tynima/core/math/vec.h>

#include <cmath>

namespace tynima::math {

// An axis-aligned box, closed on every side: two boxes that share a face
// overlap. An empty() box has min > max on some axis and overlaps nothing.
struct Aabb {
    Vec3 min{0.0f};
    Vec3 max{0.0f};

    [[nodiscard]] static constexpr Aabb empty() noexcept {
        return {Vec3{3.402823466e+38f}, Vec3{-3.402823466e+38f}};
    }
    [[nodiscard]] static constexpr Aabb from_center(const Vec3& center, const Vec3& half_extents) noexcept {
        return {center - half_extents, center + half_extents};
    }

    [[nodiscard]] constexpr Vec3 center() const noexcept { return (min + max) * 0.5f; }
    [[nodiscard]] constexpr Vec3 extent() const noexcept { return max - min; }
    [[nodiscard]] constexpr bool is_empty() const noexcept {
        return min.x > max.x || min.y > max.y || min.z > max.z;
    }

    [[nodiscard]] constexpr bool overlaps(const Aabb& o) const noexcept {
        return min.x <= o.max.x && o.min.x <= max.x && min.y <= o.max.y && o.min.y <= max.y &&
               min.z <= o.max.z && o.min.z <= max.z;
    }
    [[nodiscard]] constexpr bool contains(const Aabb& o) const noexcept {
        return min.x <= o.min.x && min.y <= o.min.y && min.z <= o.min.z && o.max.x <= max.x &&
               o.max.y <= max.y && o.max.z <= max.z;
    }
    [[nodiscard]] constexpr bool contains(const Vec3& p) const noexcept {
        return min.x <= p.x && p.x <= max.x && min.y <= p.y && p.y <= max.y && min.z <= p.z && p.z <= max.z;
    }

    // The smallest box holding both.
    [[nodiscard]] constexpr Aabb merged(const Aabb& o) const noexcept {
        return {math::min(min, o.min), math::max(max, o.max)};
    }
    [[nodiscard]] constexpr Aabb expanded(float margin) const noexcept {
        return {min - Vec3{margin}, max + Vec3{margin}};
    }

    // Twice the sum of the face areas: the cost a bounding-volume tree
    // minimises, since it is proportional to how many rays or boxes hit it.
    [[nodiscard]] constexpr float surface_area() const noexcept {
        const Vec3 d = max - min;
        return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
    }

    // Slab test: does the segment origin + direction * t, t in [0, max_t],
    // pass through the box? `direction` need not be unit length. The second
    // form also says where it enters (t of the first face crossed; 0 when
    // the origin is inside).
    [[nodiscard]] bool intersects_ray(const Vec3& origin, const Vec3& direction, float max_t) const noexcept {
        float t_enter = 0.0f;
        return intersects_ray(origin, direction, max_t, t_enter);
    }
    [[nodiscard]] bool intersects_ray(const Vec3& origin, const Vec3& direction, float max_t,
                                      float& t_enter) const noexcept {
        float t_near = 0.0f, t_far = max_t;
        const float o[3] = {origin.x, origin.y, origin.z};
        const float d[3] = {direction.x, direction.y, direction.z};
        const float lo[3] = {min.x, min.y, min.z};
        const float hi[3] = {max.x, max.y, max.z};
        for (int i = 0; i < 3; ++i) {
            if (std::fabs(d[i]) < 1e-12f) {
                if (o[i] < lo[i] || o[i] > hi[i]) {
                    return false; // parallel and outside the slab
                }
                continue;
            }
            const float inv = 1.0f / d[i];
            float t0 = (lo[i] - o[i]) * inv;
            float t1 = (hi[i] - o[i]) * inv;
            if (t0 > t1) {
                const float tmp = t0;
                t0 = t1;
                t1 = tmp;
            }
            t_near = t0 > t_near ? t0 : t_near;
            t_far = t1 < t_far ? t1 : t_far;
            if (t_near > t_far) {
                return false;
            }
        }
        t_enter = t_near;
        return true;
    }

    friend constexpr bool operator==(const Aabb& a, const Aabb& b) noexcept = default;
};

} // namespace tynima::math
