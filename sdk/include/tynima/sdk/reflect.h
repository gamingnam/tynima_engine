#pragma once

#include <tynima.h>

#include <tynima/core/reflect.h>

#include <cstdint>

// For a game module written in C++ against the table: registers a component
// type with the engine and, when the type is reflected (TY_REFLECT), describes
// its fields through the C API, so the editor's inspector shows them. Header
// only — a module links nothing from the engine.
namespace tynima::sdk {

template <typename T> tynima_component_id register_component(const tynima_api& api, tynima_engine* engine) {
    const tynima_component_id id = api.register_component(engine, T::kName, sizeof(T), alignof(T));
    if constexpr (core::Reflected<T>) {
        constexpr core::TypeInfo type = core::type_info<T>();
        tynima_field fields[TYNIMA_MAX_COMPONENT_FIELDS];
        const std::uint32_t count =
            type.count < TYNIMA_MAX_COMPONENT_FIELDS ? type.count : TYNIMA_MAX_COMPONENT_FIELDS;
        for (std::uint32_t i = 0; i < count; ++i) {
            const core::FieldInfo& field = type.fields[i];
            fields[i] = tynima_field{field.name,   static_cast<tynima_field_kind>(field.kind),
                                     field.offset, field.size,
                                     field.count,  field.flags};
        }
        (void)api.describe_component(engine, id, fields, count);
        (void)api.set_component_defaults(engine, id, type.defaults);
    }
    return id;
}

} // namespace tynima::sdk
