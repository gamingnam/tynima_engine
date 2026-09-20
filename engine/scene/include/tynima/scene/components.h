#pragma once

#include <tynima/core/math.h>
#include <tynima/physics/physics.h>
#include <tynima/scene/entity.h>

#include <cstdint>

// The engine's built-in components. Plain data with a name — the World copies
// them with memcpy and identifies them by kName — and a list of fields
// (TY_REFLECT, core/reflect.h) that the World keeps beside the layout, so
// the editor's inspector shows every one of them without knowing any.
// Header-only on purpose: a hot-reloaded game module includes this without
// linking the engine.
namespace tynima::scene {

// What an entity is called, for people: the hierarchy panel, a scene file.
// Fixed storage, so it is plain data like every component; longer names
// are cut. Never required — an entity without one is "entity N".
struct Name {
    static constexpr const char* kName = "Name";
    static constexpr std::uint32_t kCapacity = 32; // bytes, including the terminator
    char text[kCapacity] = {};

    Name() = default;
    explicit Name(const char* value) noexcept { set(value); }
    void set(const char* value) noexcept {
        std::uint32_t i = 0;
        for (; value != nullptr && value[i] != '\0' && i < kCapacity - 1; ++i) {
            text[i] = value[i];
        }
        text[i] = '\0';
    }
};
TY_REFLECT(Name, TY_FIELD(text));

// Where an entity is, relative to its Parent (or the world when it has none).
struct Transform {
    static constexpr const char* kName = "Transform";
    math::Vec3 position{0.0f};
    math::Quat rotation = math::Quat::identity();
    math::Vec3 scale{1.0f};

    [[nodiscard]] math::Mat4 matrix() const noexcept { return math::trs(position, rotation, scale); }
};
TY_REFLECT(Transform, TY_FIELD(position), TY_FIELD(rotation), TY_FIELD(scale));

// The entity's world matrix, written by update_transforms(). Add it to every
// entity that has a Transform and is drawn or otherwise needs its world pose.
struct LocalToWorld {
    static constexpr const char* kName = "LocalToWorld";
    math::Mat4 matrix = math::Mat4::identity();
};
TY_REFLECT(LocalToWorld, TY_FIELD_FLAGS(matrix, core::kFieldReadOnly));

// Makes the entity's Transform relative to another entity's.
struct Parent {
    static constexpr const char* kName = "Parent";
    Entity entity;
};
TY_REFLECT(Parent, TY_FIELD(entity));

// Draws a model — an index into whatever model list the application keeps —
// at the entity's LocalToWorld.
struct MeshRenderer {
    static constexpr const char* kName = "MeshRenderer";
    std::uint32_t model = 0;
    bool visible = true;
};
TY_REFLECT(MeshRenderer, TY_FIELD(model), TY_FIELD(visible));

// Ties the entity to a body in the application's PhysicsWorld. The body
// drives the Transform: update_bodies() copies its pose in after every step.
// The handle is only meaningful in the world that made it.
//
// With a fixed timestep the frame is rarely on a step boundary, so the pose
// drawn is a blend: record_previous_poses() keeps where the body was before
// the last step, and update_bodies(world, physics, alpha) draws alpha of the
// way from there to where it is now.
struct RigidBody {
    static constexpr const char* kName = "RigidBody";
    physics::BodyHandle body;
    math::Vec3 previous_position{0.0f};
    math::Quat previous_rotation = math::Quat::identity();
    bool has_previous = false; // until the first record_previous_poses()
};
TY_REFLECT(RigidBody, TY_FIELD_FLAGS(body, core::kFieldReadOnly),
           TY_FIELD_FLAGS(previous_position, core::kFieldHidden),
           TY_FIELD_FLAGS(previous_rotation, core::kFieldHidden),
           TY_FIELD_FLAGS(has_previous, core::kFieldHidden));

} // namespace tynima::scene
