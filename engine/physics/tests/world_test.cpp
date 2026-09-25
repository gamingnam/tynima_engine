#include "backends.h"

#include <doctest/doctest.h>
#include <tynima/core/jobs.h>
#include <tynima/core/memory.h>
#include <tynima/physics/physics.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace tynima::physics;
using tynima::math::Quat;
using tynima::math::Vec3;

namespace {

struct Random {
    std::mt19937 engine{4242};
    float unit() { return static_cast<float>(engine() & 0xFFFFFF) / static_cast<float>(0x1000000); }
    float range(float lo, float hi) { return lo + (hi - lo) * unit(); }
    Vec3 vec(float lo, float hi) { return Vec3{range(lo, hi), range(lo, hi), range(lo, hi)}; }
};

} // namespace

TEST_CASE("the backends say who they are") {
    auto jolt = create_jolt_world({});
    REQUIRE(jolt != nullptr);
    CHECK(std::strncmp(jolt->backend_name(), "Jolt 5.", 7) == 0);
    auto ours = create_tynima_world({});
    REQUIRE(ours != nullptr);
    CHECK(std::string(ours->backend_name()) == "Tynima");
    CHECK(ours->body_count() == 0);
}

TEST_CASE("a box dropped on the floor lands, settles and falls asleep") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            REQUIRE(add_floor(*world));

            BodyDesc box;
            box.shape = Shape::box(Vec3{0.5f});
            box.position = Vec3{0.0f, 3.0f, 0.0f};
            const BodyHandle body = world->create_body(box);
            REQUIRE(body);
            CHECK(world->body_count() == 2);
            CHECK(world->active_body_count() == 1);

            run(*world, 0.25f); // still in the air, and falling
            const BodyState falling = world->body_state(body);
            CHECK(falling.active);
            CHECK(falling.position.y < 3.0f);
            CHECK(falling.position.y > 0.5f);
            CHECK(falling.linear_velocity.y < -1.0f);

            run(*world, 3.0f);
            const BodyState rest = world->body_state(body);
            CHECK(rest.position.y == doctest::Approx(0.5f).epsilon(0.02));
            CHECK(std::fabs(rest.position.x) < 0.01f);
            CHECK(std::fabs(rest.position.z) < 0.01f);
            CHECK(std::fabs(rest.linear_velocity.y) < 0.01f);
            CHECK_FALSE(rest.active); // a body that stopped moving goes to sleep
            CHECK(world->active_body_count() == 0);
        }
    }
}

TEST_CASE("restitution decides how much of a bounce comes back") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            REQUIRE(add_floor(*world));

            BodyDesc ball;
            ball.shape = Shape::sphere(0.25f);
            ball.position = Vec3{0.0f, 2.25f, 0.0f}; // 2 m of fall to the surface
            ball.restitution = 0.8f;
            const BodyHandle bouncy = world->create_body(ball);
            ball.position.x = 3.0f;
            ball.restitution = 0.0f;
            const BodyHandle dead = world->create_body(ball);
            REQUIRE(bouncy);
            REQUIRE(dead);

            // Track the highest point after the first impact.
            float bounce_height = 0.0f, dead_height = 0.0f;
            bool bouncy_hit = false, dead_hit = false;
            for (int i = 0; i < 180; ++i) {
                world->step(kStep);
                const BodyState b = world->body_state(bouncy);
                const BodyState d = world->body_state(dead);
                bouncy_hit = bouncy_hit || b.linear_velocity.y > 0.0f;
                dead_hit = dead_hit || d.position.y < 0.3f;
                if (bouncy_hit) bounce_height = std::max(bounce_height, b.position.y - 0.25f);
                if (dead_hit) dead_height = std::max(dead_height, d.position.y - 0.25f);
            }
            CHECK(bouncy_hit);
            CHECK(bounce_height > 0.8f); // e = 0.8 returns up to 64% of the height, minus solver losses
            CHECK(bounce_height < 1.6f);
            CHECK(dead_height < 0.05f);
        }
    }
}

