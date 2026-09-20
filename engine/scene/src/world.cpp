#include <tynima/scene/world.h>

#include <tynima/core/profile.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

namespace tynima::scene {

namespace {

constexpr std::uint32_t kChunkAlignment = 64;

std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

std::uint64_t bit(ComponentId id) noexcept {
    return std::uint64_t{1} << id;
}

} // namespace

// A chunk is one 16 KB block: the entity array first, then each component's
// column at an aligned offset, all sized for `capacity` rows.
struct Chunk {
    std::byte* data = nullptr;
    std::uint32_t count = 0;

    Chunk() : data(static_cast<std::byte*>(::operator new(kChunkBytes, std::align_val_t{kChunkAlignment}))) {}
    ~Chunk() {
        if (data != nullptr) {
            ::operator delete(data, std::align_val_t{kChunkAlignment});
        }
    }
    Chunk(Chunk&& other) noexcept : data(std::exchange(other.data, nullptr)), count(other.count) {}
    Chunk& operator=(Chunk&&) = delete;
    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;

    [[nodiscard]] Entity* entities() noexcept { return reinterpret_cast<Entity*>(data); }
};

struct World::Archetype {
    std::uint64_t mask = 0;
    std::vector<ComponentId> components;      // ascending
    std::vector<std::uint32_t> column_offset; // per entry of `components`, bytes from the chunk start
    std::int32_t column_of[kMaxComponentTypes]; // id -> index into `components`, or -1
    std::uint32_t capacity = 0;               // rows per chunk
    std::vector<Chunk> chunks;
    std::uint32_t count = 0; // rows in use across all chunks

    [[nodiscard]] std::byte* column(std::uint32_t chunk, std::uint32_t col) noexcept {
        return chunks[chunk].data + column_offset[col];
    }
};

World::World(std::uint32_t max_entities) : entities_(max_entities) {
    for (ComponentInfo& info : infos_) {
        info = {};
    }
}

World::~World() = default;

ComponentId World::register_component(const ComponentInfo& info) {
    for (std::uint32_t i = 0; i < component_count_; ++i) {
        if (infos_[i].name_hash == info.name_hash) {
            TY_ASSERT(infos_[i].size == info.size && infos_[i].alignment == info.alignment,
                      "a component name was registered twice with different layouts");
            if (infos_[i].fields == nullptr && info.fields != nullptr) {
                keep_fields(i, info.fields, info.field_count); // the first registration was blind
            }
            if (infos_[i].defaults == nullptr && info.defaults != nullptr) {
                (void)set_component_defaults(i, info.defaults);
            }
            return i;
        }
    }
    TY_ASSERT(component_count_ < kMaxComponentTypes, "too many component types");
    TY_ASSERT(info.size > 0 && info.alignment > 0 && (info.alignment & (info.alignment - 1)) == 0,
              "component layout must have a size and a power-of-two alignment");
    if (storage_ == nullptr) {
        storage_ = std::make_unique<ComponentStorage[]>(kMaxComponentTypes);
    }
    const ComponentId id = component_count_++;
    ComponentStorage& storage = storage_[id];
    std::snprintf(storage.name, sizeof storage.name, "%s", info.name != nullptr ? info.name : "");
    infos_[id] = info;
    infos_[id].name = storage.name;
    infos_[id].fields = nullptr;
    infos_[id].field_count = 0;
    infos_[id].defaults = nullptr;
    if (info.fields != nullptr) {
        keep_fields(id, info.fields, info.field_count);
    }
    if (info.defaults != nullptr) {
        (void)set_component_defaults(id, info.defaults);
    }
    return id;
}

bool World::set_component_defaults(ComponentId id, const void* defaults) {
    if (id >= component_count_ || defaults == nullptr) {
        return false;
    }
    if (infos_[id].defaults != nullptr) {
        return true; // the first defaults stand
    }
    ComponentStorage& storage = storage_[id];
    const auto* bytes = static_cast<const std::uint8_t*>(defaults);
    storage.defaults.assign(bytes, bytes + infos_[id].size);
    infos_[id].defaults = storage.defaults.data();
    return true;
}

bool World::describe_component(ComponentId id, const core::FieldInfo* fields, std::uint32_t count) {
    if (id >= component_count_ || fields == nullptr || count == 0 || count > kMaxComponentFields) {
        return false;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        if (fields[i].name == nullptr || fields[i].offset + fields[i].size > infos_[id].size) {
            return false;
        }
    }
    if (infos_[id].fields != nullptr) {
        return true; // already described: the first description stands
    }
    keep_fields(id, fields, count);
    return true;
}

void World::keep_fields(ComponentId id, const core::FieldInfo* fields, std::uint32_t count) {
    ComponentStorage& storage = storage_[id];
    count = std::min(count, kMaxComponentFields);
    storage.fields.assign(fields, fields + count);
    storage.field_names.clear();
    storage.field_names.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        storage.field_names.emplace_back(fields[i].name != nullptr ? fields[i].name : "");
        storage.fields[i].name = storage.field_names.back().c_str();
    }
    infos_[id].fields = storage.fields.data();
    infos_[id].field_count = count;
}

