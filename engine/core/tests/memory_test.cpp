#include <doctest/doctest.h>
#include <tynima/core/arena.h>
#include <tynima/core/math.h>
#include <tynima/core/memory.h>
#include <tynima/core/pool.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace tynima::core;

// ---------------------------------------------------------------- heap

// Measurements are taken before any CHECK: the test framework itself may
// allocate while it records a result.
TEST_CASE("heap accounting sees every C++ allocation") {
    const std::uint64_t count0 = heap_allocation_count();
    const std::uint64_t bytes0 = heap_bytes_in_use();
    const std::uint64_t frees0 = heap_free_count();
    std::uint64_t count1 = 0, bytes1 = 0, last = 0;
    {
        auto p = std::make_unique<std::uint64_t[]>(1024);
        count1 = heap_allocation_count();
        bytes1 = heap_bytes_in_use();
        p[1023] = 7; // the memory is real and writable to the end
        last = p[1023];
    }
    const std::uint64_t bytes2 = heap_bytes_in_use();
    const std::uint64_t frees1 = heap_free_count();
    CHECK(count1 == count0 + 1);
    CHECK(bytes1 == bytes0 + 8192);
    CHECK(last == 7);
    CHECK(bytes2 == bytes0);
    CHECK(frees1 == frees0 + 1);

    // Over-aligned types go through the aligned overloads and are still counted.
    struct alignas(64) Wide {
        float v[16];
    };
    const std::uint64_t count2 = heap_allocation_count();
    auto w = std::make_unique<Wide>();
    const std::uint64_t count3 = heap_allocation_count();
    const auto address = reinterpret_cast<std::uintptr_t>(w.get());
    CHECK(address % 64 == 0);
    CHECK(count3 == count2 + 1);
}

TEST_CASE("a HeapAllocationScope counts what happens inside it") {
    std::vector<int> v;
    v.reserve(16);
    HeapAllocationScope scope;
    v.push_back(1); // fits: no allocation
    const std::uint64_t after_push = scope.allocations();
    std::string s(100, 'x'); // too long for the small-string buffer
    const std::uint64_t after_string = scope.allocations();
    CHECK(after_push == 0);
    CHECK(after_string == 1);
}

TEST_CASE("external allocation scopes are excluded from the engine count") {
    HeapAllocationScope scope;
    std::uint64_t engine_after_external = 0, total_after_external = 0;
    {
        ExternalAllocationScope external;
        auto p = std::make_unique<std::uint64_t[]>(16); // as if the driver did it
        engine_after_external = scope.allocations();
        total_after_external = scope.total();
    }
    auto q = std::make_unique<std::uint64_t[]>(16); // ours
    const std::uint64_t engine_after_ours = scope.allocations();
    CHECK(engine_after_external == 0);
    CHECK(total_after_external == 1);
    CHECK(engine_after_ours == 1);
}

// --------------------------------------------------------------- arena

TEST_CASE("arena bumps, aligns, and refuses to grow") {
    Arena arena(256);
    CHECK(arena.capacity() == 256);
    CHECK(arena.used() == 0);

    void* a = arena.allocate(1, 1);
    void* b = arena.allocate(8, 64);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(b) % 64 == 0);
    CHECK(arena.used() == 72); // 1 byte, padding to 64, then 8

    HeapAllocationScope scope;
    void* c = arena.allocate(100);
    const std::uint64_t heap_traffic = scope.allocations();
    CHECK(c != nullptr);
    CHECK(heap_traffic == 0); // the point: no heap traffic per allocation
    CHECK(arena.high_water() == arena.used());
}

TEST_CASE("arena markers and reset reuse the same memory") {
    Arena arena(1024);
    void* first = arena.allocate(64);
    const Arena::Marker marker = arena.mark();
    void* second = arena.allocate(64);
    arena.reset_to(marker);
    void* third = arena.allocate(64);
    CHECK(third == second); // the marker handed the space back
    arena.reset();
    CHECK(arena.used() == 0);
    CHECK(arena.allocate(64) == first);
    CHECK(arena.high_water() == 128); // remembers the peak across resets
}

TEST_CASE("arena create and create_array construct properly") {
    using tynima::math::Vec3;
    Arena arena(4096);
    Vec3* v = arena.create<Vec3>(1.0f, 2.0f, 3.0f);
    REQUIRE(v != nullptr);
    CHECK(*v == Vec3{1.0f, 2.0f, 3.0f});

    std::span<Vec3> many = arena.create_array<Vec3>(10);
    REQUIRE(many.size() == 10);
    CHECK(many[9] == Vec3::zero()); // value-initialized: default member initializers applied
    CHECK(reinterpret_cast<std::uintptr_t>(many.data()) % alignof(Vec3) == 0);
    CHECK(arena.create_array<Vec3>(0).empty());
}

// ---------------------------------------------------------------- pool

namespace {
struct Tracked {
    static inline int live = 0;
    int value;
    explicit Tracked(int v) : value(v) { ++live; }
    ~Tracked() { --live; }
};
} // namespace

TEST_CASE("pool allocates up to capacity, frees in O(1), and runs destructors") {
    Pool<Tracked> pool(3);
    CHECK(pool.capacity() == 3);
    Tracked* a = pool.allocate(1);
    Tracked* b = pool.allocate(2);
    Tracked* c = pool.allocate(3);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(c != nullptr);
    CHECK(pool.full());
    CHECK(pool.allocate(4) == nullptr); // a budget, not a suggestion
    CHECK(Tracked::live == 3);
    CHECK(b->value == 2);
    CHECK(pool.owns(b));
    CHECK(pool.index_of(c) == 2);

    pool.free(b);
    CHECK(Tracked::live == 2);
    CHECK(pool.size() == 2);
    Tracked* d = pool.allocate(4);
    CHECK(d == b); // the freed slot is reused first
    CHECK(d->value == 4);

    pool.free(a);
    pool.free(c);
    pool.free(d);
    CHECK(Tracked::live == 0);
    CHECK(pool.size() == 0);
    CHECK_FALSE(pool.owns(reinterpret_cast<Tracked*>(&pool)));
}

TEST_CASE("pool allocation does not touch the heap") {
    Pool<int> pool(64);
    int* objects[64] = {};
    HeapAllocationScope scope;
    bool all_allocated = true;
    bool all_correct = true;
    for (int i = 0; i < 64; ++i) {
        objects[i] = pool.allocate(i);
        all_allocated = all_allocated && objects[i] != nullptr;
        all_correct = all_correct && objects[i] != nullptr && *objects[i] == i;
    }
    const std::uint64_t heap_traffic = scope.allocations();
    CHECK(all_allocated);
    CHECK(all_correct);
    CHECK(heap_traffic == 0);
    for (int* object : objects) {
        pool.free(object);
    }
    CHECK(pool.size() == 0);
}
