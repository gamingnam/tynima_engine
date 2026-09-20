#pragma once

#include <tynima/core/handle.h>
#include <tynima/core/reflect.h>

// The entity handle on its own, so component headers — plain data that game
// modules include too — depend on nothing that needs linking.
namespace tynima::scene {

struct EntityTag {};
using Entity = core::Handle<EntityTag>;

} // namespace tynima::scene

// To reflection an entity handle is its own kind: an inspector offers to
// select what it points at.
namespace tynima::core {
template <>
struct FieldTraits<scene::Entity> {
    static constexpr FieldKind kind = FieldKind::Entity;
    static constexpr std::uint32_t count = 1;
};
} // namespace tynima::core
