#pragma once

#include <tynima/scene/components.h>
#include <tynima/scene/world.h>

namespace tynima::scene {

// Computes LocalToWorld for every entity that has one and a Transform,
// composing through Parent chains. A parent without its own Transform counts
// as identity. Cycles are cut at a fixed depth.
void update_transforms(World& world) noexcept;

} // namespace tynima::scene
