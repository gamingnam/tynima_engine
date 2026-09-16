#include "backends.h"

#include <doctest/doctest.h>
#include <tynima/core/jobs.h>
#include <tynima/physics/physics.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace tynima::physics;
using tynima::math::Quat;
using tynima::math::Vec3;

namespace {

// A heap of every shape with a chain hanging over it: contacts, friction,
// joints and sleep, so a hash that missed any of them would show. Two calls
// build two worlds that are the same in every bit.
std::vector<BodyHandle> populate(PhysicsWorld& world) {
    std::vector<BodyHandle> bodies;
    REQUIRE(add_floor(world));
    BodyDesc body;
    body.friction = 0.4f;
    for (int i = 0; i < 30; ++i) {
        const float n = static_cast<float>(i);
        switch (i % 3) {
        case 0:
            body.shape = Shape::box(Vec3{0.2f});
            break;
        case 1:
            body.shape = Shape::sphere(0.2f);
            break;
        default:
            body.shape = Shape::capsule(0.15f, 0.2f);
            break;
        }
        body.position = Vec3{0.6f * std::sin(n * 0.9f), 0.5f + 0.5f * n, 0.6f * std::cos(n * 1.3f)};
        body.rotation = Quat::from_axis_angle(normalize(Vec3{0.2f, 1.0f, 0.5f}), n * 0.41f);
        bodies.push_back(world.create_body(body));
        REQUIRE(bodies.back());
    }
    const Vec3 top{2.5f, 3.0f, 0.0f};
    BodyDesc bob;
    bob.shape = Shape::sphere(0.06f);
    bob.mass = 0.3f;
    for (int i = 0; i < 4; ++i) {
        // Hung out sideways, so the chain swings rather than hangs.
        bob.position = top + Vec3{0.3f * static_cast<float>(i + 1), 0.0f, 0.0f};
        bodies.push_back(world.create_body(bob));
        REQUIRE(bodies.back());
        JointDesc link;
        link.a = bodies.back();
        if (i == 0) {
            link.anchor_b = top;
        } else {
            link.b = bodies[bodies.size() - 2];
        }
        REQUIRE(world.create_joint(link));
    }
    return bodies;
}

} // namespace

TEST_CASE("the state hash is the same for the same history and different for any other") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto first = backend.make({});
            auto second = backend.make({});
            const std::vector<BodyHandle> bodies = populate(*first);
            (void)populate(*second);
            const std::uint64_t at_rest = first->state_hash();
            CHECK(at_rest == second->state_hash());

            // Every step the world changes, and so does the hash: the same
            // way in both worlds.
            std::uint64_t previous = at_rest;
            int unchanged = 0;
            for (int i = 0; i < 120; ++i) {
                first->step(kStep);
                second->step(kStep);
                const std::uint64_t now = first->state_hash();
                REQUIRE(now == second->state_hash());
                unchanged += now == previous ? 1 : 0;
                previous = now;
            }
            CHECK(unchanged == 0);

            // A nudge too small to see is not too small to hash.
            second->add_impulse(bodies[7], Vec3{1e-4f, 0.0f, 0.0f});
            CHECK(first->state_hash() != second->state_hash());
            run(*first, 1.0f);
            run(*second, 1.0f);
            CHECK(first->state_hash() != second->state_hash());
        }
    }
}

TEST_CASE("the state hash covers whether a body is awake") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            REQUIRE(add_floor(*world));
            BodyDesc box;
            box.shape = Shape::box(Vec3{0.25f});
            box.position = Vec3{0.0f, 0.25f, 0.0f};
            const BodyHandle body = world->create_body(box);
            REQUIRE(body);
            run(*world, 2.0f);
            REQUIRE_FALSE(world->body_state(body).active);
            const std::uint64_t asleep = world->state_hash();

            // Setting the velocity it already has wakes it and changes
            // nothing else; the hash still tells the two apart.
            world->set_velocity(body, Vec3{0.0f}, Vec3{0.0f});
            REQUIRE(world->body_state(body).active);
            CHECK(world->state_hash() != asleep);
        }
    }
}

TEST_CASE("the state hash does not depend on how many threads stepped the world") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            tynima::core::JobSystem jobs;
            auto threaded = backend.make({.jobs = &jobs});
            auto serial = backend.make({});
            (void)populate(*threaded);
            (void)populate(*serial);
            for (int i = 0; i < 180; ++i) {
                threaded->step(kStep);
                serial->step(kStep);
                REQUIRE(threaded->state_hash() == serial->state_hash());
            }
            MESSAGE(std::string(backend.name) << ": " << threaded->active_body_count() << " of "
                                              << threaded->body_count() << " bodies still awake after 3 s");
        }
    }
}
