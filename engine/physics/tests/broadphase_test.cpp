#include <doctest/doctest.h>
#include <tynima/core/memory.h>
#include <tynima/physics/broadphase.h>
#include <tynima/physics/physics.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace tynima::physics;
using tynima::math::Aabb;
using tynima::math::Vec3;

namespace {

using Factory = std::unique_ptr<Broadphase> (*)(std::uint32_t capacity);

struct Kind {
    const char* name;
    Factory make;
};

const Kind kKinds[] = {
    {"brute force", &create_brute_force_broadphase},
    {"sweep and prune", &create_sweep_and_prune},
    {"AABB tree", [](std::uint32_t capacity) { return create_aabb_tree(capacity, 0.1f); }},
};

bool pair_less(const ProxyPair& x, const ProxyPair& y) {
    return x.a.packed() != y.a.packed() ? x.a.packed() < y.a.packed() : x.b.packed() < y.b.packed();
}

std::vector<ProxyPair> sorted_pairs(Broadphase& bp, std::vector<ProxyPair>& scratch) {
    bp.find_pairs(scratch);
    std::vector<ProxyPair> pairs = scratch;
    std::sort(pairs.begin(), pairs.end(), pair_less);
    return pairs;
}

// The same operations on every implementation. The handle pools all see
// the same sequence, so the handles come out identical, and the pair lists
// compare directly against the brute-force reference.
struct Mirror {
    std::vector<std::unique_ptr<Broadphase>> all;
    std::vector<ProxyHandle> handles;
    std::vector<ProxyPair> scratch;

    explicit Mirror(std::uint32_t capacity) {
        for (const Kind& kind : kKinds) {
            all.push_back(kind.make(capacity));
        }
    }
    Broadphase& reference() { return *all[0]; }

    std::size_t add(const Aabb& box, std::uint64_t user_data = 0) {
        const ProxyHandle handle = all[0]->add(box, user_data);
        for (std::size_t i = 1; i < all.size(); ++i) {
            const ProxyHandle other = all[i]->add(box, user_data);
            REQUIRE(other == handle);
        }
        handles.push_back(handle);
        return handles.size() - 1;
    }
    void update(std::size_t i, const Aabb& box) {
        for (auto& bp : all) {
            bp->update(handles[i], box);
        }
    }
    void remove(std::size_t i) {
        for (auto& bp : all) {
            REQUIRE(bp->remove(handles[i]));
        }
        handles[i] = ProxyHandle::null();
    }

