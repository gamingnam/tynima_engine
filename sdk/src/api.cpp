// The C API, implemented against the engine. Every function takes the
// tynima_engine the host filled in and forwards to the World, the Input and
// the clock it points at; the host half of tynima.h forwards to the Runtime
// behind the same context.
#include <tynima.h>

#include <tynima/cooker/cook.h>
#include <tynima/core/log.h>
#include <tynima/physics/physics.h>
#include <tynima/platform/platform.h>
#include <tynima/scene/components.h>
#include <tynima/scene/scene_file.h>
#include <tynima/scene/world.h>
#include <tynima/sdk/game_module.h>
#include <tynima/sdk/runtime.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <string>

namespace {

using tynima::core::LogLevel;
using tynima::physics::BodyHandle;
using tynima::scene::ChunkView;
using tynima::scene::Entity;
using tynima::sdk::Runtime;

static_assert(sizeof(tynima_entity) == sizeof(Entity), "tynima_entity mirrors scene::Entity");
static_assert(sizeof(tynima_body) == sizeof(BodyHandle), "tynima_body mirrors physics::BodyHandle");
static_assert(static_cast<int>(TYNIMA_KEY_COUNT) == static_cast<int>(tynima::platform::Key::Count),
              "tynima_key mirrors platform::Key");
static_assert(static_cast<int>(TYNIMA_KEY_Escape) == static_cast<int>(tynima::platform::Key::Escape));
static_assert(static_cast<int>(TYNIMA_KEY_RightSuper) == static_cast<int>(tynima::platform::Key::RightSuper));
static_assert(static_cast<int>(TYNIMA_MOUSE_BUTTON_COUNT) ==
                  static_cast<int>(tynima::platform::MouseButton::Count),
              "tynima_mouse_button mirrors platform::MouseButton");
static_assert(static_cast<int>(TYNIMA_MOUSE_RIGHT) == static_cast<int>(tynima::platform::MouseButton::Right));
static_assert(static_cast<int>(TYNIMA_LOG_TRACE) == static_cast<int>(LogLevel::Trace) &&
                  static_cast<int>(TYNIMA_LOG_FATAL) == static_cast<int>(LogLevel::Fatal),
              "tynima_log_level mirrors core::LogLevel");

// The math types: the same floats in the same places.
static_assert(sizeof(tynima_vec3) == sizeof(tynima::math::Vec3) &&
              alignof(tynima_vec3) == alignof(tynima::math::Vec3));
static_assert(sizeof(tynima_quat) == sizeof(tynima::math::Quat) && offsetof(tynima_quat, w) == 12);
static_assert(sizeof(tynima_mat4) == sizeof(tynima::math::Mat4));

// The built-in components, as C sees them: the same bytes as the engine's own.
using tynima::scene::LocalToWorld;
using tynima::scene::MeshRenderer;
using tynima::scene::Name;
using tynima::scene::Parent;
using tynima::scene::RigidBody;
using tynima::scene::Transform;
static_assert(sizeof(tynima_name) == sizeof(Name) && TYNIMA_NAME_CAPACITY == Name::kCapacity);
static_assert(sizeof(tynima_transform) == sizeof(Transform) &&
              offsetof(tynima_transform, position) == offsetof(Transform, position) &&
              offsetof(tynima_transform, rotation) == offsetof(Transform, rotation) &&
              offsetof(tynima_transform, scale) == offsetof(Transform, scale));
static_assert(sizeof(tynima_local_to_world) == sizeof(LocalToWorld));
static_assert(sizeof(tynima_parent) == sizeof(Parent));
static_assert(sizeof(tynima_mesh_renderer) == sizeof(MeshRenderer) &&
              offsetof(tynima_mesh_renderer, model) == offsetof(MeshRenderer, model) &&
              offsetof(tynima_mesh_renderer, visible) == offsetof(MeshRenderer, visible));
static_assert(sizeof(tynima_rigid_body) == sizeof(RigidBody) &&
              offsetof(tynima_rigid_body, body) == offsetof(RigidBody, body) &&
              offsetof(tynima_rigid_body, previous_position) == offsetof(RigidBody, previous_position) &&
              offsetof(tynima_rigid_body, previous_rotation) == offsetof(RigidBody, previous_rotation) &&
              offsetof(tynima_rigid_body, has_previous) == offsetof(RigidBody, has_previous));
static_assert(TYNIMA_MAX_COMPONENT_TYPES == tynima::scene::kMaxComponentTypes);
static_assert(TYNIMA_MAX_COMPONENT_FIELDS == tynima::scene::kMaxComponentFields);

// A field, as reflection keeps it and as C sees it: the same enum values
// and the same flags, so the two convert by cast.
using tynima::core::FieldInfo;
using tynima::core::FieldKind;
static_assert(static_cast<int>(TYNIMA_FIELD_BOOL) == static_cast<int>(FieldKind::Bool) &&
              static_cast<int>(TYNIMA_FIELD_QUAT) == static_cast<int>(FieldKind::Quat) &&
              static_cast<int>(TYNIMA_FIELD_ENTITY) == static_cast<int>(FieldKind::Entity) &&
              static_cast<int>(TYNIMA_FIELD_BYTES) == static_cast<int>(FieldKind::Bytes));
static_assert(TYNIMA_FIELD_READ_ONLY == tynima::core::kFieldReadOnly &&
              TYNIMA_FIELD_HIDDEN == tynima::core::kFieldHidden);

// The rendering choices, as the enums the engine keeps them in.
static_assert(static_cast<int>(TYNIMA_SHADING_SPLIT) == static_cast<int>(tynima::render::ShadingPath::Split));
static_assert(static_cast<int>(TYNIMA_MODEL_COOK_TORRANCE) ==
              static_cast<int>(tynima::render::ShadingModel::CookTorrance));
static_assert(static_cast<int>(TYNIMA_TONEMAP_AGX) == static_cast<int>(tynima::render::Tonemap::Agx));
static_assert(static_cast<int>(TYNIMA_AA_TAA) == static_cast<int>(tynima::render::AntiAliasing::Taa));
static_assert(TYNIMA_DEBUG_VIEW_COUNT == static_cast<std::uint32_t>(tynima::render::DebugView::Count));

Entity to_entity(tynima_entity e) noexcept {
    return Entity{e.index, e.generation};
}
tynima_entity from_entity(Entity e) noexcept {
    return tynima_entity{e.index, e.generation};
}
tynima::platform::Key to_key(tynima_key key) noexcept {
    return static_cast<tynima::platform::Key>(key);
}
tynima::platform::MouseButton to_button(tynima_mouse_button button) noexcept {
    return static_cast<tynima::platform::MouseButton>(button);
}
BodyHandle to_body(tynima_body body) noexcept {
    return BodyHandle{body.index, body.generation};
}
tynima::math::Vec3 to_vec3(tynima_vec3 v) noexcept {
    return tynima::math::Vec3{v.x, v.y, v.z};
}
tynima_vec3 from_vec3(tynima::math::Vec3 v) noexcept {
    return tynima_vec3{v.x, v.y, v.z};
}
tynima::math::Quat to_quat(tynima_quat q) noexcept {
    return tynima::math::Quat{q.x, q.y, q.z, q.w};
}
tynima_quat from_quat(tynima::math::Quat q) noexcept {
    return tynima_quat{q.x, q.y, q.z, q.w};
}

// ---- the table ----

tynima_component_id api_register_component(tynima_engine* engine, const char* name, uint32_t size,
                                           uint32_t alignment) {
    return engine->world->register_component(
        {name, tynima::scene::component_name_hash(name), size, alignment});
}

tynima_entity api_create_entity(tynima_engine* engine, const tynima_component_id* ids,
                                const void* const* values, uint32_t count) {
    // The template create() is a thin wrapper over this same raw path: the C
    // API names components by id and lets the World build the archetype.
    return from_entity(engine->world->create_raw_public(ids, values, count));
}

bool api_destroy_entity(tynima_engine* engine, tynima_entity entity) {
    return engine->world->destroy(to_entity(entity));
}

bool api_entity_alive(tynima_engine* engine, tynima_entity entity) {
    return engine->world->alive(to_entity(entity));
}

uint32_t api_entity_count(tynima_engine* engine) {
    return engine->world->entity_count();
}

void* api_get_component(tynima_engine* engine, tynima_entity entity, tynima_component_id id) {
    return engine->world->get_raw_public(to_entity(entity), id);
}

bool api_add_component(tynima_engine* engine, tynima_entity entity, tynima_component_id id,
                       const void* value) {
    return engine->world->add_raw_public(to_entity(entity), id, value);
}

bool api_remove_component(tynima_engine* engine, tynima_entity entity, tynima_component_id id) {
    return engine->world->remove_raw_public(to_entity(entity), id);
}

struct ChunkThunk {
    tynima_chunk_fn fn;
    void* user;
};

void api_each_chunk(tynima_engine* engine, const tynima_component_id* ids, uint32_t count, tynima_chunk_fn fn,
                    void* user) {
    ChunkThunk thunk{fn, user};
    engine->world->each_chunk_raw(
        ids, count,
        [](void* raw, const ChunkView& view) {
            const auto* t = static_cast<ChunkThunk*>(raw);
            t->fn(t->user, reinterpret_cast<const tynima_entity*>(view.entities), view.count, view.columns);
        },
        &thunk);
}

bool api_key_down(tynima_engine* engine, tynima_key key) {
    return engine->input != nullptr && engine->input->key_down(to_key(key));
}
bool api_key_pressed(tynima_engine* engine, tynima_key key) {
    return engine->input != nullptr && engine->input->key_pressed(to_key(key));
}
bool api_key_released(tynima_engine* engine, tynima_key key) {
    return engine->input != nullptr && engine->input->key_released(to_key(key));
}

double api_time_seconds(tynima_engine* engine) {
    return engine->time_seconds;
}

void api_body_add_impulse(tynima_engine* engine, tynima_body body, tynima_vec3 impulse) {
    if (engine->physics != nullptr) {
        engine->physics->add_impulse(to_body(body), to_vec3(impulse));
    }
}
void api_body_add_impulse_at(tynima_engine* engine, tynima_body body, tynima_vec3 impulse,
                             tynima_vec3 point) {
    if (engine->physics != nullptr) {
        engine->physics->add_impulse_at(to_body(body), to_vec3(impulse), to_vec3(point));
    }
}

void api_log(tynima_engine*, tynima_log_level level, const char* message) {
    // No source location: the call site is in the module, on the far side of
    // the ABI, and the module's own file names mean nothing to the engine.
    int severity = static_cast<int>(level);
    if (severity < static_cast<int>(TYNIMA_LOG_TRACE) || severity > static_cast<int>(TYNIMA_LOG_FATAL)) {
        severity = static_cast<int>(TYNIMA_LOG_INFO);
    }
    tynima::core::log_message(static_cast<LogLevel>(severity), "game", nullptr, 0, "%s",
                              message != nullptr ? message : "");
}

uint32_t api_component_count(tynima_engine* engine) {
    return engine->world->component_type_count();
}

tynima_component_id api_find_component(tynima_engine* engine, const char* name) {
    static_assert(TYNIMA_NO_COMPONENT == tynima::scene::World::kNoComponent);
    return engine->world->find_component(name);
}

bool api_component_info(tynima_engine* engine, tynima_component_id id, tynima_component_info* out) {
    if (id >= engine->world->component_type_count() || out == nullptr) {
        return false;
    }
    const tynima::scene::ComponentInfo& info = engine->world->component_info(id);
    out->name = info.name;
    out->size = info.size;
    out->alignment = info.alignment;
    return true;
}

uint32_t api_entity_components(tynima_engine* engine, tynima_entity entity, tynima_component_id* ids,
                               uint32_t max) {
    return engine->world->entity_components(to_entity(entity), ids, ids != nullptr ? max : 0);
}

bool api_mouse_down(tynima_engine* engine, tynima_mouse_button button) {
    return engine->input != nullptr && engine->input->mouse_down(to_button(button));
}
bool api_mouse_pressed(tynima_engine* engine, tynima_mouse_button button) {
    return engine->input != nullptr && engine->input->mouse_pressed(to_button(button));
}
bool api_mouse_released(tynima_engine* engine, tynima_mouse_button button) {
    return engine->input != nullptr && engine->input->mouse_released(to_button(button));
}
void api_mouse_position(tynima_engine* engine, float* x, float* y) {
    if (x)
        *x = engine->input != nullptr ? engine->input->mouse_x() : 0.0f;
    if (y)
        *y = engine->input != nullptr ? engine->input->mouse_y() : 0.0f;
}
void api_mouse_delta(tynima_engine* engine, float* dx, float* dy) {
    if (dx)
        *dx = engine->input != nullptr ? engine->input->mouse_dx() : 0.0f;
    if (dy)
        *dy = engine->input != nullptr ? engine->input->mouse_dy() : 0.0f;
}
void api_mouse_wheel(tynima_engine* engine, float* dx, float* dy) {
    if (dx)
        *dx = engine->input != nullptr ? engine->input->wheel_x() : 0.0f;
    if (dy)
        *dy = engine->input != nullptr ? engine->input->wheel_y() : 0.0f;
}

uint32_t api_component_field_count(tynima_engine* engine, tynima_component_id id) {
    return id < engine->world->component_type_count() ? engine->world->component_info(id).field_count : 0;
}

bool api_component_field(tynima_engine* engine, tynima_component_id id, uint32_t index, tynima_field* out) {
    if (id >= engine->world->component_type_count() || out == nullptr) {
        return false;
    }
    const tynima::scene::ComponentInfo& info = engine->world->component_info(id);
    if (index >= info.field_count) {
        return false;
    }
    const FieldInfo& field = info.fields[index];
    out->name = field.name;
    out->kind = static_cast<tynima_field_kind>(field.kind);
    out->offset = field.offset;
    out->size = field.size;
    out->count = field.count;
    out->flags = field.flags;
    return true;
}

bool api_describe_component(tynima_engine* engine, tynima_component_id id, const tynima_field* fields,
                            uint32_t count) {
    if (fields == nullptr || count == 0 || count > TYNIMA_MAX_COMPONENT_FIELDS) {
        return false;
    }
    FieldInfo converted[TYNIMA_MAX_COMPONENT_FIELDS];
    for (uint32_t i = 0; i < count; ++i) {
        if (fields[i].kind < TYNIMA_FIELD_BOOL || fields[i].kind > TYNIMA_FIELD_BYTES) {
            return false;
        }
        converted[i] = FieldInfo{.name = fields[i].name,
                                 .kind = static_cast<FieldKind>(fields[i].kind),
                                 .offset = fields[i].offset,
                                 .size = fields[i].size,
                                 .count = fields[i].count,
                                 .flags = fields[i].flags};
    }
    return engine->world->describe_component(id, converted, count);
}

const void* api_component_defaults(tynima_engine* engine, tynima_component_id id) {
    return id < engine->world->component_type_count() ? engine->world->component_info(id).defaults : nullptr;
}

bool api_set_component_defaults(tynima_engine* engine, tynima_component_id id, const void* defaults) {
    return engine->world->set_component_defaults(id, defaults);
}

void api_body_set_transform(tynima_engine* engine, tynima_body body, tynima_vec3 position,
                            tynima_quat rotation) {
    if (engine->physics != nullptr && engine->physics->valid(to_body(body))) {
        engine->physics->set_transform(to_body(body), to_vec3(position), to_quat(rotation));
    }
}
void api_body_set_velocity(tynima_engine* engine, tynima_body body, tynima_vec3 linear, tynima_vec3 angular) {
    if (engine->physics != nullptr && engine->physics->valid(to_body(body))) {
        engine->physics->set_velocity(to_body(body), to_vec3(linear), to_vec3(angular));
    }
}

const tynima_api kApi{
    TYNIMA_API_VERSION,
    api_register_component,
    api_create_entity,
    api_destroy_entity,
    api_entity_alive,
    api_entity_count,
    api_get_component,
    api_add_component,
    api_remove_component,
    api_each_chunk,
    api_key_down,
    api_key_pressed,
    api_key_released,
    api_time_seconds,
    api_body_add_impulse,
    api_body_add_impulse_at,
    api_log,
    api_component_count,
    api_find_component,
    api_component_info,
    api_entity_components,
    api_mouse_down,
    api_mouse_pressed,
    api_mouse_released,
    api_mouse_position,
    api_mouse_delta,
    api_mouse_wheel,
    api_body_set_transform,
    api_body_set_velocity,
    api_component_field_count,
    api_component_field,
    api_describe_component,
    api_component_defaults,
    api_set_component_defaults,
};

// ---- hosting ----

std::string g_last_error;

Runtime* runtime_of(tynima_engine* engine) noexcept {
    return engine != nullptr ? engine->runtime : nullptr;
}

} // namespace

