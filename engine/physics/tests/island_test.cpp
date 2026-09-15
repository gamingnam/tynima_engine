#include "backends.h"

#include <doctest/doctest.h>
#include <tynima/core/jobs.h>
#include <tynima/physics/fixed_step.h>
#include <tynima/physics/physics.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace tynima::physics;
using tynima::math::Quat;
using tynima::math::Vec3;

namespace {

// A column of boxes standing on the floor at x.
std::vector<BodyHandle> add_stack(PhysicsWorld& world, float x, int height) {
    std::vector<BodyHandle> stack;
    BodyDesc box;
    box.shape = Shape::box(Vec3{0.25f});
    for (int i = 0; i < height; ++i) {
        box.position = Vec3{x, 0.25f + 0.5f * static_cast<float>(i), 0.0f};
        stack.push_back(world.create_body(box));
        REQUIRE(stack.back());
    }
    return stack;
}

int awake_in(const PhysicsWorld& world, const std::vector<BodyHandle>& bodies) {
    int awake = 0;
    for (const BodyHandle body : bodies) {
        awake += world.body_state(body).active ? 1 : 0;
    }
    return awake;
}

} // namespace

TEST_CASE("islands: what does not touch is left alone") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            REQUIRE(add_floor(*world));
            const std::vector<BodyHandle> left = add_stack(*world, -3.0f, 3);
            const std::vector<BodyHandle> right = add_stack(*world, 3.0f, 3);
            run(*world, 3.0f);
            REQUIRE(world->active_body_count() == 0); // both stacks settled and asleep

            // A knock on the top of one stack wakes that stack, not the other.
            world->add_impulse(left.back(), Vec3{2.0f, 0.0f, 0.0f});
            run(*world, 0.2f);
            CHECK(awake_in(*world, left) == 3);
            CHECK(awake_in(*world, right) == 0);
            CHECK(world->active_body_count() == 3);

            // And it settles again on its own.
            run(*world, 3.0f);
            CHECK(world->active_body_count() == 0);
            for (const BodyHandle body : right) {
                CHECK(world->body_state(body).position.x == doctest::Approx(3.0f).epsilon(0.01));
            }
        }
    }
}

TEST_CASE("islands: bodies that touch wake together and sleep together") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            REQUIRE(add_floor(*world));
            const std::vector<BodyHandle> stack = add_stack(*world, 0.0f, 1);
            const BodyHandle lower = stack.front();
            run(*world, 2.0f);
            REQUIRE_FALSE(world->body_state(lower).active);

            // A box dropped on the sleeper wakes it: from then on the two are
            // awake together, and they go back to sleep on the same step.
            BodyDesc box;
            box.shape = Shape::box(Vec3{0.25f});
            box.position = Vec3{0.1f, 1.5f, 0.0f};
            const BodyHandle upper = world->create_body(box);
            REQUIRE(upper);
            bool touched = false;
            int split_steps = 0;    // steps with one awake and the other not, once they touch
            int slept_together = -1; // the step both went quiet on
            for (int i = 0; i < 300; ++i) {
                world->step(kStep);
                const bool lower_awake = world->body_state(lower).active;
                const bool upper_awake = world->body_state(upper).active;
                if (!touched) {
                    touched = lower_awake; // the drop reached it
                    continue;
                }
                if (lower_awake != upper_awake) {
                    ++split_steps;
                }
                if (!lower_awake && !upper_awake && slept_together < 0) {
                    slept_together = i;
                }
            }
            MESSAGE(std::string(backend.name) << ": both asleep after step " << slept_together << ", "
                                              << split_steps << " step(s) with only one awake");
            CHECK(touched);
            CHECK(split_steps == 0);
            CHECK(slept_together > 0);
            CHECK(world->body_state(upper).position.y == doctest::Approx(0.75f).epsilon(0.05));
        }
    }
}

TEST_CASE("islands: a chain is one island, joints and all") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            const Vec3 top{0.0f, 4.0f, 0.0f};
            std::vector<BodyHandle> bobs;
            BodyDesc bob;
            bob.shape = Shape::sphere(0.05f);
            bob.mass = 0.5f;
            for (int i = 0; i < 5; ++i) {
                bob.position = top - Vec3{0.0f, 0.4f * static_cast<float>(i + 1), 0.0f};
                bobs.push_back(world->create_body(bob));
                REQUIRE(bobs.back());
                JointDesc link;
                link.a = bobs.back();
                if (i == 0) {
                    link.anchor_b = top;
                } else {
                    link.b = bobs[static_cast<std::size_t>(i) - 1];
                }
                REQUIRE(world->create_joint(link));
            }
            run(*world, 3.0f);
            REQUIRE(world->active_body_count() == 0); // hanging still, asleep

            // A kick at the bottom wakes the whole chain within a few steps.
            world->add_impulse(bobs.back(), Vec3{0.5f, 0.0f, 0.0f});
            run(*world, 0.2f);
            CHECK(awake_in(*world, bobs) == 5);
            // The top bob moves too: the kick travelled up the links.
            run(*world, 1.0f);
            float top_swing = 0.0f;
            for (int i = 0; i < 120; ++i) {
                world->step(kStep);
                top_swing = std::max(top_swing, std::abs(world->body_state(bobs.front()).position.x));
            }
            CHECK(top_swing > 0.01f);
        }
    }
}