    // Every implementation agrees with the reference, exactly.
    void check_pairs() {
        const std::vector<ProxyPair> expected = sorted_pairs(reference(), scratch);
        for (std::size_t i = 1; i < all.size(); ++i) {
            INFO(all[i]->name());
            const std::vector<ProxyPair> pairs = sorted_pairs(*all[i], scratch);
            CHECK(pairs.size() == expected.size());
            CHECK(pairs == expected);
            CHECK(all[i]->self_check());
        }
        CHECK(reference().self_check());
    }
    void check_query(const Aabb& box) {
        std::vector<std::uint64_t> expected;
        reference().query(box, [&](ProxyHandle h, std::uint64_t) { expected.push_back(h.packed()); });
        std::sort(expected.begin(), expected.end());
        for (std::size_t i = 1; i < all.size(); ++i) {
            INFO(all[i]->name());
            std::vector<std::uint64_t> found;
            all[i]->query(box, [&](ProxyHandle h, std::uint64_t) { found.push_back(h.packed()); });
            std::sort(found.begin(), found.end());
            CHECK(found == expected);
        }
    }
    void check_ray(const Vec3& origin, const Vec3& direction, float max_t) {
        std::vector<std::uint64_t> expected;
        reference().cast_ray(origin, direction, max_t,
                             [&](ProxyHandle h, std::uint64_t) { expected.push_back(h.packed()); });
        std::sort(expected.begin(), expected.end());
        for (std::size_t i = 1; i < all.size(); ++i) {
            INFO(all[i]->name());
            std::vector<std::uint64_t> found;
            all[i]->cast_ray(origin, direction, max_t,
                             [&](ProxyHandle h, std::uint64_t) { found.push_back(h.packed()); });
            std::sort(found.begin(), found.end());
            CHECK(found == expected);
        }
    }
};

// Deterministic on every platform: mt19937 is specified bit for bit, and
// the scaling below is ours.
struct Random {
    std::mt19937 engine{12345};
    float unit() { return static_cast<float>(engine() & 0xFFFFFF) / static_cast<float>(0x1000000); }
    float range(float lo, float hi) { return lo + (hi - lo) * unit(); }
    Vec3 vec(float lo, float hi) { return Vec3{range(lo, hi), range(lo, hi), range(lo, hi)}; }
    Aabb box(float world, float min_size, float max_size) {
        const Vec3 center = vec(-world, world);
        const Vec3 half = vec(min_size, max_size) * 0.5f;
        return Aabb::from_center(center, half);
    }
};

double ms_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

TEST_CASE("Aabb: closed boxes, unions, areas, rays") {
    const Aabb a{Vec3{0, 0, 0}, Vec3{1, 1, 1}};
    const Aabb b{Vec3{1, 0, 0}, Vec3{2, 1, 1}}; // shares the x = 1 face
    const Aabb c{Vec3{1.001f, 0, 0}, Vec3{2, 1, 1}};
    CHECK(a.overlaps(b));
    CHECK(b.overlaps(a));
    CHECK_FALSE(a.overlaps(c));
    CHECK(a.contains(Aabb{Vec3{0.2f}, Vec3{0.8f}}));
    CHECK_FALSE(a.contains(b));
    CHECK(a.contains(Vec3{1, 1, 1}));
    CHECK_FALSE(a.contains(Vec3{1, 1, 1.5f}));
    CHECK(a.merged(c) == Aabb{Vec3{0, 0, 0}, Vec3{2, 1, 1}});
    CHECK(a.surface_area() == doctest::Approx(6.0f));
    CHECK(a.expanded(0.5f).extent().x == doctest::Approx(2.0f));
    CHECK(Aabb::empty().is_empty());
    CHECK_FALSE(Aabb::empty().overlaps(a));
    CHECK(Aabb::empty().merged(a) == a);

    CHECK(a.intersects_ray(Vec3{0.5f, 0.5f, -1}, Vec3{0, 0, 1}, 10));
    CHECK_FALSE(a.intersects_ray(Vec3{0.5f, 0.5f, -1}, Vec3{0, 0, 1}, 0.5f)); // too short
    CHECK_FALSE(a.intersects_ray(Vec3{0.5f, 0.5f, -1}, Vec3{0, 0, -1}, 10));  // wrong way
    CHECK(a.intersects_ray(Vec3{0.5f, 0.5f, 0.5f}, Vec3{1, 0, 0}, 10));       // from inside
    CHECK_FALSE(a.intersects_ray(Vec3{2, 0.5f, -1}, Vec3{0, 0, 1}, 10));      // parallel, outside the slab
    CHECK(a.intersects_ray(Vec3{-1, -1, -1}, Vec3{1, 1, 1}, 10));             // through the corner diagonal
}

TEST_CASE("every broadphase finds each overlapping pair once, ordered by index") {
    for (const Kind& kind : kKinds) {
        SUBCASE(kind.name) {
            auto bp = kind.make(16);
            CHECK(std::string(bp->name()) == kind.name);
            std::vector<ProxyPair> pairs;
            bp->find_pairs(pairs);
            CHECK(pairs.empty());

            const ProxyHandle a = bp->add(Aabb{Vec3{0}, Vec3{1}}, 10);
            const ProxyHandle b = bp->add(Aabb{Vec3{0.5f}, Vec3{1.5f}}, 20); // overlaps a
            const ProxyHandle c = bp->add(Aabb{Vec3{5}, Vec3{6}}, 30);       // alone
            // Touches a's face (closed boxes overlap), overlaps b.
            const ProxyHandle d = bp->add(Aabb{Vec3{1, 0, 0}, Vec3{2, 1, 1}}, 40);
            REQUIRE(a);
            REQUIRE(b);
            REQUIRE(c);
            REQUIRE(d);
            CHECK(bp->proxy_count() == 4);
            CHECK(bp->user_data(b) == 20);
            CHECK(bp->box(c) == Aabb{Vec3{5}, Vec3{6}});

            bp->find_pairs(pairs);
            std::sort(pairs.begin(), pairs.end(), pair_less);
            REQUIRE(pairs.size() == 3);
            CHECK(pairs[0] == ProxyPair{a, b});
            CHECK(pairs[1] == ProxyPair{a, d});
            CHECK(pairs[2] == ProxyPair{b, d});
            for (const ProxyPair& p : pairs) {
                CHECK(p.a.index < p.b.index);
            }

            // Move c onto the others, and b away.
            bp->update(c, Aabb{Vec3{0.9f, 0, 0}, Vec3{1.1f, 1, 1}});
            bp->update(b, Aabb{Vec3{10}, Vec3{11}});
            bp->find_pairs(pairs);
            std::sort(pairs.begin(), pairs.end(), pair_less);
            REQUIRE(pairs.size() == 3);
            CHECK(pairs[0] == ProxyPair{a, c});
            CHECK(pairs[1] == ProxyPair{a, d});
            CHECK(pairs[2] == ProxyPair{c, d});
            CHECK(bp->self_check());

            // Remove one; its handle dies; the slot's next tenant is a new generation.
            CHECK(bp->remove(a));
            CHECK_FALSE(bp->remove(a));
            CHECK_FALSE(bp->valid(a));
            CHECK(bp->user_data(a) == 0);
            CHECK(bp->box(a).is_empty());
            bp->update(a, Aabb{Vec3{0}, Vec3{100}}); // ignored
            bp->find_pairs(pairs);
            REQUIRE(pairs.size() == 1);
            CHECK(pairs[0] == ProxyPair{c, d});
            const ProxyHandle e = bp->add(Aabb{Vec3{0}, Vec3{1}}, 50);
            CHECK(e.index == a.index);
            CHECK(e.generation != a.generation);
            CHECK(bp->self_check());
        }
    }
}

TEST_CASE("a broadphase has a fixed capacity") {
    for (const Kind& kind : kKinds) {
        SUBCASE(kind.name) {
            auto bp = kind.make(3);
            CHECK(bp->add(Aabb{Vec3{0}, Vec3{1}}, 0));
            CHECK(bp->add(Aabb{Vec3{0}, Vec3{1}}, 0));
            const ProxyHandle last = bp->add(Aabb{Vec3{0}, Vec3{1}}, 0);
            CHECK(last);
            CHECK_FALSE(bp->add(Aabb{Vec3{0}, Vec3{1}}, 0));
            CHECK(bp->remove(last));
            CHECK(bp->add(Aabb{Vec3{0}, Vec3{1}}, 0));
            CHECK(bp->self_check());
        }
    }
}

TEST_CASE("random boxes, moving, coming and going: all three agree with brute force") {
    Random random;
    Mirror mirror(400);
    for (int i = 0; i < 300; ++i) {
        mirror.add(random.box(10.0f, 0.2f, 2.5f), static_cast<std::uint64_t>(i));
    }
    mirror.check_pairs();
    CHECK(sorted_pairs(mirror.reference(), mirror.scratch).size() > 50); // dense enough to mean something

    for (int round = 0; round < 25; ++round) {
        // Small moves for most, a jump for a few: what a step looks like.
        for (int i = 0; i < 60; ++i) {
            const auto which = static_cast<std::size_t>(random.engine() % mirror.handles.size());
            if (!mirror.handles[which]) {
                continue;
            }
            Aabb box = mirror.reference().box(mirror.handles[which]);
            const Vec3 delta = round % 5 == 4 ? random.vec(-8.0f, 8.0f) : random.vec(-0.15f, 0.15f);
            box.min += delta;
            box.max += delta;
            mirror.update(which, box);
        }
        if (round % 3 == 2) {
            for (int i = 0; i < 20; ++i) {
                const auto which = static_cast<std::size_t>(random.engine() % mirror.handles.size());
                if (mirror.handles[which]) {
                    mirror.remove(which);
                }
            }
            for (int i = 0; i < 20; ++i) {
                mirror.add(random.box(10.0f, 0.2f, 2.5f), 1000 + static_cast<std::uint64_t>(i));
            }
        }
        mirror.check_pairs();
        mirror.check_query(random.box(8.0f, 1.0f, 6.0f));
        mirror.check_ray(random.vec(-12.0f, 12.0f), random.vec(-1.0f, 1.0f), random.range(1.0f, 40.0f));
    }
}

TEST_CASE("a line of boxes along one axis stays fast: the sweep axis follows the scene") {
    // Everything spread along z: a sweep along x would keep every interval
    // open at once and test every pair. The variance check must pick z.
    Mirror mirror(600);
    for (int i = 0; i < 500; ++i) {
        const float z = static_cast<float>(i) * 0.5f;
        mirror.add(Aabb{Vec3{-0.2f, -0.2f, z}, Vec3{0.2f, 0.2f, z + 0.6f}}, static_cast<std::uint64_t>(i));
    }
    mirror.check_pairs();
    const std::vector<ProxyPair> pairs = sorted_pairs(mirror.reference(), mirror.scratch);
    CHECK(pairs.size() == 499); // each box overlaps the next along the line

    // Once the pairs have been found (and the axis chosen), a sweep must
    // not be quadratic: time it against brute force.
    std::vector<ProxyPair> out;
    double brute = 0.0, sap = 0.0;
    for (int i = 0; i < 20; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        mirror.all[0]->find_pairs(out);
        brute += ms_since(t0);
        t0 = std::chrono::steady_clock::now();
        mirror.all[1]->find_pairs(out);
        sap += ms_since(t0);
    }
    MESSAGE("500 boxes in a line, 20 sweeps: brute force " << brute << " ms, sweep and prune " << sap
                                                            << " ms");
    CHECK(sap * 4.0 < brute);
}

TEST_CASE("find_pairs, update and queries allocate nothing once the scene is in") {
    for (const Kind& kind : kKinds) {
        SUBCASE(kind.name) {
            Random random;
            auto bp = kind.make(256);
            std::vector<ProxyHandle> handles;
            for (int i = 0; i < 200; ++i) {
                handles.push_back(bp->add(random.box(6.0f, 0.3f, 1.5f), 0));
            }
            std::vector<ProxyPair> pairs;
            bp->find_pairs(pairs); // grows the output once
            pairs.reserve(pairs.size() * 2 + 64);
            int visited = 0;

            const tynima::core::HeapAllocationScope heap;
            for (int i = 0; i < 100; ++i) {
                Aabb box = bp->box(handles[static_cast<std::size_t>(i)]);
                box.min += Vec3{0.05f, -0.1f, 0.02f};
                box.max += Vec3{0.05f, -0.1f, 0.02f};
                bp->update(handles[static_cast<std::size_t>(i)], box);
            }
            bp->find_pairs(pairs);
            bp->query(Aabb{Vec3{-2}, Vec3{2}}, [&](ProxyHandle, std::uint64_t) { ++visited; });
            bp->cast_ray(Vec3{-10, 0, 0}, Vec3{1, 0, 0}, 20.0f,
                         [&](ProxyHandle, std::uint64_t) { ++visited; });
            const std::uint64_t allocations = heap.allocations();
            CHECK(allocations == 0);
            CHECK(visited > 0);
        }
    }
}

TEST_CASE("the pile through Jolt: real trajectories, identical pairs, timed") {
    // The sandbox's scene, more or less: a floor and a hundred boxes dropped
    // in a heap. Jolt moves them; every few frames each body's box goes
    // into all three broadphases, which must agree pair for pair.
    auto world = create_jolt_world({});
    BodyDesc floor;
    floor.shape = Shape::box(Vec3{6.0f, 0.25f, 6.0f});
    floor.position = Vec3{0.0f, -0.25f, 0.0f};
    floor.motion = MotionType::Static;
    std::vector<BodyHandle> bodies;
    bodies.push_back(world->create_body(floor));
    Random random;
    for (int i = 0; i < 100; ++i) {
        BodyDesc box;
        box.shape = Shape::box(Vec3{0.055f, 0.13f, 0.055f});
        box.position = Vec3{random.range(-0.4f, 0.4f), 0.6f + static_cast<float>(i) * 0.12f,
                            random.range(-0.4f, 0.4f)};
        box.angular_velocity = random.vec(-2.0f, 2.0f);
        box.mass = 0.6f;
        bodies.push_back(world->create_body(box));
        REQUIRE(bodies.back());
    }

    Mirror mirror(128);
    for (const BodyHandle body : bodies) {
        mirror.add(world->body_bounds(body), body.packed());
    }
    mirror.check_pairs();

    double update_ms[3] = {0.0, 0.0, 0.0};
    double pairs_ms[3] = {0.0, 0.0, 0.0};
    std::vector<ProxyPair> out;
    int samples = 0;
    std::size_t pairs_at_rest = 0;
    for (int frame = 1; frame <= 600; ++frame) {
        world->step(1.0f / 60.0f);
        if (frame % 6 != 0) {
            continue;
        }
        ++samples;
        for (std::size_t k = 0; k < mirror.all.size(); ++k) {
            const auto t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < bodies.size(); ++i) {
                mirror.all[k]->update(mirror.handles[i], world->body_bounds(bodies[i]));
            }
            update_ms[k] += ms_since(t0);
            const auto t1 = std::chrono::steady_clock::now();
            mirror.all[k]->find_pairs(out);
            pairs_ms[k] += ms_since(t1);
        }
        mirror.check_pairs();
        pairs_at_rest = sorted_pairs(mirror.reference(), mirror.scratch).size();
    }
    CHECK(world->active_body_count() <= 5); // the pile has all but settled by now
    CHECK(pairs_at_rest >= 60);             // a heap: boxes against their neighbours
    for (std::size_t k = 0; k < mirror.all.size(); ++k) {
        MESSAGE(std::string(mirror.all[k]->name())
                << ": " << samples << " samples of 101 bodies, update " << update_ms[k] / samples
                << " ms, find_pairs " << pairs_ms[k] / samples << " ms per sample (" << pairs_at_rest
                << " pairs at rest)");
    }
}