TEST_CASE("friction: a box on a slope stays put, or slides, by the coefficient") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            // A 20 degree ramp: tan(20) = 0.36. Combined friction is the geometric
            // mean, so mu = 0.6 (0.77 combined) holds and mu = 0.1 (0.32) slides —
            // at g (sin 20 - 0.32 cos 20) = 0.44 m/s^2, 0.88 m in two seconds.
            const float angle = 20.0f * tynima::math::kPi / 180.0f;
            BodyDesc ramp;
            ramp.shape = Shape::box(Vec3{5.0f, 0.5f, 5.0f});
            ramp.rotation = Quat::from_axis_angle(Vec3::unit_z(), angle);
            ramp.position = Vec3{0.0f, -0.5f, 0.0f};
            ramp.motion = MotionType::Static;
            ramp.friction = 1.0f;
            REQUIRE(world->create_body(ramp));

            BodyDesc box;
            box.shape = Shape::box(Vec3{0.2f});
            box.rotation = ramp.rotation;
            // Resting on the ramp surface, 1 m up the slope from the centre.
            const Vec3 along = ramp.rotation.rotate(Vec3::unit_x());
            const Vec3 up = ramp.rotation.rotate(Vec3::unit_y());
            box.position = along * 1.0f + up * 0.2f;
            box.friction = 0.6f;
            const BodyHandle grippy = world->create_body(box);
            box.position = along * -1.0f + up * 0.2f + Vec3{0.0f, 0.0f, 1.0f};
            box.friction = 0.1f;
            const BodyHandle slippy = world->create_body(box);
            REQUIRE(grippy);
            REQUIRE(slippy);
            const Vec3 grippy_start = world->body_state(grippy).position;
            const Vec3 slippy_start = world->body_state(slippy).position;

            run(*world, 2.0f);
            const float grippy_moved = length(world->body_state(grippy).position - grippy_start);
            const float slippy_moved = length(world->body_state(slippy).position - slippy_start);
            CHECK(grippy_moved < 0.03f);
            CHECK(slippy_moved > 0.7f);
            CHECK(slippy_moved < 1.1f);
            CHECK(world->body_state(slippy).position.y < slippy_start.y - 0.2f); // down the slope
        }
    }
}

TEST_CASE("handles are generational: a destroyed body's handle stops resolving") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            BodyDesc box;
            box.shape = Shape::box(Vec3{0.5f});
            const BodyHandle first = world->create_body(box);
            REQUIRE(first);
            CHECK(world->valid(first));
            CHECK(world->destroy_body(first));
            CHECK_FALSE(world->valid(first));
            CHECK_FALSE(world->destroy_body(first)); // twice is a no-op
            CHECK(world->body_count() == 0);

            const BodyHandle second = world->create_body(box);
            REQUIRE(second);
            CHECK(second.index == first.index);           // the slot is reused...
            CHECK(second.generation != first.generation); // ...under a new generation
            CHECK_FALSE(world->valid(first));
            CHECK(world->body_state(first).rotation.w == 1.0f); // a default state, not a crash

            // Every mutator is a no-op on the stale handle.
            world->add_impulse(first, Vec3{0.0f, 100.0f, 0.0f});
            world->set_velocity(first, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f});
            world->step(kStep);
            CHECK(world->body_state(second).linear_velocity.x == 0.0f);
        }
    }
}

TEST_CASE("destroying a body wakes whatever rested on it") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            REQUIRE(add_floor(*world));
            BodyDesc box;
            box.shape = Shape::box(Vec3{0.5f});
            box.position = Vec3{0.0f, 0.5f, 0.0f};
            const BodyHandle lower = world->create_body(box);
            box.position = Vec3{0.0f, 1.5f, 0.0f};
            const BodyHandle upper = world->create_body(box);
            REQUIRE(lower);
            REQUIRE(upper);
            run(*world, 2.0f);
            REQUIRE_FALSE(world->body_state(upper).active); // stacked and asleep

            // The lower box goes: the upper one does not hang in the air.
            CHECK(world->destroy_body(lower));
            run(*world, 1.0f);
            CHECK(world->body_state(upper).position.y == doctest::Approx(0.5f).epsilon(0.05));
        }
    }
}

TEST_CASE("the world has a fixed capacity") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            WorldDesc desc;
            desc.max_bodies = 2;
            auto world = backend.make(desc);
            BodyDesc box;
            box.shape = Shape::box(Vec3{0.5f});
            CHECK(world->create_body(box));
            CHECK(world->create_body(box));
            CHECK_FALSE(world->create_body(box));
            CHECK(world->body_count() == 2);
        }
    }
}