TEST_CASE("islands: something kinematic pushing on a body keeps it awake") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            BodyDesc platform;
            platform.shape = Shape::box(Vec3{2.0f, 0.25f, 2.0f});
            platform.position = Vec3{0.0f, -0.25f, 0.0f};
            platform.motion = MotionType::Kinematic;
            const BodyHandle mover = world->create_body(platform);
            REQUIRE(mover);
            const std::vector<BodyHandle> box = add_stack(*world, 0.0f, 1);
            run(*world, 2.0f);
            REQUIRE_FALSE(world->body_state(box.front()).active);

            // The platform creeps sideways, slower than the sleep threshold
            // itself: the box on it rides along, awake, the whole time.
            world->set_velocity(mover, Vec3{0.02f, 0.0f, 0.0f}, Vec3{0.0f});
            int asleep_steps = 0;
            for (int i = 0; i < 180; ++i) {
                world->step(kStep);
                asleep_steps += world->body_state(box.front()).active ? 0 : 1;
            }
            MESSAGE(std::string(backend.name) << ": box asleep for " << asleep_steps << " of 180 steps; x = "
                                              << world->body_state(box.front()).position.x);
            CHECK(asleep_steps < 20); // the first steps, before the platform's motion reaches it
            CHECK(world->body_state(box.front()).position.x > 0.04f);
        }
    }
}

TEST_CASE("the same scene with and without the job system comes out bit for bit the same") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            tynima::core::JobSystem jobs;
            auto threaded = backend.make({.jobs = &jobs});
            auto serial = backend.make({});
            std::vector<BodyHandle> handles[2];
            int k = 0;
            for (PhysicsWorld* world : {threaded.get(), serial.get()}) {
                REQUIRE(add_floor(*world));
                // Several separate heaps, so there are islands to spread over the workers.
                BodyDesc box;
                box.shape = Shape::box(Vec3{0.2f});
                box.friction = 0.5f;
                for (int i = 0; i < 120; ++i) {
                    const float heap = static_cast<float>(i % 6) * 2.5f - 6.0f;
                    const float n = static_cast<float>(i / 6);
                    box.position =
                        Vec3{heap + 0.15f * std::sin(n), 0.5f + 0.45f * n, 0.1f * std::cos(n * 1.7f)};
                    box.rotation = Quat::from_axis_angle(normalize(Vec3{0.3f, 1.0f, 0.2f}), n * 0.37f);
                    handles[k].push_back(world->create_body(box));
                    REQUIRE(handles[k].back());
                }
                ++k;
            }
            run(*threaded, 3.0f);
            run(*serial, 3.0f);
            int different = 0;
            for (std::size_t i = 0; i < handles[0].size(); ++i) {
                const BodyState a = threaded->body_state(handles[0][i]);
                const BodyState b = serial->body_state(handles[1][i]);
                if (std::memcmp(&a.position, &b.position, sizeof(Vec3)) != 0 ||
                    std::memcmp(&a.rotation, &b.rotation, sizeof(Quat)) != 0) {
                    ++different;
                }
            }
            MESSAGE(std::string(backend.name) << ": " << different << " of " << handles[0].size()
                                              << " bodies differ between the threaded and the serial run");
            CHECK(different == 0);
            CHECK(threaded->active_body_count() == serial->active_body_count());
        }
    }
}

TEST_CASE("the fixed stepper steps in whole steps and carries the rest") {
    auto world = create_tynima_world({});
    BodyDesc ball;
    ball.shape = Shape::sphere(0.1f);
    ball.position = Vec3{0.0f, 50.0f, 0.0f};
    const BodyHandle body = world->create_body(ball);
    REQUIRE(body);

    FixedStepper stepper;
    stepper.step = 0.25f; // exact in binary, so the arithmetic below is too
    int calls = 0;
    const auto count = [&](float step) {
        CHECK(step == 0.25f);
        ++calls;
    };
    CHECK(stepper.advance(*world, 0.375f, count) == 1); // one step, half of one left over
    CHECK(calls == 1);
    CHECK(stepper.alpha() == doctest::Approx(0.5f));
    CHECK(stepper.advance(*world, 0.125f, count) == 1); // the half fills up
    CHECK(stepper.alpha() == doctest::Approx(0.0f));
    CHECK(stepper.advance(*world, 0.1f, count) == 0); // not enough for a step: nothing happens
    CHECK(stepper.alpha() == doctest::Approx(0.4f));
    CHECK(calls == 2);
    // A huge frame is cut to max_steps, and time is dropped rather than caught up.
    stepper.max_steps = 3;
    CHECK(stepper.advance(*world, 100.0f, count) == 3);
    CHECK(stepper.alpha() == doctest::Approx(0.0f));
    CHECK(stepper.advance(*world, -1.0f, count) == 0); // and a negative dt is ignored
    CHECK(calls == 5);
    CHECK(stepper.total_steps == 5);
    // The body saw exactly five quarter-second steps of gravity, less 5 %/s of damping.
    CHECK(world->body_state(body).linear_velocity.y == doctest::Approx(-9.81f * 1.25f).epsilon(0.05));
    CHECK(stepper.advance(*world, 0.25f) == 1); // the no-callback form
}