TEST_CASE("two thousand boxes: where the curves part") {
    Random random;
    const std::uint32_t count = 2000;
    std::vector<std::unique_ptr<Broadphase>> all;
    for (const Kind& kind : kKinds) {
        all.push_back(kind.make(count));
    }
    std::vector<Aabb> initial;
    for (std::uint32_t i = 0; i < count; ++i) {
        initial.push_back(random.box(25.0f, 0.3f, 1.2f));
    }
    const Random motion = random; // every implementation sees the same moves
    std::vector<std::vector<ProxyHandle>> handles(all.size());
    for (std::size_t k = 0; k < all.size(); ++k) {
        for (const Aabb& box : initial) {
            handles[k].push_back(all[k]->add(box, 0));
        }
    }
    std::vector<ProxyPair> out;
    std::vector<std::vector<ProxyPair>> results(all.size());
    for (std::size_t k = 0; k < all.size(); ++k) {
        std::vector<Aabb> boxes = initial;
        random = motion;
        double total = 0.0;
        for (int frame = 0; frame < 5; ++frame) {
            // Everything drifts a little, a few boxes jump.
            for (std::uint32_t i = 0; i < count; ++i) {
                const Vec3 delta = i % 97 == 0 ? random.vec(-5.0f, 5.0f) : random.vec(-0.1f, 0.1f);
                boxes[i].min += delta;
                boxes[i].max += delta;
            }
            const auto t0 = std::chrono::steady_clock::now();
            for (std::uint32_t i = 0; i < count; ++i) {
                all[k]->update(handles[k][i], boxes[i]);
            }
            all[k]->find_pairs(out);
            total += ms_since(t0);
        }
        results[k] = out;
        std::sort(results[k].begin(), results[k].end(), pair_less);
        MESSAGE(std::string(all[k]->name()) << ": " << total / 5.0 << " ms per frame for " << count
                                            << " boxes, " << out.size() << " pairs");
    }
    CHECK(results[1] == results[0]);
    CHECK(results[2] == results[0]);
}
