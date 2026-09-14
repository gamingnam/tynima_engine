#include <tynima/scene/systems.h>

#include <tynima/core/profile.h>

namespace tynima::scene {

namespace {

constexpr int kMaxDepth = 64;

// Walks up the Parent chain and composes. O(depth) per entity, which keeps
// the system order-independent: no need to sort entities by depth first.
math::Mat4 world_matrix(World& world, Entity entity, int depth) noexcept {
    const Transform* transform = world.get<Transform>(entity);
    const math::Mat4 local = transform != nullptr ? transform->matrix() : math::Mat4::identity();
    const Parent* parent = world.get<Parent>(entity);
    if (parent == nullptr || depth >= kMaxDepth || !world.alive(parent->entity)) {
        return local;
    }
    return world_matrix(world, parent->entity, depth + 1) * local;
}

} // namespace

void update_transforms(World& world) noexcept {
    TY_PROFILE_SCOPE_NAMED("scene::update_transforms");
    world.each<Transform, LocalToWorld>([&](Entity entity, Transform& transform, LocalToWorld& out) {
        const Parent* parent = world.get<Parent>(entity);
        if (parent == nullptr || !world.alive(parent->entity)) {
            out.matrix = transform.matrix();
        } else {
            out.matrix = world_matrix(world, parent->entity, 1) * transform.matrix();
        }
    });
}

} // namespace tynima::scene
