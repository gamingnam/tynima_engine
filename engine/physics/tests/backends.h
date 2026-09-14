#pragma once

#include <tynima/physics/physics.h>

#include <memory>

// Every scene in these tests runs through both implementations of
// PhysicsWorld: Jolt, the reference, and the engine's own. Same calls, same
// expectations.
namespace {

constexpr float kStep = 1.0f / 60.0f;

using Factory = std::unique_ptr<tynima::physics::PhysicsWorld> (*)(const tynima::physics::WorldDesc&);

struct Backend {
    const char* name;
    Factory make;
};

const Backend kBackends[] = {
    {"Jolt", &tynima::physics::create_jolt_world},
    {"Tynima", &tynima::physics::create_tynima_world},
};

// A 20 m square slab whose top face is y = 0.
inline tynima::physics::BodyHandle add_floor(tynima::physics::PhysicsWorld& world) {
    using namespace tynima::physics;
    BodyDesc floor;
    floor.shape = Shape::box(tynima::math::Vec3{10.0f, 0.5f, 10.0f});
    floor.position = tynima::math::Vec3{0.0f, -0.5f, 0.0f};
    floor.motion = MotionType::Static;
    return world.create_body(floor);
}

inline void run(tynima::physics::PhysicsWorld& world, float seconds) {
    for (float t = 0.0f; t < seconds; t += kStep) {
        world.step(kStep);
    }
}

} // namespace