TEST_CASE("impulses, velocities and user data") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({.gravity = Vec3{0.0f}});
            BodyDesc box;
            box.shape = Shape::box(Vec3{0.5f});
            box.mass = 2.0f;
            box.user_data = 0xC0FFEEu;
            const BodyHandle body = world->create_body(box);
            REQUIRE(body);
            CHECK(world->user_data(body) == 0xC0FFEEu);

            world->add_impulse(body, Vec3{4.0f, 0.0f, 0.0f}); // 4 N.s on 2 kg = 2 m/s
            world->step(kStep);
            BodyState state = world->body_state(body);
            CHECK(state.linear_velocity.x == doctest::Approx(2.0f).epsilon(0.01));
            CHECK(state.position.x > 0.0f);

            world->set_velocity(body, Vec3{0.0f}, Vec3{0.0f, 1.0f, 0.0f});
            world->set_transform(body, Vec3{5.0f, 5.0f, 5.0f}, Quat::identity());
            world->step(kStep);
            state = world->body_state(body);
            CHECK(state.position.x == doctest::Approx(5.0f).epsilon(0.001));
            CHECK(state.angular_velocity.y == doctest::Approx(1.0f).epsilon(0.01));
            CHECK(state.rotation.y != 0.0f); // it has started to turn about y

            // An off-centre impulse spins it: 1 N.s at 0.5 m from the centre
            // on a 2 kg box of inertia 2/3 * 0.5^2 ... = 1/3 kg m^2 -> 1.5 rad/s.
            world->set_velocity(body, Vec3{0.0f}, Vec3{0.0f});
            world->add_impulse_at(body, Vec3{0.0f, 0.0f, 1.0f},
                                  world->body_state(body).position + Vec3{0.5f, 0, 0});
            world->step(kStep);
            state = world->body_state(body);
            CHECK(state.angular_velocity.y == doctest::Approx(-1.5f).epsilon(0.02));
        }
    }
}

TEST_CASE("a ray finds the nearest body, where and which way it faces") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            const BodyHandle floor = add_floor(*world);
            REQUIRE(floor);
            BodyDesc box;
            box.shape = Shape::box(Vec3{0.5f});
            box.position = Vec3{0.0f, 0.5f, 0.0f};
            box.motion = MotionType::Static;
            const BodyHandle cube = world->create_body(box);
            REQUIRE(cube);
            BodyDesc ball;
            ball.shape = Shape::sphere(0.5f);
            ball.position = Vec3{3.0f, 2.0f, 0.0f};
            ball.motion = MotionType::Static;
            const BodyHandle sphere = world->create_body(ball);
            REQUIRE(sphere);
            BodyDesc pill;
            pill.shape = Shape::capsule(0.25f, 0.5f);
            pill.position = Vec3{-3.0f, 2.0f, 0.0f};
            pill.rotation = Quat::from_axis_angle(Vec3::unit_z(), tynima::math::kPi * 0.5f); // lying along x
            pill.motion = MotionType::Static;
            const BodyHandle capsule = world->create_body(pill);
            REQUIRE(capsule);

            RayHit hit;
            REQUIRE(world->cast_ray(Vec3{0.0f, 5.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 10.0f, hit));
            CHECK(hit.body == cube);
            CHECK(hit.fraction == doctest::Approx(0.4f).epsilon(0.01)); // 4 m of 10
            CHECK(hit.point.y == doctest::Approx(1.0f).epsilon(0.01));
            CHECK(hit.normal.y == doctest::Approx(1.0f).epsilon(0.01));

            REQUIRE(world->cast_ray(Vec3{1.5f, 5.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 10.0f, hit));
            CHECK(hit.body == floor);
            CHECK(hit.point.y == doctest::Approx(0.0f).epsilon(0.01));

            REQUIRE(world->cast_ray(Vec3{3.0f, 5.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 10.0f, hit));
            CHECK(hit.body == sphere);
            CHECK(hit.point.y == doctest::Approx(2.5f).epsilon(0.01));
            CHECK(hit.normal.y == doctest::Approx(1.0f).epsilon(0.01));

            REQUIRE(world->cast_ray(Vec3{-3.2f, 5.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 10.0f, hit));
            CHECK(hit.body == capsule);
            CHECK(hit.point.y == doctest::Approx(2.25f).epsilon(0.01)); // on the straight part
            REQUIRE(world->cast_ray(Vec3{-3.7f, 5.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 10.0f, hit));
            CHECK(hit.body == capsule);
            CHECK(hit.point.y < 2.25f); // on the end cap, which curves away

            // Up: nothing there. Down, but too short a ray: nothing reached.
            CHECK_FALSE(world->cast_ray(Vec3{0.0f, 5.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 10.0f, hit));
            CHECK_FALSE(world->cast_ray(Vec3{0.0f, 5.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 3.0f, hit));
        }
    }
}

TEST_CASE("a shape offset from the body origin still lands on its base") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            REQUIRE(add_floor(*world));
            // A 1 m tall box whose origin is at its bottom face, like a mesh
            // modelled standing on y = 0: the shape sits 0.5 m above the origin.
            BodyDesc box;
            box.shape = Shape::box(Vec3{0.5f}, Vec3{0.0f, 0.5f, 0.0f});
            box.position = Vec3{0.0f, 2.0f, 0.0f};
            const BodyHandle body = world->create_body(box);
            REQUIRE(body);
            run(*world, 3.0f);
            CHECK(world->body_state(body).position.y == doctest::Approx(0.0f).epsilon(0.02));
        }
    }
}

TEST_CASE("a stack of boxes stands") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            REQUIRE(add_floor(*world));
            std::vector<BodyHandle> stack;
            for (int i = 0; i < 6; ++i) {
                BodyDesc box;
                box.shape = Shape::box(Vec3{0.25f});
                box.position = Vec3{0.0f, 0.25f + static_cast<float>(i) * 0.5f + 0.01f, 0.0f};
                box.friction = 0.6f;
                stack.push_back(world->create_body(box));
                REQUIRE(stack.back());
            }
            run(*world, 4.0f);
            for (int i = 0; i < 6; ++i) {
                const BodyState s = world->body_state(stack[static_cast<std::size_t>(i)]);
                CHECK(s.position.y == doctest::Approx(0.25f + static_cast<float>(i) * 0.5f).epsilon(0.03));
                CHECK(std::fabs(s.position.x) < 0.02f);
                CHECK(std::fabs(s.position.z) < 0.02f);
            }
            CHECK(world->active_body_count() == 0);
        }
    }
}

