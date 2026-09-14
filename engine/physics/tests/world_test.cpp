#include <doctest/doctest.h>
#include <tynima/core/jobs.h>
#include <tynima/core/memory.h>
#include <tynima/physics/physics.h>

#include <cmath>
#include <cstring>
#include <memory>

using namespace tynima::physics;
using tynima::math::Quat;
using tynima::math::Vec3;

namespace {

constexpr float kStep = 1.0f / 60.0f;

// A 20 m square slab whose top face is y = 0.
BodyHandle add_floor(PhysicsWorld& world) {
    BodyDesc floor;
    floor.shape = Shape::box(Vec3{10.0f, 0.5f, 10.0f});
    floor.position = Vec3{0.0f, -0.5f, 0.0f};
    floor.motion = MotionType::Static;
    return world.create_body(floor);
}

void run(PhysicsWorld& world, float seconds) {
    for (float t = 0.0f; t < seconds; t += kStep) {
        world.step(kStep);
    }
}

} // namespace

TEST_CASE("the backend is Jolt") {
    auto world = create_jolt_world({});
    REQUIRE(world != nullptr);
    CHECK(std::strncmp(world->backend_name(), "Jolt 5.", 7) == 0);
    CHECK(world->body_count() == 0);
}

TEST_CASE("a box dropped on the floor lands, settles and falls asleep") {
    auto world = create_jolt_world({});
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
    CHECK_FALSE(rest.active); // Jolt puts a body that stopped moving to sleep
    CHECK(world->active_body_count() == 0);
}

TEST_CASE("restitution decides how much of a bounce comes back") {
    auto world = create_jolt_world({});
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

TEST_CASE("handles are generational: a destroyed body's handle stops resolving") {
    auto world = create_jolt_world({});
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
    CHECK(second.index == first.index); // the slot is reused...
    CHECK(second.generation != first.generation); // ...under a new generation
    CHECK_FALSE(world->valid(first));
    CHECK(world->body_state(first).rotation.w == 1.0f); // a default state, not a crash

    // Every mutator is a no-op on the stale handle.
    world->add_impulse(first, Vec3{0.0f, 100.0f, 0.0f});
    world->set_velocity(first, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f});
    world->step(kStep);
    CHECK(world->body_state(second).linear_velocity.x == 0.0f);
}

TEST_CASE("the world has a fixed capacity") {
    WorldDesc desc;
    desc.max_bodies = 2;
    auto world = create_jolt_world(desc);
    BodyDesc box;
    box.shape = Shape::box(Vec3{0.5f});
    CHECK(world->create_body(box));
    CHECK(world->create_body(box));
    CHECK_FALSE(world->create_body(box));
    CHECK(world->body_count() == 2);
}

TEST_CASE("impulses, velocities and user data") {
    auto world = create_jolt_world({.gravity = Vec3{0.0f}});
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
}

TEST_CASE("a ray finds the nearest body, where and which way it faces") {
    auto world = create_jolt_world({});
    const BodyHandle floor = add_floor(*world);
    REQUIRE(floor);
    BodyDesc box;
    box.shape = Shape::box(Vec3{0.5f});
    box.position = Vec3{0.0f, 0.5f, 0.0f};
    box.motion = MotionType::Static;
    const BodyHandle cube = world->create_body(box);
    REQUIRE(cube);

    RayHit hit;
    REQUIRE(world->cast_ray(Vec3{0.0f, 5.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 10.0f, hit));
    CHECK(hit.body == cube);
    CHECK(hit.fraction == doctest::Approx(0.4f).epsilon(0.01)); // 4 m of 10
    CHECK(hit.point.y == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(hit.normal.y == doctest::Approx(1.0f).epsilon(0.01));

    REQUIRE(world->cast_ray(Vec3{3.0f, 5.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 10.0f, hit));
    CHECK(hit.body == floor);
    CHECK(hit.point.y == doctest::Approx(0.0f).epsilon(0.01));

    CHECK_FALSE(world->cast_ray(Vec3{0.0f, 5.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 10.0f, hit)); // up: nothing
    CHECK_FALSE(world->cast_ray(Vec3{0.0f, 5.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 3.0f, hit)); // too short
}

TEST_CASE("a shape offset from the body origin still lands on its base") {
    auto world = create_jolt_world({});
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

TEST_CASE("steps run on the engine job system and add no engine heap allocations") {
    tynima::core::JobSystem jobs;
    auto world = create_jolt_world({.jobs = &jobs});
    REQUIRE(add_floor(*world));
    BodyDesc box;
    box.shape = Shape::box(Vec3{0.25f});
    box.friction = 0.6f;
    for (int i = 0; i < 40; ++i) {
        box.position = Vec3{static_cast<float>(i % 4) * 0.55f, 0.3f + static_cast<float>(i / 4) * 0.55f,
                            static_cast<float>(i % 3) * 0.55f};
        REQUIRE(world->create_body(box));
    }
    run(*world, 1.0f);
    // Jolt's memory is counted as external (its allocator is the engine's
    // operator new, under an ExternalAllocationScope); the module's own code
    // allocates nothing per step.
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

TEST_CASE("worlds come and go; the runtime follows the last one") {
    for (int i = 0; i < 3; ++i) {
        auto a = create_jolt_world({});
        auto b = create_jolt_world({});
        BodyDesc box;
        box.shape = Shape::box(Vec3{0.5f});
        REQUIRE(a->create_body(box));
        REQUIRE(b->create_body(box));
        a->step(kStep);
        b->step(kStep);
    }
    CHECK(true);
}
