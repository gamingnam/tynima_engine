#include "backends.h"

#include <doctest/doctest.h>
#include <tynima/physics/character.h>
#include <tynima/physics/physics.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace tynima::physics;
using tynima::math::kPi;
using tynima::math::Quat;
using tynima::math::Vec3;

namespace {

// Radius 0.3 + half height 0.5: where the capsule's centre is when it stands on y = 0.
constexpr float kStandHeight = 0.8f;

// Drives the controller for `seconds`, jumping on the first frame if asked,
// and hands every frame's time to `each` after the step.
template <typename Fn>
void advance(PhysicsWorld& world, CharacterController& character, Vec3 walk, bool jump, float seconds,
             Fn&& each) {
    const int steps = static_cast<int>(std::round(seconds / kStep));
    for (int i = 0; i < steps; ++i) {
        character.move(walk, jump && i == 0, kStep);
        world.step(kStep);
        each(static_cast<float>(i + 1) * kStep);
    }
}

void advance(PhysicsWorld& world, CharacterController& character, Vec3 walk, bool jump, float seconds) {
    advance(world, character, walk, jump, seconds, [](float) {});
}

CharacterDesc spawn_at(Vec3 position) {
    CharacterDesc desc;
    desc.position = position;
    return desc;
}

} // namespace

TEST_CASE("a character stands on the floor and walks at the speed it is asked to") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            REQUIRE(add_floor(*world));
            CharacterController character(*world, spawn_at(Vec3{0.0f, 1.0f, 0.0f}));
            REQUIRE(world->valid(character.body()));
            CHECK(world->body_count() == 2);

            // Dropped a little way, it lands and stands still.
            advance(*world, character, Vec3{0.0f}, false, 1.0f);
            CHECK(character.on_ground());
            CHECK(character.position().y == doctest::Approx(kStandHeight).epsilon(0.02));
            CHECK(length(character.velocity()) < 0.01f);
            CHECK(character.ground_normal().y > 0.999f);
            // It never turns: the capsule stays upright whatever happens.
            CHECK(world->body_state(character.body()).rotation.rotate(Vec3{0.0f, 1.0f, 0.0f}).y > 0.9999f);

            // Two seconds at 2 m/s is four metres, and it stays on the ground the whole way.
            float lowest = 10.0f;
            float highest = 0.0f;
            advance(*world, character, Vec3{2.0f, 0.0f, 0.0f}, false, 2.0f, [&](float) {
                lowest = std::min(lowest, character.position().y);
                highest = std::max(highest, character.position().y);
                CHECK(character.on_ground());
            });
            MESSAGE(std::string(backend.name) << ": walked to x = " << character.position().x
                                              << ", height between " << lowest << " and " << highest);
            CHECK(character.position().x == doctest::Approx(4.0f).epsilon(0.05));
            CHECK(std::abs(character.position().z) < 0.01f);
            CHECK(lowest > kStandHeight - 0.02f);
            CHECK(highest < kStandHeight + 0.02f);

            // Asked to stop, it stops within a frame or two.
            advance(*world, character, Vec3{0.0f}, false, 0.25f);
            CHECK(length(character.velocity()) < 0.01f);
            CHECK(character.position().x == doctest::Approx(4.0f).epsilon(0.05));
        }
    }
}

TEST_CASE("a character walking into a wall stops at it") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            REQUIRE(add_floor(*world));
            BodyDesc wall;
            wall.shape = Shape::box(Vec3{0.1f, 2.0f, 2.0f});
            wall.position = Vec3{2.0f, 2.0f, 0.0f}; // its near face at x = 1.9
            wall.motion = MotionType::Static;
            REQUIRE(world->create_body(wall));
            CharacterController character(*world, spawn_at(Vec3{0.0f, kStandHeight, 0.0f}));

            float highest = 0.0f;
            advance(*world, character, Vec3{2.0f, 0.0f, 0.0f}, false, 3.0f,
                    [&](float) { highest = std::max(highest, character.position().y); });
            MESSAGE(std::string(backend.name) << ": stopped at x = " << character.position().x);
            // Its radius from the face, neither through it nor up it.
            CHECK(character.position().x == doctest::Approx(1.9f - 0.3f).epsilon(0.03));
            CHECK(highest < kStandHeight + 0.03f);
            CHECK(character.on_ground());
            CHECK(character.ground_normal().y > 0.999f); // the wall is not ground
        }
    }
}

