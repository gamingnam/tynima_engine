// The C API, implemented against the engine. Every function takes the
// tynima_engine the host filled in and forwards to the World, the Input and
// the clock it points at.
#include <tynima.h>

#include <tynima/core/log.h>
#include <tynima/scene/world.h>
#include <tynima/sdk/game_module.h>

namespace {

using tynima::core::LogLevel;
using tynima::scene::ChunkView;
using tynima::scene::Entity;

static_assert(sizeof(tynima_entity) == sizeof(Entity), "tynima_entity mirrors scene::Entity");
static_assert(static_cast<int>(TYNIMA_KEY_COUNT) == static_cast<int>(tynima::platform::Key::Count),
              "tynima_key mirrors platform::Key");
static_assert(static_cast<int>(TYNIMA_KEY_Escape) == static_cast<int>(tynima::platform::Key::Escape));
static_assert(static_cast<int>(TYNIMA_KEY_RightSuper) == static_cast<int>(tynima::platform::Key::RightSuper));
static_assert(static_cast<int>(TYNIMA_LOG_TRACE) == static_cast<int>(LogLevel::Trace) &&
                  static_cast<int>(TYNIMA_LOG_FATAL) == static_cast<int>(LogLevel::Fatal),
              "tynima_log_level mirrors core::LogLevel");

Entity to_entity(tynima_entity e) noexcept {
    return Entity{e.index, e.generation};
}
tynima_entity from_entity(Entity e) noexcept {
    return tynima_entity{e.index, e.generation};
}
tynima::platform::Key to_key(tynima_key key) noexcept {
    return static_cast<tynima::platform::Key>(key);
}

tynima_component_id api_register_component(tynima_engine* engine, const char* name, uint32_t size,
                                           uint32_t alignment) {
    return engine->world->register_component(
        {name, tynima::scene::component_name_hash(name), size, alignment});
}

tynima_entity api_create_entity(tynima_engine* engine, const tynima_component_id* ids, const void* const* values,
                                uint32_t count) {
    // The template create() is a thin wrapper over this same raw path; the C
    // API reaches it through a friend-free route: register everything through
    // component ids and let the World build the archetype.
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

bool api_add_component(tynima_engine* engine, tynima_entity entity, tynima_component_id id, const void* value) {
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

const tynima_api kApi{
    TYNIMA_API_VERSION,     api_register_component, api_create_entity, api_destroy_entity, api_entity_alive,
    api_entity_count,       api_get_component,      api_add_component, api_remove_component, api_each_chunk,
    api_key_down,           api_key_pressed,        api_key_released,  api_time_seconds,     api_log,
};

} // namespace

namespace tynima::sdk {

const tynima_api& api() noexcept {
    return kApi;
}

} // namespace tynima::sdk
