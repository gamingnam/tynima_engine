#include <doctest/doctest.h>
#include <tynima/core/jobs.h>
#include <tynima/core/memory.h>
#include <tynima/scene/components.h>
#include <tynima/scene/systems.h>
#include <tynima/scene/world.h>

#include <cstdint>
#include <vector>

using namespace tynima;
using namespace tynima::math;
using scene::Entity;
using scene::World;

namespace {
struct Position {
    static constexpr const char* kName = "Position";
    float x = 0, y = 0, z = 0;
};
struct Velocity {
    static constexpr const char* kName = "Velocity";
    float x = 0, y = 0, z = 0;
};
struct Tag {
    static constexpr const char* kName = "Tag";
    std::uint32_t value = 0;
};
struct Wide {
    static constexpr const char* kName = "Wide";
    alignas(32) float data[16] = {};
};
} // namespace

static_assert(scene::Component<Position>);
static_assert(scene::component_name_hash("Position") != scene::component_name_hash("Velocity"));

TEST_CASE("component types register once by name and get stable ids") {
    World world(64);
    const scene::ComponentId p = world.component_id<Position>();
    const scene::ComponentId v = world.component_id<Velocity>();
    CHECK(p != v);
    CHECK(world.component_id<Position>() == p);
    CHECK(world.component_type_count() == 2);
    CHECK(world.component_info(p).size == sizeof(Position));
    CHECK(world.component_info(p).name_hash == scene::component_name_hash("Position"));
}

TEST_CASE("entities are created with components, read back, and destroyed") {
    World world(64);
    const Entity a = world.create(Position{1, 2, 3}, Velocity{4, 5, 6});
    const Entity b = world.create(Position{7, 8, 9});
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a != b);
    CHECK(world.entity_count() == 2);
    CHECK(world.archetype_count() == 2);

    REQUIRE(world.get<Position>(a) != nullptr);
    CHECK(world.get<Position>(a)->y == 2);
    CHECK(world.get<Velocity>(a)->z == 6);
    CHECK(world.get<Velocity>(b) == nullptr);
    CHECK(world.has<Position>(b));
    CHECK_FALSE(world.has<Velocity>(b));

    world.get<Position>(a)->x = 42; // writable in place
    CHECK(world.get<Position>(a)->x == 42);

    CHECK(world.destroy(a));
    CHECK_FALSE(world.alive(a));
    CHECK(world.get<Position>(a) == nullptr); // the stale handle resolves to nothing
    CHECK_FALSE(world.destroy(a));
    CHECK(world.entity_count() == 1);
    CHECK(world.get<Position>(b)->x == 7); // b untouched
    CHECK(world.create() != Entity::null());  // an entity with no components at all
}

TEST_CASE("destroying from the middle keeps the moved entity's data and record right") {
    World world(64);
    Entity ids[5];
    for (std::uint32_t i = 0; i < 5; ++i) {
        ids[i] = world.create(Tag{i});
    }
    world.destroy(ids[1]); // the last row (Tag 4) moves into row 1
    CHECK(world.get<Tag>(ids[4])->value == 4);
    CHECK(world.get<Tag>(ids[2])->value == 2);
    world.destroy(ids[4]); // now the moved entity is destroyed from its new place
    CHECK(world.get<Tag>(ids[3])->value == 3);
    CHECK(world.entity_count() == 3);
    std::uint32_t sum = 0;
    world.each<Tag>([&](Entity, Tag& tag) { sum += tag.value; });
    CHECK(sum == 0 + 2 + 3);
}

TEST_CASE("adding and removing components moves entities between archetypes intact") {
    World world(64);
    const Entity e = world.create(Position{1, 2, 3});
    CHECK(world.add(e, Velocity{9, 8, 7}));
    CHECK_FALSE(world.add(e, Velocity{})); // already there
    CHECK(world.get<Position>(e)->z == 3); // survived the move
    CHECK(world.get<Velocity>(e)->x == 9);
    CHECK(world.archetype_count() == 2);

    // Others in the source archetype are not disturbed by the move.
    const Entity other = world.create(Position{5, 5, 5});
    const Entity third = world.create(Position{6, 6, 6});
    CHECK(world.add(other, Tag{1}));
    CHECK(world.get<Position>(third)->x == 6);
    CHECK(world.get<Position>(other)->x == 5);

    CHECK(world.remove<Velocity>(e));
    CHECK_FALSE(world.remove<Velocity>(e));
    CHECK_FALSE(world.has<Velocity>(e));
    CHECK(world.get<Position>(e)->y == 2);
    CHECK_FALSE(world.add(Entity::null(), Tag{}));
}

TEST_CASE("queries see exactly the entities that have every requested component") {
    World world(1024);
    for (int i = 0; i < 10; ++i) {
        (void)world.create(Position{float(i), 0, 0}, Velocity{1, 0, 0});
    }
    for (int i = 0; i < 5; ++i) {
        (void)world.create(Position{100, 0, 0});
    }
    for (int i = 0; i < 3; ++i) {
        (void)world.create(Velocity{0, 0, 0});
    }
    int moving = 0, positioned = 0, everything = 0;
    world.each<Position, Velocity>([&](Entity, Position& p, Velocity& v) {
        p.x += v.x;
        ++moving;
    });
    world.each<Position>([&](Entity, Position&) { ++positioned; });
    world.each<>([&](Entity) { ++everything; });
    CHECK(moving == 10);
    CHECK(positioned == 15);
    CHECK(everything == 18);
    float sum = 0;
    world.each<Position, Velocity>([&](Entity, Position& p, Velocity&) { sum += p.x; });
    CHECK(sum == doctest::Approx(45.0f + 10.0f)); // 0..9 each moved by 1
}

