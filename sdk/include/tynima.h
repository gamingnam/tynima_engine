/*
 * tynima.h — the Tynima public C ABI.
 *
 * Everything a game module may call is declared here, in C, so it can be
 * bound from any language and survive hot reload across a library boundary.
 * A game module never links the engine: on load it receives a tynima_api
 * table of function pointers and a tynima_engine context, and calls through
 * those. Engine internals (tynima/<module>/...) are never visible here.
 *
 * Phase 2: the world, keys, time and logging — enough for a game module to
 * own its systems. Grows with the SDK in Phase 6.
 *
 * Version history:
 *   1  components, entities, chunk iteration, keys, time, log
 *   2  log takes a level
 */
#ifndef TYNIMA_H
#define TYNIMA_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TYNIMA_API_VERSION 2u

/* ---- engine version ---- */

typedef struct tynima_version {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
} tynima_version;

tynima_version tynima_get_version(void);
const char* tynima_get_version_string(void); /* "major.minor.patch", static storage */

/* ---- world ---- */

/* An entity handle: index and generation, the same bits as scene::Entity.
 * Generation 0 is the null entity. */
typedef struct tynima_entity {
    uint32_t index;
    uint32_t generation;
} tynima_entity;

typedef uint32_t tynima_component_id;

/* Called once per chunk of entities: `count` entities, and for each
 * requested component a pointer to its contiguous array, in request order. */
typedef void (*tynima_chunk_fn)(void* user, const tynima_entity* entities, uint32_t count, void* const* columns);

/* ---- input ---- */

/* Physical keys; the values match the engine's platform::Key exactly. */
typedef enum tynima_key {
#define TYNIMA_KEY(name, scancode) TYNIMA_KEY_##name,
#include <tynima/platform/keys.def>
#undef TYNIMA_KEY
    TYNIMA_KEY_COUNT
} tynima_key;

/* ---- logging ---- */

/* Severity, matching the engine's core::LogLevel. Events below the engine's
 * minimum level (Debug in Debug builds, Info otherwise, or the TYNIMA_LOG
 * environment variable) are dropped. */
typedef enum tynima_log_level {
    TYNIMA_LOG_TRACE,
    TYNIMA_LOG_DEBUG,
    TYNIMA_LOG_INFO,
    TYNIMA_LOG_WARN,
    TYNIMA_LOG_ERROR,
    TYNIMA_LOG_FATAL
} tynima_log_level;

/* ---- the API a game module receives ---- */

/* Opaque: the host's engine state for this module. Pass it back unchanged. */
typedef struct tynima_engine tynima_engine;

typedef struct tynima_api {
    uint32_t version; /* TYNIMA_API_VERSION of the host */

    /* Components are identified by name; registering the same name again
     * returns the same id, and the layout must match. */
    tynima_component_id (*register_component)(tynima_engine* engine, const char* name, uint32_t size,
                                              uint32_t alignment);

    /* Entities. `values[i]` is copied as `size` bytes of component `ids[i]`.
     * A null entity comes back when the world is full. */
    tynima_entity (*create_entity)(tynima_engine* engine, const tynima_component_id* ids, const void* const* values,
                                   uint32_t count);
    bool (*destroy_entity)(tynima_engine* engine, tynima_entity entity);
    bool (*entity_alive)(tynima_engine* engine, tynima_entity entity);
    uint32_t (*entity_count)(tynima_engine* engine);

    /* NULL when the entity is dead or lacks the component; valid until the
     * next structural change. */
    void* (*get_component)(tynima_engine* engine, tynima_entity entity, tynima_component_id id);
    bool (*add_component)(tynima_engine* engine, tynima_entity entity, tynima_component_id id, const void* value);
    bool (*remove_component)(tynima_engine* engine, tynima_entity entity, tynima_component_id id);

    /* Iterates every chunk whose entities have all `count` components.
     * No structural changes from inside the callback. */
    void (*each_chunk)(tynima_engine* engine, const tynima_component_id* ids, uint32_t count, tynima_chunk_fn fn,
                       void* user);

    /* Input, as of the current frame. */
    bool (*key_down)(tynima_engine* engine, tynima_key key);
    bool (*key_pressed)(tynima_engine* engine, tynima_key key);
    bool (*key_released)(tynima_engine* engine, tynima_key key);

    /* Seconds since the engine started. */
    double (*time_seconds)(tynima_engine* engine);

    /* One line to the engine's log, in the "game" category. */
    void (*log)(tynima_engine* engine, tynima_log_level level, const char* message);
} tynima_api;

/* ---- what a game module exports ---- */

/* The module's entry points. Keep all state in the world: the module is
 * unloaded and reloaded while the engine — and the world — keep running. */
typedef struct tynima_game {
    uint32_t api_version; /* TYNIMA_API_VERSION the module was built against */
    /* After the module is loaded. `reloaded` is true after a hot reload. */
    void (*load)(const tynima_api* api, tynima_engine* engine, bool reloaded);
    /* Before the module is unloaded. `reloading` is true when a new build is about to replace it. */
    void (*unload)(const tynima_api* api, tynima_engine* engine, bool reloading);
    /* Once per frame. */
    void (*update)(const tynima_api* api, tynima_engine* engine, float dt);
} tynima_game;

#if defined(_WIN32)
#define TYNIMA_GAME_EXPORT __declspec(dllexport)
#else
#define TYNIMA_GAME_EXPORT __attribute__((visibility("default")))
#endif

/* The one symbol a game module must export:
 *   extern "C" TYNIMA_GAME_EXPORT const tynima_game* tynima_game_entry(void);
 */
#define TYNIMA_GAME_ENTRY_NAME "tynima_game_entry"

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TYNIMA_H */
