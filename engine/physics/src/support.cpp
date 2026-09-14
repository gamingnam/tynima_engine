// Support mappings: what GJK and EPA know about a shape — the farthest point
// along a direction — plus the face that faces a direction, for manifolds.
#include <tynima/physics/collision.h>

#include <algorithm>
#include <cmath>

namespace tynima::physics {

namespace {

Vec3 abs_each(const Vec3& v) noexcept {
    return Vec3{std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)};
}

} // namespace

Vec3 Convex::support(const Vec3& direction) const noexcept {
    const Vec3 d = pose.direction_to_local(direction);
    Vec3 local = shape.center;
    switch (shape.type) {
    case ShapeType::Box: {
        const Vec3& h = shape.half_extents;
        local += Vec3{d.x >= 0.0f ? h.x : -h.x, d.y >= 0.0f ? h.y : -h.y, d.z >= 0.0f ? h.z : -h.z};
        break;
    }
    case ShapeType::Sphere:
        break; // the core is the centre
    case ShapeType::Capsule:
        local.y += d.y >= 0.0f ? shape.half_height : -shape.half_height;
        break;
    }
    return pose.to_world(local);
}

float Convex::radius() const noexcept {
    return shape.type == ShapeType::Box ? 0.0f : shape.radius;
}

std::uint32_t Convex::supporting_face(const Vec3& direction, Vec3 out[4]) const noexcept {
    const Vec3 d = pose.direction_to_local(direction);
    switch (shape.type) {
    case ShapeType::Box: {
        const Vec3& h = shape.half_extents;
        const Vec3 a = abs_each(d);
        // The face whose normal is most aligned with the direction: fix that
        // axis at its extreme, walk the other two around the face.
        Vec3 corners[4];
        if (a.x >= a.y && a.x >= a.z) {
            const float x = d.x >= 0.0f ? h.x : -h.x;
            corners[0] = {x, -h.y, -h.z};
            corners[1] = {x, h.y, -h.z};
            corners[2] = {x, h.y, h.z};
            corners[3] = {x, -h.y, h.z};
        } else if (a.y >= a.z) {
            const float y = d.y >= 0.0f ? h.y : -h.y;
            corners[0] = {-h.x, y, -h.z};
            corners[1] = {h.x, y, -h.z};
            corners[2] = {h.x, y, h.z};
            corners[3] = {-h.x, y, h.z};
        } else {
            const float z = d.z >= 0.0f ? h.z : -h.z;
            corners[0] = {-h.x, -h.y, z};
            corners[1] = {h.x, -h.y, z};
            corners[2] = {h.x, h.y, z};
            corners[3] = {-h.x, h.y, z};
        }
        for (int i = 0; i < 4; ++i) {
            out[i] = pose.to_world(shape.center + corners[i]);
        }
        return 4;
    }
    case ShapeType::Capsule: {
        // The axis segment, unless the direction runs along it: then the
        // contact is at one end, a single point the caller already has.
        const float along = std::fabs(d.y) / std::sqrt(std::max(dot(d, d), 1e-20f));
        if (along > 0.99f) {
            return 0;
        }
        out[0] = pose.to_world(shape.center + Vec3{0.0f, -shape.half_height, 0.0f});
        out[1] = pose.to_world(shape.center + Vec3{0.0f, shape.half_height, 0.0f});
        return 2;
    }
    case ShapeType::Sphere:
        break;
    }
    return 0;
}

math::Aabb Convex::bounds() const noexcept {
    const Vec3 center = pose.to_world(shape.center);
    switch (shape.type) {
    case ShapeType::Box: {
        // Each local axis contributes |axis| * extent to the world extent.
        const Vec3 x = abs_each(pose.rotation.rotate(Vec3::unit_x())) * shape.half_extents.x;
        const Vec3 y = abs_each(pose.rotation.rotate(Vec3::unit_y())) * shape.half_extents.y;
        const Vec3 z = abs_each(pose.rotation.rotate(Vec3::unit_z())) * shape.half_extents.z;
        return math::Aabb::from_center(center, x + y + z);
    }
    case ShapeType::Sphere:
        return math::Aabb::from_center(center, Vec3{shape.radius});
    case ShapeType::Capsule: {
        const Vec3 half_axis = abs_each(pose.rotation.rotate(Vec3::unit_y())) * shape.half_height;
        return math::Aabb::from_center(center, half_axis + Vec3{shape.radius});
    }
    }
    return math::Aabb::from_center(center, Vec3{0.0f});
}

} // namespace tynima::physics
