#pragma once

#include <tynima/core/assert.h>
#include <tynima/core/handle.h>
#include <tynima/core/jobs.h>
#include <tynima/scene/entity.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace tynima::scene {

using ComponentId = std::uint32_t;
inline constexpr std::uint32_t kMaxComponentTypes = 64;
inline constexpr std::size_t kChunkBytes = 16 * 1024;

// Components are identified by name, not by C++ type identity, so code
// compiled into a separately loaded library (hot reload) agrees with the
// engine about what a "Transform" is. Compile-time FNV-1a of that name.
[[nodiscard]] constexpr std::uint64_t component_name_hash(const char* name) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (; *name != '\0'; ++name) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(*name));
        hash *= 1099511628211ull;
    }
    return hash;
}

// Everything the World needs to know about a component type: its name (for
// identity) and its layout (for storage). Components are plain data —
// trivially copyable, no destructors — because the World moves them with
// memcpy as entities change archetype.
struct ComponentInfo {
    const char* name = nullptr;
    std::uint64_t name_hash = 0;
    std::uint32_t size = 0;
    std::uint32_t alignment = 0;
};

template <typename T>
concept Component = std::is_trivially_copyable_v<T> && requires {
    { T::kName } -> std::convertible_to<const char*>;
};

// One chunk's worth of one archetype, as a query sees it: `count` entities
// and a pointer to each requested column.
struct ChunkView {
    const Entity* entities = nullptr;
    std::uint32_t count = 0;
    void* columns[kMaxComponentTypes] = {}; // in the query's component order
};

// The entity store: archetype-sorted, chunked, struct-of-arrays.
//
// Structural changes (create, destroy, add, remove) are not allowed while a
// query is running: they move rows the query is walking. Do them before or
// after, or collect what to change and apply it afterwards.
class World {
public:
    explicit World(std::uint32_t max_entities = 65536);
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // ---- component types ----

    // Registers by name; calling again with the same name returns the same id.
    ComponentId register_component(const ComponentInfo& info);

    template <Component T>
    [[nodiscard]] ComponentId component_id() {
        return register_component({T::kName, component_name_hash(T::kName), sizeof(T), alignof(T)});
    }
    [[nodiscard]] std::uint32_t component_type_count() const noexcept { return component_count_; }
    [[nodiscard]] const ComponentInfo& component_info(ComponentId id) const noexcept { return infos_[id]; }
    // The id registered under `name`, or kNoComponent: a question, never a registration.
    static constexpr ComponentId kNoComponent = 0xFFFFFFFFu;
    [[nodiscard]] ComponentId find_component(const char* name) const noexcept;

    // ---- entities ----

    template <Component... Ts>
    [[nodiscard]] Entity create(const Ts&... components) {
        const ComponentId ids[] = {component_id<Ts>()...};
        const void* const values[] = {static_cast<const void*>(&components)...};
        return create_raw(ids, values, sizeof...(Ts));
    }
    [[nodiscard]] Entity create() { return create_raw(nullptr, nullptr, 0); }

    bool destroy(Entity entity) noexcept;
    [[nodiscard]] bool alive(Entity entity) const noexcept;
    [[nodiscard]] std::uint32_t entity_count() const noexcept;

    // ---- components on an entity ----

    // nullptr when the entity is dead or lacks the component. The pointer is
    // good until the next structural change.
    template <Component T>
    [[nodiscard]] T* get(Entity entity) noexcept {
        return static_cast<T*>(get_raw(entity, component_id<T>()));
    }
    template <Component T>
    [[nodiscard]] bool has(Entity entity) noexcept {
        return get_raw(entity, component_id<T>()) != nullptr;
    }
    // False if the entity is dead or already has the component.
    template <Component T>
    bool add(Entity entity, const T& value) {
        return add_raw(entity, component_id<T>(), &value);
    }
    // False if the entity is dead or lacks the component.
    template <Component T>
    bool remove(Entity entity) {
        return remove_raw(entity, component_id<T>());
    }

    // ---- queries ----

    // fn(Entity, Ts&...) for every entity that has all of Ts.
    template <Component... Ts, typename Fn>
    void each(Fn&& fn) {
        const ComponentId ids[sizeof...(Ts) + 1] = {component_id<Ts>()...}; // +1: Ts may be empty
        for_each_chunk_raw(ids, sizeof...(Ts), [&](const ChunkView& view) {
            for (std::uint32_t i = 0; i < view.count; ++i) {
                each_call<Ts...>(fn, view, i, std::index_sequence_for<Ts...>{});
            }
        });
    }

    // fn(std::span<const Entity>, Ts*...) per chunk: the raw contiguous
    // arrays, for code that wants to vectorize or index directly.
    template <Component... Ts, typename Fn>
    void each_chunk(Fn&& fn) {
        const ComponentId ids[sizeof...(Ts) + 1] = {component_id<Ts>()...};
        for_each_chunk_raw(ids, sizeof...(Ts), [&](const ChunkView& view) {
            chunk_call<Ts...>(fn, view, std::index_sequence_for<Ts...>{});
        });
    }

