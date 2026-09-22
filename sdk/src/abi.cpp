// The ABI, pinned. A game module is a separate binary, built at another
// time, possibly by another compiler: everything it and the engine agree on
// is in tynima.h, and every one of those agreements is asserted here. This
// file has no code — it fails the build when the public header changes in a
// way that would break a module already built.
//
// The rule it enforces: the table only grows at the end, and nothing in it
// ever changes. Each version's block of entries is pinned by the offset of
// its first entry, so inserting a function in the middle — which would
// silently shift every later entry a module calls — is caught here, and the
// fix is to move the addition to the end and take the next version number.
// Every public struct is pinned the same way: the offset of every field, so
// reordering or resizing one is caught rather than discovered as a module
// reading the wrong bytes.
//
// When you do mean to change one of these numbers: bump TYNIMA_API_VERSION,
// add the version to the history at the top of tynima.h, and change the
// number here in the same commit. When the number surprises you, do not
// change it.
#include <tynima.h>

#include <cstddef>

namespace {

// The table is a version word and then function pointers, in order. On any
// ABI the engine targets a pointer is at least as wide as uint32_t and the
// word is padded out to one, so the nth entry sits at (n + 1) pointers in
// and the whole table is (entries + 1) pointers wide. Counting in pointers
// rather than bytes is what makes these numbers the same on 32 and 64 bits.
constexpr std::size_t kEntry = sizeof(void*);
constexpr std::size_t entry_at(std::size_t index) {
    return kEntry * (index + 1);
}

// How many entries each version of the table added, and where its block
// begins. These numbers are history: they never change.
constexpr std::size_t kEntriesV1 = 16; // components, entities, chunks, keys, time, impulses, log
constexpr std::size_t kEntriesV4 = 12; // component queries, the mouse, body poses
constexpr std::size_t kEntriesV5 = 3;  // a component's fields
constexpr std::size_t kEntriesV6 = 2;  // a component's defaults
constexpr std::size_t kEntriesV9 = 15; // the camera, the light, models, bodies, rays, the window
constexpr std::size_t kEntries = kEntriesV1 + kEntriesV4 + kEntriesV5 + kEntriesV6 + kEntriesV9;

// Version 10 added no entries: it says the engine runs Lua against this
// same table, which is a promise about the engine, not about the table.
static_assert(TYNIMA_API_VERSION == 10u, "a new version means a new block of entries below");
static_assert(TYNIMA_API_VERSION_MIN <= TYNIMA_API_VERSION);
static_assert(sizeof(tynima_api) == kEntry * (kEntries + 1), "the table grew or shrank somewhere");
static_assert(offsetof(tynima_api, version) == 0, "the version word is what a module reads first");

// The first entry of each version's block: proof that the earlier blocks
// are the length they always were.
static_assert(offsetof(tynima_api, register_component) == entry_at(0));
static_assert(offsetof(tynima_api, component_count) == entry_at(kEntriesV1));
static_assert(offsetof(tynima_api, component_field_count) == entry_at(kEntriesV1 + kEntriesV4));
static_assert(offsetof(tynima_api, component_defaults) == entry_at(kEntriesV1 + kEntriesV4 + kEntriesV5));
static_assert(offsetof(tynima_api, quit) == entry_at(kEntriesV1 + kEntriesV4 + kEntriesV5 + kEntriesV6));
// And the last entry of the last block: the table ends where it should.
static_assert(offsetof(tynima_api, set_relative_mouse) == entry_at(kEntries - 1));

// What a module exports. This one cannot grow at all: the engine reads
// every field of it out of a module that may have been built against an
// older header, so a field added here would be read from memory the module
// never wrote. A future addition goes behind a new exported symbol instead.
static_assert(sizeof(tynima_game) == kEntry * 4, "tynima_game may never grow");
static_assert(offsetof(tynima_game, api_version) == 0, "the version is read before anything else");
static_assert(offsetof(tynima_game, load) == kEntry);
static_assert(offsetof(tynima_game, unload) == kEntry * 2);
static_assert(offsetof(tynima_game, update) == kEntry * 3);

// ---- the plain data that crosses the boundary ----
//
// Sizes in floats and bytes, and the offset of every field: a module passes
// these by value and by pointer, so a field that moved is a module reading
// another field's bytes.

static_assert(sizeof(tynima_vec3) == 12 && offsetof(tynima_vec3, z) == 8);
static_assert(sizeof(tynima_vec4) == 16 && offsetof(tynima_vec4, w) == 12);
static_assert(sizeof(tynima_quat) == 16 && offsetof(tynima_quat, w) == 12);
static_assert(sizeof(tynima_mat4) == 64);
static_assert(sizeof(tynima_entity) == 8 && offsetof(tynima_entity, generation) == 4);
static_assert(sizeof(tynima_body) == 8 && offsetof(tynima_body, generation) == 4);
static_assert(sizeof(tynima_version) == 6);

static_assert(sizeof(tynima_camera) == 36 && offsetof(tynima_camera, position) == 0 &&
              offsetof(tynima_camera, rotation) == 12 && offsetof(tynima_camera, fov_y) == 28 &&
              offsetof(tynima_camera, near) == 32);
static_assert(sizeof(tynima_lighting) == 44 && offsetof(tynima_lighting, sun_direction) == 0 &&
              offsetof(tynima_lighting, sun_intensity) == 12 && offsetof(tynima_lighting, sun_color) == 16 &&
              offsetof(tynima_lighting, ambient) == 28 && offsetof(tynima_lighting, sky) == 32);
static_assert(sizeof(tynima_point_light) == 28 && offsetof(tynima_point_light, radius) == 12 &&
              offsetof(tynima_point_light, color) == 16);
static_assert(sizeof(tynima_shape) == 36 && offsetof(tynima_shape, half_extents) == 4 &&
              offsetof(tynima_shape, radius) == 16 && offsetof(tynima_shape, half_height) == 20 &&
              offsetof(tynima_shape, center) == 24);
static_assert(sizeof(tynima_body_desc) == 120 && offsetof(tynima_body_desc, shape) == 0 &&
              offsetof(tynima_body_desc, position) == 36 && offsetof(tynima_body_desc, rotation) == 48 &&
              offsetof(tynima_body_desc, motion) == 64 && offsetof(tynima_body_desc, mass) == 68 &&
              offsetof(tynima_body_desc, linear_velocity) == 80 &&
              offsetof(tynima_body_desc, angular_velocity) == 92 &&
              offsetof(tynima_body_desc, start_active) == 104 &&
              offsetof(tynima_body_desc, lock_rotation) == 105 &&
              offsetof(tynima_body_desc, user_data) == 112);
static_assert(sizeof(tynima_ray_hit) == 48 && offsetof(tynima_ray_hit, position) == 8 &&
              offsetof(tynima_ray_hit, normal) == 20 && offsetof(tynima_ray_hit, distance) == 32 &&
              offsetof(tynima_ray_hit, user_data) == 40);

// The built-in components: what a module stores in the world, byte for
// byte. (api.cpp checks these against the engine's own C++ structs; here
// they are checked against the numbers this header has always promised.)
static_assert(sizeof(tynima_name) == TYNIMA_NAME_CAPACITY);
static_assert(sizeof(tynima_transform) == 40 && offsetof(tynima_transform, rotation) == 12 &&
              offsetof(tynima_transform, scale) == 28);
static_assert(sizeof(tynima_local_to_world) == 64);
static_assert(sizeof(tynima_parent) == 8);
static_assert(sizeof(tynima_mesh_renderer) == 8 && offsetof(tynima_mesh_renderer, visible) == 4);
static_assert(sizeof(tynima_rigid_body) == 40 && offsetof(tynima_rigid_body, previous_position) == 8 &&
              offsetof(tynima_rigid_body, previous_rotation) == 20 &&
              offsetof(tynima_rigid_body, has_previous) == 36);

// Reflection and the description a module gives its own components.
static_assert(sizeof(tynima_field) == 32 && offsetof(tynima_field, kind) == 8 &&
              offsetof(tynima_field, offset) == 12 && offsetof(tynima_field, size) == 16 &&
              offsetof(tynima_field, count) == 20 && offsetof(tynima_field, flags) == 24);
static_assert(sizeof(tynima_component_info) == 16 && offsetof(tynima_component_info, size) == 8);

// What a host reads back. These cross no module boundary, but a tool built
// against one version and run against another reads them the same way.
static_assert(sizeof(tynima_log_entry) == 24 + TYNIMA_LOG_CATEGORY_MAX + TYNIMA_LOG_MESSAGE_MAX);
static_assert(sizeof(tynima_render_settings) == 24);
static_assert(sizeof(tynima_stats) == 88);
static_assert(sizeof(tynima_engine_desc) == 56);

// The enumerations a module passes by value: their ranges are ABI too.
static_assert(sizeof(tynima_key) == sizeof(int) && TYNIMA_KEY_COUNT > 0);
static_assert(TYNIMA_MOUSE_BUTTON_COUNT == 5);
static_assert(TYNIMA_LOG_FATAL == 5);
static_assert(TYNIMA_SHAPE_CAPSULE == 2 && TYNIMA_MOTION_DYNAMIC == 2);
static_assert(TYNIMA_MAX_LIGHTS == 256u);
static_assert(TYNIMA_NO_MODEL == 0xFFFFFFFFu);
static_assert(TYNIMA_NO_COMPONENT == 0xFFFFFFFFu);

} // namespace