TEST_CASE("a character climbs a slope at full speed and holds still on it") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            const float slope = 20.0f * kPi / 180.0f;
            BodyDesc ramp;
            ramp.shape = Shape::box(Vec3{5.0f, 0.5f, 5.0f});
            ramp.rotation = Quat::from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, slope); // rises towards +x
            ramp.motion = MotionType::Static;
            REQUIRE(world->create_body(ramp));
            CharacterController character(*world, spawn_at(Vec3{-2.0f, 1.5f, 0.0f}));

            // Lands on the slope and does not creep down it.
            advance(*world, character, Vec3{0.0f}, false, 1.0f);
            REQUIRE(character.on_ground());
            CHECK(character.ground_normal().y == doctest::Approx(std::cos(slope)).epsilon(0.01));
            const Vec3 rest = character.position();
            advance(*world, character, Vec3{0.0f}, false, 1.0f);
            CHECK(length(character.position() - rest) < 0.01f);

            // Two seconds at 2 m/s up the slope: four metres along it.
            advance(*world, character, Vec3{2.0f, 0.0f, 0.0f}, false, 2.0f,
                    [&](float) { CHECK(character.on_ground()); });
            const Vec3 moved = character.position() - rest;
            MESSAGE(std::string(backend.name) << ": climbed " << moved.x << " along, " << moved.y << " up");
            CHECK(moved.x == doctest::Approx(4.0f * std::cos(slope)).epsilon(0.05));
            CHECK(moved.y == doctest::Approx(4.0f * std::sin(slope)).epsilon(0.08));
        }
    }
}

TEST_CASE("a character jumps as high as its jump speed says and lands again") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            REQUIRE(add_floor(*world));
            CharacterDesc desc = spawn_at(Vec3{0.0f, kStandHeight, 0.0f});
            desc.jump_speed = 5.0f;
            CharacterController character(*world, desc);
            advance(*world, character, Vec3{0.0f}, false, 0.5f);
            REQUIRE(character.on_ground());

            float peak = 0.0f;
            float landed_at = -1.0f;
            bool left_ground = false;
            advance(*world, character, Vec3{0.0f}, true, 2.0f, [&](float t) {
                peak = std::max(peak, character.position().y);
                if (!character.on_ground()) {
                    left_ground = true;
                } else if (left_ground && landed_at < 0.0f) {
                    landed_at = t;
                }
            });
            // v^2 / 2g = 1.27 m up, back down after 2 v / g = 1.02 s.
            MESSAGE(std::string(backend.name) << ": jumped " << peak - kStandHeight << " m, landed after "
                                              << landed_at << " s");
            CHECK(left_ground);
            CHECK(peak - kStandHeight == doctest::Approx(25.0f / (2.0f * 9.81f)).epsilon(0.05));
            CHECK(landed_at == doctest::Approx(2.0f * 5.0f / 9.81f).epsilon(0.1));
            CHECK(character.on_ground());
            CHECK(character.position().y == doctest::Approx(kStandHeight).epsilon(0.02));

            // Jumping is only for the ground: asked again in the air, nothing happens.
            float second_peak = 0.0f;
            advance(*world, character, Vec3{0.0f}, true, 0.3f, [&](float) {
                second_peak = std::max(second_peak, character.position().y);
                character.move(Vec3{0.0f}, true, 0.0f); // asking mid-air changes nothing
            });
            CHECK(second_peak < kStandHeight + 1.5f);
        }
    }
}

TEST_CASE("a character shoves a light box along and is not shoved by it") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            REQUIRE(add_floor(*world));
            BodyDesc crate;
            crate.shape = Shape::box(Vec3{0.25f});
            crate.position = Vec3{1.5f, 0.25f, 0.0f};
            crate.mass = 5.0f;
            const BodyHandle box = world->create_body(crate);
            REQUIRE(box);
            CharacterController character(*world, spawn_at(Vec3{0.0f, kStandHeight, 0.0f}));

            advance(*world, character, Vec3{1.5f, 0.0f, 0.0f}, false, 2.0f);
            const float box_x = world->body_state(box).position.x;
            MESSAGE(std::string(backend.name) << ": box pushed to x = " << box_x << ", character at "
                                              << character.position().x);
            CHECK(box_x > 2.0f);
            CHECK(character.position().x > 1.5f);
            CHECK(box_x > character.position().x);
            CHECK(character.on_ground());
            CHECK(character.position().y == doctest::Approx(kStandHeight).epsilon(0.03));
        }
    }
}

TEST_CASE("the controller owns its body") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            BodyHandle body;
            {
                CharacterController character(*world, spawn_at(Vec3{0.0f, 1.0f, 0.0f}));
                body = character.body();
                CHECK(world->valid(body));
                CHECK(world->body_count() == 1);
            }
            CHECK_FALSE(world->valid(body));
            CHECK(world->body_count() == 0);
        }
    }
}