TEST_CASE("steps run on the engine job system and add no engine heap allocations") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            tynima::core::JobSystem jobs;
            auto world = backend.make({.jobs = &jobs});
            REQUIRE(add_floor(*world));
            BodyDesc box;
            box.shape = Shape::box(Vec3{0.25f});
            box.friction = 0.6f;
            for (int i = 0; i < 40; ++i) {
                const float column = static_cast<float>(i % 4);
                const float row = static_cast<float>(i / 4);
                box.position = Vec3{column * 0.55f, 0.3f + row * 0.55f, static_cast<float>(i % 3) * 0.55f};
                REQUIRE(world->create_body(box));
            }
            run(*world, 1.0f);
            // Jolt's memory is counted as external (its allocator is the engine's
            // operator new, under an ExternalAllocationScope); ours allocates
            // nothing per step by construction.
            const tynima::core::HeapAllocationScope heap;
            world->step(kStep);
            const std::uint64_t engine_allocations = heap.allocations();
            CHECK(engine_allocations == 0);
            CHECK(world->body_count() == 41);

            run(*world, 4.0f);
            CHECK(world->active_body_count() == 0); // the pile settled
            for (int i = 0; i < 40; ++i) {
                // Nothing fell through the floor or flew off the slab.
                const BodyState s = world->body_state(BodyHandle{static_cast<std::uint32_t>(i + 1), 1});
                CHECK(s.position.y > 0.2f);
                CHECK(std::fabs(s.position.x) < 10.0f);
            }
        }
    }
}

TEST_CASE("worlds come and go; the runtime follows the last one") {
    for (int i = 0; i < 3; ++i) {
        auto a = create_jolt_world({});
        auto b = create_jolt_world({});
        auto c = create_tynima_world({});
        BodyDesc box;
        box.shape = Shape::box(Vec3{0.5f});
        REQUIRE(a->create_body(box));
        REQUIRE(b->create_body(box));
        REQUIRE(c->create_body(box));
        a->step(kStep);
        b->step(kStep);
        c->step(kStep);
    }
    CHECK(true);
}

