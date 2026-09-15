#pragma once

#include <tynima/physics/physics.h>
#include <tynima/scene/components.h>
#include <tynima/scene/world.h>

namespace tynima::scene {

// Copies the position and rotation of every entity's RigidBody from
// `physics` into its Transform (scale is left alone). Run it after
// physics.step() and before update_transforms(). An entity whose body is
// gone from the world is skipped, not destroyed.
//
// `alpha` below 1 draws the entity that fraction of the way from the pose
// record_previous_poses() saved to the body's current one — the render
// interpolation of a fixed timestep (physics::FixedStepper::alpha()). An
// entity with no recorded pose yet is drawn where its body is.
void update_bodies(World& world, const physics::PhysicsWorld& physics, float alpha = 1.0f) noexcept;

// Saves every RigidBody's current pose as the one to interpolate from. Run it
// right before each physics step (FixedStepper's before_step).
void record_previous_poses(World& world, const physics::PhysicsWorld& physics) noexcept;

// Computes LocalToWorld for every entity that has one and a Transform,
// composing through Parent chains. A parent without its own Transform counts
// as identity. Cycles are cut at a fixed depth.
void update_transforms(World& world) noexcept;

} // namespace tynima::scene
