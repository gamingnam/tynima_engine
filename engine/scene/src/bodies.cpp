#include <tynima/scene/systems.h>

#include <tynima/core/profile.h>

namespace tynima::scene {

void update_bodies(World& world, const physics::PhysicsWorld& physics) noexcept {
    TY_PROFILE_SCOPE_NAMED("scene::update_bodies");
    world.each<Transform, RigidBody>([&](Entity, Transform& transform, RigidBody& body) {
        if (!physics.valid(body.body)) {
            return;
        }
        const physics::BodyState state = physics.body_state(body.body);
        transform.position = state.position;
        transform.rotation = state.rotation;
    });
}

} // namespace tynima::scene