// The sandbox's pile through both worlds, from the same drop. A pile is
// chaotic — one seed's widest-flung bottle says little — so the comparison
// is on the middle of the distribution, and the hard checks are the ones
// that must hold for any solver: nothing sinks, nothing flies off, all
// asleep at the end.
namespace {

struct PileReport {
    int upright = 0, toppled = 0, awake = 0;
    float median_radius = 0.0f, p90_radius = 0.0f, max_radius = 0.0f, top = 0.0f, lowest = 1.0e9f;
    double step_ms = 0.0;
};

PileReport drop_pile(PhysicsWorld& world, float height_step, float seconds) {
    BodyDesc floor;
    floor.shape = Shape::box(Vec3{6.0f, 0.25f, 6.0f});
    floor.position = Vec3{0.0f, -0.25f, 0.0f};
    floor.motion = MotionType::Static;
    floor.friction = 0.6f;
    REQUIRE(world.create_body(floor));
    Random random;
    std::vector<BodyHandle> bodies;
    for (int i = 0; i < 100; ++i) {
        BodyDesc body;
        body.shape = Shape::box(Vec3{0.055f, 0.13f, 0.055f});
        body.position = Vec3{random.range(-0.4f, 0.4f), 0.5f + static_cast<float>(i) * height_step,
                             random.range(-0.4f, 0.4f)};
        body.rotation = Quat::from_axis_angle(normalize(random.vec(-1.0f, 1.0f)), random.range(0.0f, 1.0f));
        body.angular_velocity = random.vec(-2.0f, 2.0f);
        body.mass = 0.6f;
        body.friction = 0.5f;
        body.restitution = 0.1f;
        bodies.push_back(world.create_body(body));
        REQUIRE(bodies.back());
    }
    const auto t0 = std::chrono::steady_clock::now();
    run(world, seconds);
    PileReport report;
    const double total_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    report.step_ms = total_ms / static_cast<double>(seconds / kStep);
    report.awake = static_cast<int>(world.active_body_count());
    std::vector<float> radii;
    for (const BodyHandle body : bodies) {
        const BodyState s = world.body_state(body);
        (s.rotation.rotate(Vec3::unit_y()).y > 0.7f ? report.upright : report.toppled)++;
        radii.push_back(std::sqrt(s.position.x * s.position.x + s.position.z * s.position.z));
        report.top = std::max(report.top, s.position.y);
        report.lowest = std::min(report.lowest, s.position.y);
    }
    std::sort(radii.begin(), radii.end());
    report.median_radius = radii[50];
    report.p90_radius = radii[90];
    report.max_radius = radii[99];
    return report;
}

void report(const char* name, float height_step, const PileReport& r) {
    MESSAGE(std::string(name) << " (heights every " << height_step << " m): " << r.upright << " upright, "
                              << r.toppled << " toppled; radius median " << r.median_radius << " m, p90 "
                              << r.p90_radius << " m, max " << r.max_radius << " m; top " << r.top
                              << " m, lowest " << r.lowest << " m; " << r.awake << " awake at the end; "
                              << r.step_ms << " ms per step");
}

} // namespace

TEST_CASE("the pile, dropped through both worlds") {
    // The sandbox's drop (up to 3.5 m), and a tall one (12.5 m: 15 m/s impacts).
    for (const float height_step : {0.03f, 0.12f}) {
        PileReport reports[2];
        for (int k = 0; k < 2; ++k) {
            auto world = kBackends[k].make({});
            reports[k] = drop_pile(*world, height_step, 8.0f);
            report(kBackends[k].name, height_step, reports[k]);
        }
        for (const PileReport& r : reports) {
            CHECK(r.lowest > 0.03f);    // nothing sank into the floor
            CHECK(r.max_radius < 6.0f); // nothing was thrown off the slab
            CHECK(r.top < 1.0f);        // a heap, not a tower
            CHECK(r.awake == 0);        // settled
            CHECK(r.toppled > 50);      // a heap: most lie down
        }
        // Ours lands in the same regime as the reference.
        const PileReport& jolt = reports[0];
        const PileReport& ours = reports[1];
        CHECK(ours.median_radius < jolt.median_radius * 1.5f + 0.1f);
        CHECK(ours.median_radius > jolt.median_radius * 0.5f - 0.1f);
        CHECK(ours.p90_radius < jolt.p90_radius * 1.6f + 0.2f);
    }
}
