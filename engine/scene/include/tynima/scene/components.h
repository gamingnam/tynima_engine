#pragma once

#include <tynima/core/math.h>
#include <tynima/physics/physics.h>
#include <tynima/scene/entity.h>

#include <cstdint>

// The engine's built-in components. Plain data with a name — the World copies
// them with memcpy and identifies them by kName. Header-only on purpose: a
// hot-reloaded game module includes this without linking the engine.
namespace tynima::scene {

// Where an entity is, relative to its Parent (or the world when it has none).
struct Transform {
    static constexpr const char* kName = "Transform";
    math::Vec3 position{0.0f};
    math::Quat rotation = math::Quat::identity();
    math::Vec3 scale{1.0f};

    [[nodiscard]] math::Mat4 matrix() const noexcept { return math::trs(position, rotation, scale); }
};

// The entity's world matrix, written by update_transforms(). Add it to every
// entity that has a Transform and is drawn or otherwise needs its world pose.
struct LocalToWorld {
    static constexpr const char* kName = "LocalToWorld";
    math::Mat4 matrix = math::Mat4::identity();
};

// Makes the entity's Transform relative to another entity's.
struct Parent {
    static constexpr const char* kName = "Parent";
    Entity entity;
};

// Draws a model — an index into whatever model list the application keeps —
// at the entity's LocalToWorld.
struct MeshRenderer {
    static constexpr const char* kName = "MeshRenderer";
    std::uint32_t model = 0;
    bool visible = true;
};

// Ties the entity to a body in the application's PhysicsWorld. The body
// drives the Transform: update_bodies() copies its pose in after every step.
// The handle is only meaningful in the world that made it.
struct RigidBody {
    static constexpr const char* kName = "RigidBody";
    physics::BodyHandle body;
};

} // namespace tynima::scene
