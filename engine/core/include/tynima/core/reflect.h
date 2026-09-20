#pragma once

#include <tynima/core/handle.h>
#include <tynima/core/math.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

// Reflection, the plain way: a struct lists its fields beside its
// definition, and the list is what an inspector, a scene file or a script
// binding reads instead of the C++ type. Macros rather than codegen — no
// build step, nothing generated to check in, and the compiler still checks
// every name (a field that does not exist fails to compile, and its type
// and offset come from decltype and offsetof, never from a second copy).
// The cost is that the list is written by hand; it is one line.
//
//   struct Transform {
//       math::Vec3 position;
//       math::Quat rotation;
//       math::Vec3 scale;
//   };
//   TY_REFLECT(Transform, TY_FIELD(position), TY_FIELD(rotation), TY_FIELD(scale));
//
// TY_REFLECT goes in the struct's own namespace (it is found by argument-
// dependent lookup) and takes the struct's unqualified name. TY_FIELD_FLAGS
// marks a field ReadOnly (shown, never edited) or Hidden (an
// implementation detail). A field of a type this file does not know shows
// as bytes: describe it as Bytes and give it an editor later, or add a
// FieldTraits specialization for its type where the type is declared.
namespace tynima::core {

enum class FieldKind : std::uint8_t {
    Bool,
    Int8,
    Uint8,
    Int16,
    Uint16,
    Int32,
    Uint32,
    Int64,
    Uint64,
    Float,
    Double,
    Vec2,
    Vec3,
    Vec4,
    Quat,
    Mat4,
    Entity, // a scene::Entity: an inspector offers to select it
    Handle, // any other generational handle: shown, never edited
    String, // a char array of `count` bytes, NUL-terminated
    Bytes,  // anything else: `size` opaque bytes
};
[[nodiscard]] const char* field_kind_name(FieldKind kind) noexcept;

enum FieldFlags : std::uint32_t {
    kFieldReadOnly = 1u << 0,
    kFieldHidden = 1u << 1,
};

struct FieldInfo {
    const char* name = nullptr;
    FieldKind kind = FieldKind::Bytes;
    std::uint32_t offset = 0; // bytes from the start of the struct
    std::uint32_t size = 0;   // bytes
    std::uint32_t count = 1;  // a String's capacity; 1 otherwise
    std::uint32_t flags = 0;  // FieldFlags
};

struct TypeInfo {
    const FieldInfo* fields = nullptr;
    std::uint32_t count = 0;
};

// What a field's type is to reflection. The primary template is the
// fallback: bytes. Specialize it beside a type that deserves better.
template <typename T> struct FieldTraits {
    static constexpr FieldKind kind = FieldKind::Bytes;
    static constexpr std::uint32_t count = 1;
};
#define TY_FIELD_TRAITS(Type, Kind)                                                                          \
    template <> struct FieldTraits<Type> {                                                                   \
        static constexpr FieldKind kind = FieldKind::Kind;                                                   \
        static constexpr std::uint32_t count = 1;                                                            \
    }
TY_FIELD_TRAITS(bool, Bool);
TY_FIELD_TRAITS(std::int8_t, Int8);
TY_FIELD_TRAITS(std::uint8_t, Uint8);
TY_FIELD_TRAITS(std::int16_t, Int16);
TY_FIELD_TRAITS(std::uint16_t, Uint16);
TY_FIELD_TRAITS(std::int32_t, Int32);
TY_FIELD_TRAITS(std::uint32_t, Uint32);
TY_FIELD_TRAITS(std::int64_t, Int64);
TY_FIELD_TRAITS(std::uint64_t, Uint64);
TY_FIELD_TRAITS(float, Float);
TY_FIELD_TRAITS(double, Double);
TY_FIELD_TRAITS(math::Vec2, Vec2);
TY_FIELD_TRAITS(math::Vec3, Vec3);
TY_FIELD_TRAITS(math::Vec4, Vec4);
TY_FIELD_TRAITS(math::Quat, Quat);
TY_FIELD_TRAITS(math::Mat4, Mat4);
#undef TY_FIELD_TRAITS
template <typename Tag> struct FieldTraits<Handle<Tag>> {
    static constexpr FieldKind kind = FieldKind::Handle;
    static constexpr std::uint32_t count = 1;
};
template <std::size_t N> struct FieldTraits<char[N]> {
    static constexpr FieldKind kind = FieldKind::String;
    static constexpr std::uint32_t count = static_cast<std::uint32_t>(N);
};

template <typename T>
[[nodiscard]] constexpr FieldInfo field_of(const char* name, std::size_t offset,
                                           std::uint32_t flags = 0) noexcept {
    return FieldInfo{.name = name,
                     .kind = FieldTraits<T>::kind,
                     .offset = static_cast<std::uint32_t>(offset),
                     .size = static_cast<std::uint32_t>(sizeof(T)),
                     .count = FieldTraits<T>::count,
                     .flags = flags};
}

// A type is reflected when a tynima_reflect(const T*) is found for it — the
// function TY_REFLECT defines in the type's namespace.
template <typename T>
concept Reflected = requires(const T* pointer) {
    { tynima_reflect(pointer) } -> std::convertible_to<TypeInfo>;
};

// The fields of a reflected type; an empty TypeInfo for any other.
template <typename T> [[nodiscard]] constexpr TypeInfo type_info() noexcept {
    if constexpr (Reflected<T>) {
        return tynima_reflect(static_cast<const T*>(nullptr));
    } else {
        return {};
    }
}

} // namespace tynima::core

#define TY_FIELD(member)                                                                                     \
    ::tynima::core::field_of<decltype(ReflectedType::member)>(#member, offsetof(ReflectedType, member))
#define TY_FIELD_FLAGS(member, flags)                                                                        \
    ::tynima::core::field_of<decltype(ReflectedType::member)>(#member, offsetof(ReflectedType, member),      \
                                                              (flags))

#define TY_REFLECT(Type, ...)                                                                                \
    struct TynimaReflect_##Type {                                                                            \
        using ReflectedType = Type;                                                                          \
        static constexpr ::tynima::core::FieldInfo fields[] = {__VA_ARGS__};                                 \
    };                                                                                                       \
    [[maybe_unused]] constexpr ::tynima::core::TypeInfo tynima_reflect(const Type*) noexcept {               \
        return {TynimaReflect_##Type::fields,                                                                \
                static_cast<std::uint32_t>(sizeof(TynimaReflect_##Type::fields) /                            \
                                           sizeof(::tynima::core::FieldInfo))};                              \
    }
