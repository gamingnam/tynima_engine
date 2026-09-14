#include <doctest/doctest.h>
#include <tynima/core/handle.h>
#include <tynima/core/memory.h>

#include <cstdint>
#include <string>

using namespace tynima::core;

namespace {
struct Widget {
    static inline int live = 0;
    std::string name;
    explicit Widget(std::string n) : name(std::move(n)) { ++live; }
    ~Widget() { --live; }
};
struct WidgetTag {};
using WidgetHandle = Handle<WidgetTag>;
using Widgets = HandlePool<Widget, WidgetTag>;
} // namespace

static_assert(sizeof(WidgetHandle) == 8, "a handle is two 32-bit integers");
static_assert(!WidgetHandle{}, "a default handle is null");
static_assert(WidgetHandle::from_packed(WidgetHandle{7, 3}.packed()) == WidgetHandle{7, 3});

TEST_CASE("handles resolve while the object lives and never afterwards") {
    Widgets pool(4);
    const WidgetHandle a = pool.create("a");
    const WidgetHandle b = pool.create("b");
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a != b);
    CHECK(pool.size() == 2);
    REQUIRE(pool.get(a) != nullptr);
    CHECK(pool.get(a)->name == "a");
    CHECK(pool.get(b)->name == "b");
    CHECK(Widget::live == 2);

    CHECK(pool.destroy(a));
    CHECK(Widget::live == 1);
    CHECK(pool.get(a) == nullptr); // the use-after-free, caught
    CHECK_FALSE(pool.contains(a));
    CHECK_FALSE(pool.destroy(a)); // a double destroy is refused, not exploded
    CHECK(pool.get(b)->name == "b");

    // The slot is reused, with a new generation: the stale handle still fails.
    const WidgetHandle c = pool.create("c");
    CHECK(c.index == a.index);
    CHECK(c.generation != a.generation);
    CHECK(pool.get(a) == nullptr);
    CHECK(pool.get(c)->name == "c");
}

TEST_CASE("null and garbage handles are harmless") {
    Widgets pool(2);
    CHECK(pool.get(WidgetHandle::null()) == nullptr);
    CHECK(pool.get(WidgetHandle{99, 1}) == nullptr); // out of range
    CHECK(pool.get(WidgetHandle{0, 42}) == nullptr); // wrong generation, slot not live
    CHECK_FALSE(pool.destroy(WidgetHandle::null()));
    const WidgetHandle a = pool.create("a");
    CHECK(pool.get(WidgetHandle{a.index, a.generation + 1}) == nullptr);
}

TEST_CASE("a full pool returns the null handle; destroying frees a slot") {
    Widgets pool(2);
    const WidgetHandle a = pool.create("a");
    const WidgetHandle b = pool.create("b");
    CHECK(pool.full());
    CHECK_FALSE(pool.create("c"));
    CHECK(pool.destroy(b));
    CHECK_FALSE(pool.full());
    const WidgetHandle c = pool.create("c");
    CHECK(c);
    CHECK(pool.get(a)->name == "a");
    CHECK(pool.get(c)->name == "c");
}

TEST_CASE("the pool destroys what is still alive, and for_each visits only the living") {
    {
        Widgets pool(8);
        const WidgetHandle a = pool.create("a");
        const WidgetHandle b = pool.create("b");
        const WidgetHandle c = pool.create("c");
        pool.destroy(b);
        int visited = 0;
        std::string names;
        pool.for_each([&](WidgetHandle handle, Widget& w) {
            ++visited;
            names += w.name;
            CHECK(pool.get(handle) == &w);
        });
        CHECK(visited == 2);
        CHECK(names == "ac");
        CHECK(Widget::live == 2);
        (void)a;
        (void)c;
    }
    CHECK(Widget::live == 0); // the pool's destructor ran the remaining destructors
}

TEST_CASE("create, get and destroy do not touch the heap") {
    HandlePool<int> pool(32);
    HeapAllocationScope scope;
    Handle<int> handles[32];
    for (int i = 0; i < 32; ++i) {
        handles[i] = pool.create(i);
    }
    int sum = 0;
    for (const Handle<int> h : handles) {
        sum += *pool.get(h);
    }
    for (const Handle<int> h : handles) {
        pool.destroy(h);
    }
    const std::uint64_t traffic = scope.allocations();
    CHECK(sum == 496);
    CHECK(traffic == 0);
    CHECK(pool.size() == 0);
}