ComponentId World::find_component(const char* name) const noexcept {
    const std::uint64_t hash = component_name_hash(name != nullptr ? name : "");
    for (std::uint32_t i = 0; i < component_count_; ++i) {
        if (infos_[i].name_hash == hash) {
            return i;
        }
    }
    return kNoComponent;
}

std::uint32_t World::find_or_create_archetype(std::uint64_t mask) {
    for (std::uint32_t i = 0; i < archetypes_.size(); ++i) {
        if (archetypes_[i]->mask == mask) {
            return i;
        }
    }

    auto archetype = std::make_unique<Archetype>();
    archetype->mask = mask;
    std::fill(std::begin(archetype->column_of), std::end(archetype->column_of), -1);
    std::uint32_t row_bytes = sizeof(Entity);
    for (ComponentId id = 0; id < component_count_; ++id) {
        if (mask & bit(id)) {
            archetype->column_of[id] = static_cast<std::int32_t>(archetype->components.size());
            archetype->components.push_back(id);
            row_bytes += infos_[id].size;
        }
    }

    // Rows per chunk: start from the packed estimate and shrink until the
    // aligned layout fits.
    std::uint32_t capacity = static_cast<std::uint32_t>(kChunkBytes) / row_bytes;
    TY_ASSERT(capacity >= 1, "a single row does not fit in a chunk");
    for (;;) {
        archetype->column_offset.clear();
        std::uint32_t offset = align_up(static_cast<std::uint32_t>(sizeof(Entity)) * capacity, 16);
        bool fits = true;
        for (const ComponentId id : archetype->components) {
            offset = align_up(offset, infos_[id].alignment);
            archetype->column_offset.push_back(offset);
            offset += infos_[id].size * capacity;
            if (offset > kChunkBytes) {
                fits = false;
                break;
            }
        }
        if (fits) {
            break;
        }
        --capacity;
        TY_ASSERT(capacity >= 1, "a single row does not fit in a chunk");
    }
    archetype->capacity = capacity;

    archetypes_.push_back(std::move(archetype));
    return static_cast<std::uint32_t>(archetypes_.size() - 1);
}

void World::allocate_row(std::uint32_t archetype_index, std::uint32_t& chunk, std::uint32_t& row) {
    Archetype& archetype = *archetypes_[archetype_index];
    if (archetype.chunks.empty() || archetype.chunks.back().count == archetype.capacity) {
        archetype.chunks.emplace_back();
    }
    chunk = static_cast<std::uint32_t>(archetype.chunks.size() - 1);
    row = archetype.chunks[chunk].count++;
    ++archetype.count;
}

// Swap-remove against the archetype's globally last row, so every chunk but
// the last stays full and allocation is always an append.
void World::remove_row(std::uint32_t archetype_index, std::uint32_t chunk, std::uint32_t row) noexcept {
    Archetype& archetype = *archetypes_[archetype_index];
    const std::uint32_t last_chunk = static_cast<std::uint32_t>(archetype.chunks.size() - 1);
    Chunk& last = archetype.chunks[last_chunk];
    const std::uint32_t last_row = last.count - 1;

    if (chunk != last_chunk || row != last_row) {
        Chunk& target = archetype.chunks[chunk];
        const Entity moved = last.entities()[last_row];
        target.entities()[row] = moved;
        for (std::uint32_t col = 0; col < archetype.components.size(); ++col) {
            const std::uint32_t size = infos_[archetype.components[col]].size;
            std::memcpy(archetype.column(chunk, col) + static_cast<std::size_t>(row) * size,
                        archetype.column(last_chunk, col) + static_cast<std::size_t>(last_row) * size, size);
        }
        EntityRecord* record = entities_.get(moved);
        TY_ASSERT(record != nullptr, "moved a row whose entity has no record");
        if (record != nullptr) {
            record->chunk = chunk;
            record->row = row;
        }
    }
    --last.count;
    --archetype.count;
    if (last.count == 0 && archetype.chunks.size() > 1) {
        archetype.chunks.pop_back(); // keep one chunk around: the archetype will likely refill
    }
}

