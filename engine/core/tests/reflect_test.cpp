#include <tynima/core/handle.h>
#include <tynima/core/reflect.h>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>

using namespace tynima;
using namespace tynima::math;

namespace {

struct Plain {
    int x = 0;
};

namespace sample {

struct WidgetTag {};
using WidgetHandle = core::Handle<WidgetTag>;

struct Probe {
    bool on = false;
    std::int32_t count = 0;
    float weight = 1.0f;
    Vec3 position;
    Quat rotation;
    char label[16] = "";
    WidgetHandle widget;
    Plain other; // no traits: bytes
    std::uint64_t secret = 0;
};
TY_REFLECT(Probe, TY_FIELD(on), TY_FIELD(count), TY_FIELD(weight), TY_FIELD(position), TY_FIELD(rotation),
           TY_FIELD(label), TY_FIELD_FLAGS(widget, core::kFieldReadOnly), TY_FIELD(other),
           TY_FIELD_FLAGS(secret, core::kFieldHidden));

} // namespace sample
} // namespace

TEST_CASE("a reflected struct lists its fields with their kinds, offsets and sizes") {
    using sample::Probe;
    static_assert(core::Reflected<Probe>);
    static_assert(!core::Reflected<Plain>);
    constexpr core::TypeInfo type = core::type_info<Probe>();
    static_assert(type.count == 9);
    CHECK(core::type_info<Plain>().count == 0);
    CHECK(core::type_info<Plain>().fields == nullptr);

    const core::FieldInfo* f = type.fields;
    CHECK(std::string(f[0].name) == "on");
    CHECK(f[0].kind == core::FieldKind::Bool);
    CHECK(f[0].offset == offsetof(Probe, on));
    CHECK(f[0].size == sizeof(bool));
    CHECK(f[1].kind == core::FieldKind::Int32);
    CHECK(f[2].kind == core::FieldKind::Float);
    CHECK(f[2].offset == offsetof(Probe, weight));
    CHECK(f[3].kind == core::FieldKind::Vec3);
    CHECK(f[3].size == 12);
    CHECK(f[4].kind == core::FieldKind::Quat);
    CHECK(f[5].kind == core::FieldKind::String);
    CHECK(f[5].count == 16);
    CHECK(f[5].size == 16);
    CHECK(f[6].kind == core::FieldKind::Handle);
    CHECK(f[6].flags == core::kFieldReadOnly);
    CHECK(f[7].kind == core::FieldKind::Bytes);
    CHECK(f[7].size == sizeof(Plain));
    CHECK(f[8].kind == core::FieldKind::Uint64);
    CHECK(f[8].flags == core::kFieldHidden);
    // Every field lies inside the struct, in declaration order.
    std::uint32_t last_end = 0;
    for (std::uint32_t i = 0; i < type.count; ++i) {
        CHECK(f[i].offset >= last_end);
        CHECK(f[i].offset + f[i].size <= sizeof(Probe));
        last_end = f[i].offset + f[i].size;
    }
    CHECK(std::string(core::field_kind_name(core::FieldKind::Quat)) == "quat");
    CHECK(std::string(core::field_kind_name(core::FieldKind::Bytes)) == "bytes");
}
