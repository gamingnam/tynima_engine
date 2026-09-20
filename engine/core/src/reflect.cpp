#include <tynima/core/reflect.h>

namespace tynima::core {

const char* field_kind_name(FieldKind kind) noexcept {
    switch (kind) {
    case FieldKind::Bool:
        return "bool";
    case FieldKind::Int8:
        return "int8";
    case FieldKind::Uint8:
        return "uint8";
    case FieldKind::Int16:
        return "int16";
    case FieldKind::Uint16:
        return "uint16";
    case FieldKind::Int32:
        return "int32";
    case FieldKind::Uint32:
        return "uint32";
    case FieldKind::Int64:
        return "int64";
    case FieldKind::Uint64:
        return "uint64";
    case FieldKind::Float:
        return "float";
    case FieldKind::Double:
        return "double";
    case FieldKind::Vec2:
        return "vec2";
    case FieldKind::Vec3:
        return "vec3";
    case FieldKind::Vec4:
        return "vec4";
    case FieldKind::Quat:
        return "quat";
    case FieldKind::Mat4:
        return "mat4";
    case FieldKind::Entity:
        return "entity";
    case FieldKind::Handle:
        return "handle";
    case FieldKind::String:
        return "string";
    case FieldKind::Bytes:
        return "bytes";
    }
    return "?";
}

} // namespace tynima::core