Entity World::create_raw(const ComponentId* ids, const void* const* values, std::uint32_t count) {
    TY_ASSERT(iterating_.load() == 0, "cannot create entities while a query is running");
    std::uint64_t mask = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        TY_ASSERT((mask & bit(ids[i])) == 0, "a component type was given twice");
        mask |= bit(ids[i]);
    }
    const std::uint32_t archetype_index = find_or_create_archetype(mask);
    std::uint32_t chunk = 0;
    std::uint32_t row = 0;
    allocate_row(archetype_index, chunk, row);

    const Entity entity = entities_.create(EntityRecord{archetype_index, chunk, row});
    if (!entity) {
        remove_row(archetype_index, chunk, row);
        return Entity::null(); // out of entities: a budget, like everything else
    }
    Archetype& archetype = *archetypes_[archetype_index];
    archetype.chunks[chunk].entities()[row] = entity;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t col = static_cast<std::uint32_t>(archetype.column_of[ids[i]]);
        const std::uint32_t size = infos_[ids[i]].size;
        std::memcpy(archetype.column(chunk, col) + static_cast<std::size_t>(row) * size, values[i], size);
    }
    return entity;
}

bool World::destroy(Entity entity) noexcept {
    TY_ASSERT(iterating_.load() == 0, "cannot destroy entities while a query is running");
    const EntityRecord* record = entities_.get(entity);
    if (record == nullptr) {
        return false;
    }
    const EntityRecord copy = *record;
    entities_.destroy(entity);
    remove_row(copy.archetype, copy.chunk, copy.row);
    return true;
}

bool World::alive(Entity entity) const noexcept {
    return entities_.contains(entity);
}

std::uint32_t World::entity_count() const noexcept {
    return entities_.size();
}

std::uint32_t World::entity_components(Entity entity, ComponentId* out, std::uint32_t max) const noexcept {
    const EntityRecord* record = entities_.get(entity);
    if (record == nullptr) {
        return 0;
    }
    const Archetype& archetype = *archetypes_[record->archetype];
    const auto count = static_cast<std::uint32_t>(archetype.components.size());
    for (std::uint32_t i = 0; i < count && i < max; ++i) {
        out[i] = archetype.components[i];
    }
    return count;
}

void* World::get_raw(Entity entity, ComponentId id) noexcept {
    const EntityRecord* record = entities_.get(entity);
    if (record == nullptr) {
        return nullptr;
    }
    Archetype& archetype = *archetypes_[record->archetype];
    const std::int32_t col = archetype.column_of[id];
    if (col < 0) {
        return nullptr;
    }
    return archetype.column(record->chunk, static_cast<std::uint32_t>(col)) +
           static_cast<std::size_t>(record->row) * infos_[id].size;
}

bool World::add_raw(Entity entity, ComponentId id, const void* value) {
    TY_ASSERT(iterating_.load() == 0, "cannot add components while a query is running");
    EntityRecord* record = entities_.get(entity);
    if (record == nullptr) {
        return false;
    }
    const std::uint32_t source_index = record->archetype;
    if (archetypes_[source_index]->mask & bit(id)) {
        return false;
    }
    const std::uint32_t target_index = find_or_create_archetype(archetypes_[source_index]->mask | bit(id));
    record = entities_.get(entity); // archetypes_ may have grown; records live elsewhere but be safe
    const EntityRecord source = *record;

    std::uint32_t chunk = 0;
    std::uint32_t row = 0;
    allocate_row(target_index, chunk, row);
    Archetype& src = *archetypes_[source_index];
    Archetype& dst = *archetypes_[target_index];
    dst.chunks[chunk].entities()[row] = entity;
    for (std::uint32_t col = 0; col < dst.components.size(); ++col) {
        const ComponentId component = dst.components[col];
        const std::uint32_t size = infos_[component].size;
        std::byte* to = dst.column(chunk, col) + static_cast<std::size_t>(row) * size;
        if (component == id) {
            std::memcpy(to, value, size);
        } else {
            const std::uint32_t src_col = static_cast<std::uint32_t>(src.column_of[component]);
            std::memcpy(to, src.column(source.chunk, src_col) + static_cast<std::size_t>(source.row) * size, size);
        }
    }
    remove_row(source_index, source.chunk, source.row);
    *entities_.get(entity) = EntityRecord{target_index, chunk, row};
    return true;
}