namespace tynima::sdk {

const tynima_api& api() noexcept {
    return kApi;
}

} // namespace tynima::sdk

extern "C" {

tynima_engine* tynima_engine_create(const tynima_engine_desc* desc) {
    const tynima_engine_desc defaults{};
    if (desc == nullptr) {
        desc = &defaults;
    }
    tynima::sdk::RuntimeDesc runtime_desc;
    runtime_desc.title = desc->title != nullptr ? desc->title : "Tynima";
    runtime_desc.width = desc->width > 0 ? static_cast<int>(desc->width) : 1280;
    runtime_desc.height = desc->height > 0 ? static_cast<int>(desc->height) : 720;
    runtime_desc.headless = desc->headless;
    runtime_desc.game_module = desc->game_module;
    if (desc->max_entities > 0) {
        runtime_desc.max_entities = desc->max_entities;
    }
    if (desc->max_bodies > 0) {
        runtime_desc.max_bodies = desc->max_bodies;
    }
    if (desc->physics != nullptr && std::strcmp(desc->physics, "jolt") == 0) {
        runtime_desc.jolt = true;
    } else if (desc->physics != nullptr && std::strcmp(desc->physics, "tynima") != 0) {
        g_last_error = std::string("tynima_engine_create: physics must be \"tynima\" or \"jolt\", not \"") +
                       desc->physics + "\"";
        return nullptr;
    }
    if (desc->rhi != nullptr && std::strcmp(desc->rhi, "sdl") == 0) {
        runtime_desc.backend = tynima::rhi::Backend::SdlGpu;
    } else if (desc->rhi != nullptr && std::strcmp(desc->rhi, "metal") == 0) {
        runtime_desc.backend = tynima::rhi::Backend::Metal;
    } else if (desc->rhi != nullptr && std::strcmp(desc->rhi, "auto") != 0) {
        g_last_error =
            std::string("tynima_engine_create: rhi must be \"auto\", \"sdl\" or \"metal\", not \"") +
            desc->rhi + "\"";
        return nullptr;
    }
    auto runtime = std::make_unique<Runtime>();
    if (!runtime->create(runtime_desc)) {
        g_last_error = tynima::platform::last_error();
        return nullptr;
    }
    return &runtime.release()->context(); // owned through the context until tynima_engine_destroy()
}

void tynima_engine_destroy(tynima_engine* engine) {
    delete runtime_of(engine);
}

const char* tynima_last_error(void) {
    if (g_last_error.empty()) {
        return tynima::platform::last_error();
    }
    return g_last_error.c_str();
}

const tynima_api* tynima_engine_api(tynima_engine*) {
    return &kApi;
}

bool tynima_engine_begin_frame(tynima_engine* engine) {
    Runtime* runtime = runtime_of(engine);
    return runtime != nullptr && runtime->begin_frame();
}

void tynima_engine_end_frame(tynima_engine* engine) {
    if (Runtime* runtime = runtime_of(engine)) {
        runtime->end_frame();
    }
}

float tynima_engine_dt(tynima_engine* engine) {
    const Runtime* runtime = runtime_of(engine);
    return runtime != nullptr ? runtime->dt() : 0.0f;
}

double tynima_engine_time(tynima_engine* engine) {
    const Runtime* runtime = runtime_of(engine);
    return runtime != nullptr ? runtime->time() : 0.0;
}

uint64_t tynima_engine_frame_index(tynima_engine* engine) {
    const Runtime* runtime = runtime_of(engine);
    return runtime != nullptr ? static_cast<uint64_t>(runtime->frame_index()) : 0;
}

uint32_t tynima_load_model(tynima_engine* engine, const char* path) {
    Runtime* runtime = runtime_of(engine);
    return runtime != nullptr && path != nullptr ? runtime->load_model(path) : TYNIMA_NO_MODEL;
}

uint32_t tynima_model_count(tynima_engine* engine) {
    const Runtime* runtime = runtime_of(engine);
    return runtime != nullptr ? runtime->model_count() : 0;
}

bool tynima_cook_model(tynima_engine* engine, const char* source, const char* destination) {
    Runtime* runtime = runtime_of(engine);
    if (runtime == nullptr || source == nullptr || destination == nullptr) {
        g_last_error = "tynima_cook_model: no engine, no source or no destination";
        return false;
    }
    std::string error;
    if (!tynima::cooker::cook_model_file(source, destination, {.jobs = &runtime->jobs()}, error)) {
        g_last_error = error;
        return false;
    }
    return true;
}

bool tynima_model_bounds(tynima_engine* engine, uint32_t model, tynima_vec3* min, tynima_vec3* max) {
    const Runtime* runtime = runtime_of(engine);
    const tynima::render::Model* m = runtime != nullptr ? runtime->model(model) : nullptr;
    if (m == nullptr || m->mesh.index_count == 0) {
        return false;
    }
    if (min) *min = from_vec3(m->mesh.bounds_min);
    if (max) *max = from_vec3(m->mesh.bounds_max);
    return true;
}

bool tynima_pick(tynima_engine* engine, tynima_vec3 origin, tynima_vec3 direction, tynima_entity* out,
                 float* distance) {
    const Runtime* runtime = runtime_of(engine);
    if (runtime == nullptr) {
        return false;
    }
    float hit_distance = 0.0f;
    const Entity entity = runtime->pick(to_vec3(origin), to_vec3(direction), hit_distance);
    if (!entity) {
        return false;
    }
    if (out) *out = from_entity(entity);
    if (distance) *distance = hit_distance;
    return true;
}

bool tynima_save_scene(tynima_engine* engine, const char* path) {
    Runtime* runtime = runtime_of(engine);
    if (runtime == nullptr || path == nullptr) {
        g_last_error = "tynima_save_scene: no engine or no path";
        return false;
    }
    std::string error;
    if (!tynima::scene::save_scene_file(runtime->world(), path, error)) {
        g_last_error = error;
        return false;
    }
    return true;
}

bool tynima_load_scene(tynima_engine* engine, const char* path, bool clear) {
    Runtime* runtime = runtime_of(engine);
    if (runtime == nullptr || path == nullptr) {
        g_last_error = "tynima_load_scene: no engine or no path";
        return false;
    }
    std::string error;
    if (!tynima::scene::load_scene_file(runtime->world(), path, error, {.clear = clear})) {
        g_last_error = error;
        return false;
    }
    return true;
}

void tynima_clear_scene(tynima_engine* engine) {
    if (Runtime* runtime = runtime_of(engine)) {
        runtime->clear_scene();
    }
}

void tynima_get_camera(tynima_engine* engine, tynima_camera* out) {
    const Runtime* runtime = runtime_of(engine);
    if (runtime == nullptr || out == nullptr) {
        return;
    }
    out->position = from_vec3(runtime->camera.position);
    out->rotation = from_quat(runtime->camera.rotation);
    out->fov_y = runtime->camera.fov_y;
    out->near = runtime->camera.near;
}

void tynima_set_camera(tynima_engine* engine, const tynima_camera* camera) {
    Runtime* runtime = runtime_of(engine);
    if (runtime == nullptr || camera == nullptr) {
        return;
    }
    runtime->camera.position = to_vec3(camera->position);
    runtime->camera.rotation = tynima::math::normalize(to_quat(camera->rotation));
    if (camera->fov_y > 0.0f) {
        runtime->camera.fov_y = camera->fov_y;
    }
    if (camera->near > 0.0f) {
        runtime->camera.near = camera->near;
    }
}

void tynima_get_render_settings(tynima_engine* engine, tynima_render_settings* out) {
    Runtime* runtime = runtime_of(engine);
    if (runtime == nullptr || out == nullptr) {
        return;
    }
    const tynima::render::SceneSettings& scene = runtime->scene_settings;
    const tynima::render::PostSettings& post = runtime->post().settings;
    out->path = static_cast<tynima_shading_path>(scene.path);
    out->model = static_cast<tynima_shading_model>(scene.model);
    out->debug_view = static_cast<uint32_t>(scene.debug_view);
    out->shadows = scene.shadows;
    out->point_lights = scene.point_lights;
    out->bloom = post.bloom;
    out->tonemap = static_cast<tynima_tonemap>(post.tonemap);
    out->anti_aliasing = static_cast<tynima_anti_aliasing>(post.anti_aliasing);
}

void tynima_set_render_settings(tynima_engine* engine, const tynima_render_settings* settings) {
    Runtime* runtime = runtime_of(engine);
    if (runtime == nullptr || settings == nullptr) {
        return;
    }
    tynima::render::SceneSettings& scene = runtime->scene_settings;
    tynima::render::PostSettings& post = runtime->post().settings;
    if (settings->path <= TYNIMA_SHADING_SPLIT) {
        scene.path = static_cast<tynima::render::ShadingPath>(settings->path);
    }
    if (settings->model <= TYNIMA_MODEL_COOK_TORRANCE) {
        scene.model = static_cast<tynima::render::ShadingModel>(settings->model);
    }
    if (settings->debug_view < TYNIMA_DEBUG_VIEW_COUNT) {
        scene.debug_view = static_cast<tynima::render::DebugView>(settings->debug_view);
    }
    scene.shadows = settings->shadows;
    scene.point_lights = settings->point_lights;
    post.bloom = settings->bloom;
    if (settings->tonemap <= TYNIMA_TONEMAP_AGX) {
        post.tonemap = static_cast<tynima::render::Tonemap>(settings->tonemap);
    }
    if (settings->anti_aliasing <= TYNIMA_AA_TAA) {
        post.anti_aliasing = static_cast<tynima::render::AntiAliasing>(settings->anti_aliasing);
    }
}

const char* tynima_debug_view_name(uint32_t view) {
    if (view >= TYNIMA_DEBUG_VIEW_COUNT) {
        return "?";
    }
    return tynima::render::debug_view_name(static_cast<tynima::render::DebugView>(view));
}

void* tynima_ui_context(tynima_engine* engine) {
    Runtime* runtime = runtime_of(engine);
    return runtime != nullptr ? runtime->ui().context() : nullptr;
}

uint64_t tynima_ui_scene_texture(tynima_engine* engine, uint32_t width, uint32_t height) {
    Runtime* runtime = runtime_of(engine);
    return runtime != nullptr ? runtime->scene_texture(width, height) : 0;
}

float tynima_ui_pixel_density(tynima_engine* engine) {
    Runtime* runtime = runtime_of(engine);
    return runtime != nullptr && runtime->window() != nullptr ? runtime->window()->pixel_density() : 1.0f;
}

bool tynima_ui_wants_mouse(tynima_engine* engine) {
    Runtime* runtime = runtime_of(engine);
    return runtime != nullptr && runtime->ui().wants_mouse();
}

bool tynima_ui_wants_keyboard(tynima_engine* engine) {
    Runtime* runtime = runtime_of(engine);
    return runtime != nullptr && runtime->ui().wants_keyboard();
}

void tynima_set_relative_mouse(tynima_engine* engine, bool enabled) {
    Runtime* runtime = runtime_of(engine);
    if (runtime != nullptr && runtime->window() != nullptr) {
        runtime->window()->set_relative_mouse_mode(enabled);
    }
}

uint64_t tynima_log_count(tynima_engine* engine) {
    const Runtime* runtime = runtime_of(engine);
    return runtime != nullptr ? runtime->log_count() : 0;
}

bool tynima_log_get(tynima_engine* engine, uint64_t index, tynima_log_entry* out) {
    const Runtime* runtime = runtime_of(engine);
    return runtime != nullptr && out != nullptr && runtime->log_get(index, *out);
}

tynima_log_level tynima_log_level_get(void) {
    const LogLevel level = tynima::core::log_level();
    return level == LogLevel::Off ? TYNIMA_LOG_FATAL : static_cast<tynima_log_level>(level);
}

void tynima_log_level_set(tynima_log_level level) {
    if (level >= TYNIMA_LOG_TRACE && level <= TYNIMA_LOG_FATAL) {
        tynima::core::set_log_level(static_cast<LogLevel>(level));
    }
}

void tynima_get_stats(tynima_engine* engine, tynima_stats* out) {
    Runtime* runtime = runtime_of(engine);
    if (runtime == nullptr || out == nullptr) {
        return;
    }
    *out = tynima_stats{};
    const Runtime::Stats& stats = runtime->stats();
    out->frame_ms = stats.frame_ms;
    out->heap_allocations = stats.heap_allocations;
    out->game_reloads = stats.game_reloads;
    out->model_reloads = stats.model_reloads;
    if (tynima::rhi::Device* device = runtime->device()) {
        const tynima::rhi::Device::GpuStats gpu = device->gpu_stats();
        out->gpu_ms = static_cast<float>(gpu.frame_ms);
        out->gpu_bytes = gpu.allocated_bytes;
    }
    out->entities = runtime->world().entity_count();
    out->archetypes = runtime->world().archetype_count();
    out->bodies = runtime->physics().body_count();
    out->bodies_awake = runtime->physics().active_body_count();
    const tynima::render::FrameGraph::Stats& graph = runtime->graph().stats();
    out->graph_passes = graph.passes;
    out->graph_culled = graph.culled;
    out->graph_memoryless = graph.memoryless;
    out->graph_bytes = graph.bytes_allocated;
    out->graph_bytes_memoryless = graph.bytes_memoryless;
}

} // extern "C"