    // each_chunk spread over the job system, one job per chunk. fn runs on
    // several threads at once; different chunks never share memory.
    template <Component... Ts, typename Fn>
    void parallel_each_chunk(core::JobSystem& jobs, Fn&& fn) {
        const ComponentId ids[sizeof...(Ts) + 1] = {component_id<Ts>()...};
        const std::uint32_t chunk_total = matching_chunk_count(ids, sizeof...(Ts));
        if (chunk_total == 0) {
            return;
        }
        jobs.parallel_for(chunk_total, 1, [&](std::uint32_t begin, std::uint32_t end) {
            for_each_chunk_raw(ids, sizeof...(Ts), begin, end, [&](const ChunkView& view) {
                chunk_call<Ts...>(fn, view, std::index_sequence_for<Ts...>{});
            });
        });
    }

    // The component ids an entity has, ascending, into `out` (at most `max`
    // of them); returns how many it has in all, 0 for a dead entity. An
    // inspector's question: the World's, so a tool never guesses layouts.
    [[nodiscard]] std::uint32_t entity_components(Entity entity, ComponentId* out,
                                                  std::uint32_t max) const noexcept;

    // ---- type-erased entry points, for the C API ----
    // The templates above are thin wrappers over these.

    [[nodiscard]] Entity create_raw_public(const ComponentId* ids, const void* const* values, std::uint32_t count) {
        return create_raw(ids, values, count);
    }
    [[nodiscard]] void* get_raw_public(Entity entity, ComponentId id) noexcept { return get_raw(entity, id); }
    bool add_raw_public(Entity entity, ComponentId id, const void* value) { return add_raw(entity, id, value); }
    bool remove_raw_public(Entity entity, ComponentId id) { return remove_raw(entity, id); }

    // Every chunk whose archetype has all `count` components, columns in the order of `ids`.
    void each_chunk_raw(const ComponentId* ids, std::uint32_t count, void (*callback)(void*, const ChunkView&),
                        void* user) {
        visit_chunks_erased(ids, count, 0, 0xFFFFFFFFu, callback, user);
    }

    // Storage statistics: how many archetypes and chunks exist.
    [[nodiscard]] std::uint32_t archetype_count() const noexcept;
    [[nodiscard]] std::uint32_t chunk_count() const noexcept;

private:
    struct Archetype;
    struct EntityRecord {
        std::uint32_t archetype;
        std::uint32_t chunk;
        std::uint32_t row;
    };

    Entity create_raw(const ComponentId* ids, const void* const* values, std::uint32_t count);
    [[nodiscard]] void* get_raw(Entity entity, ComponentId id) noexcept;
    bool add_raw(Entity entity, ComponentId id, const void* value);
    bool remove_raw(Entity entity, ComponentId id);

    [[nodiscard]] std::uint32_t matching_chunk_count(const ComponentId* ids, std::uint32_t count) const noexcept;
    // Visits chunks [first, last) in global order among the matching
    // archetypes; the templates above erase their callable into this.
    template <typename Fn>
    void for_each_chunk_raw(const ComponentId* ids, std::uint32_t count, std::uint32_t first, std::uint32_t last,
                            Fn&& fn) {
        using Callable = std::remove_reference_t<Fn>;
        visit_chunks_erased(
            ids, count, first, last,
            [](void* user, const ChunkView& view) { (*static_cast<Callable*>(user))(view); }, &fn);
    }
    template <typename Fn>
    void for_each_chunk_raw(const ComponentId* ids, std::uint32_t count, Fn&& fn) {
        for_each_chunk_raw(ids, count, 0, 0xFFFFFFFFu, std::forward<Fn>(fn));
    }
    void visit_chunks_erased(const ComponentId* ids, std::uint32_t count, std::uint32_t first, std::uint32_t last,
                             void (*callback)(void*, const ChunkView&), void* user);

    template <Component... Ts, typename Fn, std::size_t... I>
    static void each_call(Fn& fn, const ChunkView& view, std::uint32_t i, std::index_sequence<I...>) {
        fn(view.entities[i], static_cast<Ts*>(view.columns[I])[i]...);
    }
    template <Component... Ts, typename Fn, std::size_t... I>
    static void chunk_call(Fn& fn, const ChunkView& view, std::index_sequence<I...>) {
        fn(std::span<const Entity>{view.entities, view.count}, static_cast<Ts*>(view.columns[I])...);
    }

    std::uint32_t find_or_create_archetype(std::uint64_t mask);
    void allocate_row(std::uint32_t archetype_index, std::uint32_t& chunk, std::uint32_t& row);
    void remove_row(std::uint32_t archetype_index, std::uint32_t chunk, std::uint32_t row) noexcept;

    core::HandlePool<EntityRecord, EntityTag> entities_;
    std::vector<std::unique_ptr<Archetype>> archetypes_;
    ComponentInfo infos_[kMaxComponentTypes] = {};
    std::uint32_t component_count_ = 0;
    std::atomic<std::uint32_t> iterating_{0}; // structural changes are refused while > 0
};

} // namespace tynima::scene
