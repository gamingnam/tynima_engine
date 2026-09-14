#pragma once

#include <tynima/core/handle.h>

// The entity handle on its own, so component headers — plain data that game
// modules include too — depend on nothing that needs linking.
namespace tynima::scene {

struct EntityTag {};
using Entity = core::Handle<EntityTag>;

} // namespace tynima::scene
