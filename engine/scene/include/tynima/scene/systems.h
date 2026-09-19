#pragma once

#include <tynima/physics/physics.h>
#include <tynima/render/scene_renderer.h>
#include <tynima/scene/components.h>
#include <tynima/scene/world.h>

#include <cstdint>

namespace tynima::scene {

// Every entity with a LocalToWorld and a visible MeshRenderer whose model
// index is below `model_count`, as a draw item for the scene renderer, into
// `out` (at most `max` of them); returns how many there were in all. Run
// it after update_transforms(). Items come out grouped by archetype, so a
// model's draws mostly follow one another and its mesh binds once.
std::uint32_t collect_draws(World& world, const render::Model* models, std::uint32_t model_count,
                            render::DrawItem* out, std::uint32_t max) noexcept;

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
