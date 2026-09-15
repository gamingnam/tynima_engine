#include <tynima/core/profile.h>
#include <tynima/scene/systems.h>

namespace tynima::scene {

void update_bodies(World& world, const physics::PhysicsWorld& physics, float alpha) noexcept {
    TY_PROFILE_SCOPE_NAMED("scene::update_bodies");
    world.each<Transform, RigidBody>([&](Entity, Transform& transform, RigidBody& body) {
        if (!physics.valid(body.body)) {
            return;
        }
        const physics::BodyState state = physics.body_state(body.body);
        if (body.has_previous && alpha < 1.0f) {
            transform.position = math::lerp(body.previous_position, state.position, alpha);
            transform.rotation = math::slerp(body.previous_rotation, state.rotation, alpha);
        } else {
            transform.position = state.position;
            transform.rotation = state.rotation;
        }
    });
}

void record_previous_poses(World& world, const physics::PhysicsWorld& physics) noexcept {
    TY_PROFILE_SCOPE_NAMED("scene::record_previous_poses");
    world.each<RigidBody>([&](Entity, RigidBody& body) {
        if (!physics.valid(body.body)) {
            return;
        }
        const physics::BodyState state = physics.body_state(body.body);
        body.previous_position = state.position;
        body.previous_rotation = state.rotation;
        body.has_previous = true;
    });
}

} // namespace tynima::scene