bool World::remove_raw(Entity entity, ComponentId id) {
    TY_ASSERT(iterating_.load() == 0, "cannot remove components while a query is running");
    EntityRecord* record = entities_.get(entity);
    if (record == nullptr) {
        return false;
    }
    const std::uint32_t source_index = record->archetype;
    if ((archetypes_[source_index]->mask & bit(id)) == 0) {
        return false;
    }
    const std::uint32_t target_index = find_or_create_archetype(archetypes_[source_index]->mask & ~bit(id));
    const EntityRecord source = *entities_.get(entity);

    std::uint32_t chunk = 0;
    std::uint32_t row = 0;
    allocate_row(target_index, chunk, row);
    Archetype& src = *archetypes_[source_index];
    Archetype& dst = *archetypes_[target_index];
    dst.chunks[chunk].entities()[row] = entity;
    for (std::uint32_t col = 0; col < dst.components.size(); ++col) {
        const ComponentId component = dst.components[col];
        const std::uint32_t size = infos_[component].size;
        const std::uint32_t src_col = static_cast<std::uint32_t>(src.column_of[component]);
        std::memcpy(dst.column(chunk, col) + static_cast<std::size_t>(row) * size,
                    src.column(source.chunk, src_col) + static_cast<std::size_t>(source.row) * size, size);
    }
    remove_row(source_index, source.chunk, source.row);
    *entities_.get(entity) = EntityRecord{target_index, chunk, row};
    return true;
}

std::uint32_t World::matching_chunk_count(const ComponentId* ids, std::uint32_t count) const noexcept {
    std::uint64_t query = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        query |= bit(ids[i]);
    }
    std::uint32_t total = 0;
    for (const auto& archetype : archetypes_) {
        if ((archetype->mask & query) == query && archetype->count > 0) {
            for (const Chunk& chunk : archetype->chunks) {
                total += chunk.count > 0 ? 1 : 0;
            }
        }
    }
    return total;
}

void World::visit_chunks_erased(const ComponentId* ids, std::uint32_t count, std::uint32_t first,
                                std::uint32_t last, void (*callback)(void*, const ChunkView&), void* user) {
    std::uint64_t query = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        query |= bit(ids[i]);
    }
    iterating_.fetch_add(1, std::memory_order_acq_rel);
    std::uint32_t global = 0;
    for (const auto& archetype : archetypes_) {
        if ((archetype->mask & query) != query || archetype->count == 0) {
            continue;
        }
        for (std::uint32_t c = 0; c < archetype->chunks.size(); ++c) {
            Chunk& chunk = archetype->chunks[c];
            if (chunk.count == 0) {
                continue;
            }
            const std::uint32_t index = global++;
            if (index < first || index >= last) {
                continue;
            }
            ChunkView view;
            view.entities = chunk.entities();
            view.count = chunk.count;
            for (std::uint32_t i = 0; i < count; ++i) {
                view.columns[i] = archetype->column(c, static_cast<std::uint32_t>(archetype->column_of[ids[i]]));
            }
            callback(user, view);
        }
    }
    iterating_.fetch_sub(1, std::memory_order_acq_rel);
}

std::uint32_t World::archetype_count() const noexcept {
    return static_cast<std::uint32_t>(archetypes_.size());
}

std::uint32_t World::chunk_count() const noexcept {
    std::uint32_t total = 0;
    for (const auto& archetype : archetypes_) {
        total += static_cast<std::uint32_t>(archetype->chunks.size());
    }
    return total;
}

} // namespace tynima::scene