TEST_CASE("chunks are contiguous struct-of-arrays and fill before a new one starts") {
    World world(20000);
    const std::uint32_t n = 5000;
    for (std::uint32_t i = 0; i < n; ++i) {
        (void)world.create(Position{float(i), 0, 0}, Wide{});
    }
    // Rows per 16 KB chunk with 8 (entity) + 12 (Position) + 64 (Wide) bytes each.
    CHECK(world.chunk_count() > 1);
    std::uint32_t seen = 0, chunks = 0;
    std::uint32_t largest = 0;
    world.each_chunk<Position, Wide>([&](std::span<const Entity> entities, Position* positions, Wide* wides) {
        ++chunks;
        seen += static_cast<std::uint32_t>(entities.size());
        largest = entities.size() > largest ? static_cast<std::uint32_t>(entities.size()) : largest;
        CHECK(reinterpret_cast<std::uintptr_t>(wides) % 32 == 0);      // column honours alignas(32)
        CHECK(&positions[1] == positions + 1);                           // trivially, but: plain arrays
        CHECK(positions[0].x == float(world.get<Position>(entities[0]) - positions) + positions[0].x); // same storage
        for (std::size_t i = 0; i < entities.size(); ++i) {
            CHECK(world.get<Position>(entities[i]) == &positions[i]); // get() points into the column
        }
    });
    CHECK(seen == n);
    CHECK(chunks == world.chunk_count());
    CHECK(largest >= 150); // 16384 / 84 ≈ 195 rows, minus alignment padding
}

TEST_CASE("parallel_each_chunk covers every chunk once across the workers") {
    core::JobSystem jobs;
    World world(20000);
    const std::uint32_t n = 6000;
    for (std::uint32_t i = 0; i < n; ++i) {
        (void)world.create(Position{1, 0, 0}, Tag{i});
    }
    std::atomic<std::uint32_t> visited{0};
    std::atomic<std::uint64_t> tag_sum{0};
    world.parallel_each_chunk<Position, Tag>(jobs, [&](std::span<const Entity> entities, Position* p, Tag* tags) {
        std::uint64_t local = 0;
        for (std::size_t i = 0; i < entities.size(); ++i) {
            p[i].x += 1.0f;
            local += tags[i].value;
        }
        visited.fetch_add(static_cast<std::uint32_t>(entities.size()));
        tag_sum.fetch_add(local);
    });
    CHECK(visited.load() == n);
    CHECK(tag_sum.load() == static_cast<std::uint64_t>(n) * (n - 1) / 2);
    float check = 0;
    world.each<Position>([&](Entity, Position& p) { check += p.x; });
    CHECK(check == doctest::Approx(2.0f * n));
}

TEST_CASE("querying allocates nothing on the heap") {
    World world(1024);
    for (int i = 0; i < 200; ++i) {
        (void)world.create(Position{}, Velocity{1, 1, 1});
    }
    core::HeapAllocationScope scope;
    float sum = 0;
    world.each<Position, Velocity>([&](Entity, Position& p, Velocity& v) {
        p.x += v.x;
        sum += p.x;
    });
    world.each_chunk<Position>([&](std::span<const Entity> e, Position*) { sum += float(e.size()); });
    const std::uint64_t traffic = scope.allocations();
    CHECK(sum > 0);
    CHECK(traffic == 0);
}

TEST_CASE("update_transforms composes through Parent chains") {
    using scene::LocalToWorld;
    using scene::Parent;
    using scene::Transform;
    World world(64);
    const Entity root = world.create(Transform{.position = {10, 0, 0}}, LocalToWorld{});
    const Entity child = world.create(Transform{.position = {0, 1, 0}}, LocalToWorld{}, Parent{root});
    const Entity grandchild = world.create(
        Transform{.position = {0, 0, 1}, .rotation = Quat::from_axis_angle(Vec3::unit_y(), radians(90.0f))},
        LocalToWorld{}, Parent{child});
    const Entity loner = world.create(Transform{.position = {5, 5, 5}, .scale = Vec3{2.0f}}, LocalToWorld{});

    scene::update_transforms(world);
    CHECK(approx_equal(world.get<LocalToWorld>(root)->matrix.translation(), Vec3{10, 0, 0}));
    CHECK(approx_equal(world.get<LocalToWorld>(child)->matrix.translation(), Vec3{10, 1, 0}));
    CHECK(approx_equal(world.get<LocalToWorld>(grandchild)->matrix.translation(), Vec3{10, 1, 1}));
    // The grandchild's rotation applies to its own local axes.
    CHECK(approx_equal(transform_vector(world.get<LocalToWorld>(grandchild)->matrix, Vec3::unit_x()), -Vec3::unit_z()));
    CHECK(approx_equal(transform_point(world.get<LocalToWorld>(loner)->matrix, Vec3::one()), Vec3{7, 7, 7}));

    // Moving the root moves the whole chain on the next update.
    world.get<Transform>(root)->position.x = 20;
    scene::update_transforms(world);
    CHECK(approx_equal(world.get<LocalToWorld>(grandchild)->matrix.translation(), Vec3{20, 1, 1}));

    // A destroyed parent detaches its children rather than crashing.
    world.destroy(root);
    scene::update_transforms(world);
    CHECK(approx_equal(world.get<LocalToWorld>(child)->matrix.translation(), Vec3{0, 1, 0}));
}
