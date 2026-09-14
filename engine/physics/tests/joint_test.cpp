#include "backends.h"

#include <doctest/doctest.h>
#include <tynima/physics/physics.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace tynima::physics;
using tynima::math::kPi;
using tynima::math::Quat;
using tynima::math::Vec3;

namespace {

constexpr float kGravity = 9.81f;

BodyHandle add_bob(PhysicsWorld& world, Vec3 position, float mass = 1.0f) {
    BodyDesc bob;
    bob.shape = Shape::sphere(0.05f);
    bob.position = position;
    bob.mass = mass;
    return world.create_body(bob);
}

// A 1 m wide, 2 m tall door standing on the x axis with its hinge edge on
// the y axis at x = 0, so the pin runs through (0, 1, 0).
constexpr Vec3 kDoorHalf{0.5f, 1.0f, 0.05f};
constexpr float kDoorMass = 20.0f;
constexpr Vec3 kPin{0.0f, 1.0f, 0.0f};

BodyHandle add_door(PhysicsWorld& world) {
    BodyDesc door;
    door.shape = Shape::box(kDoorHalf);
    door.position = Vec3{0.5f, 1.0f, 0.0f};
    door.mass = kDoorMass;
    return world.create_body(door);
}

JointHandle hinge_door(PhysicsWorld& world, BodyHandle door, bool limited = false, float min_angle = -kPi,
                       float max_angle = kPi) {
    JointDesc hinge;
    hinge.type = JointType::Hinge;
    hinge.a = door;
    hinge.anchor_a = Vec3{-0.5f, 0.0f, 0.0f};
    hinge.anchor_b = kPin; // b is the world: world space
    hinge.axis_a = Vec3{0.0f, 1.0f, 0.0f};
    hinge.axis_b = Vec3{0.0f, 1.0f, 0.0f};
    hinge.limited = limited;
    hinge.min_angle = min_angle;
    hinge.max_angle = max_angle;
    return world.create_joint(hinge);
}

// Where the door's hinge edge is right now, mid-height.
Vec3 door_pin(const PhysicsWorld& world, BodyHandle door) {
    const BodyState s = world.body_state(door);
    return s.position + s.rotation.rotate(Vec3{-0.5f, 0.0f, 0.0f});
}

// The door's up axis has to stay the world's: a hinge lets it yaw, not tilt.
float door_upright(const PhysicsWorld& world, BodyHandle door) {
    return world.body_state(door).rotation.rotate(Vec3{0.0f, 1.0f, 0.0f}).y;
}

// A 10 N s push, sideways, at the door's free edge: with the door's moment of
// inertia about the pin, I = m/12 (w^2 + t^2) + m (w/2)^2, that is an
// angular momentum of 10 about it, hence an angular speed of 10 / I.
constexpr float kPushImpulse = 10.0f;
constexpr float kDoorInertia =
    kDoorMass / 12.0f * (4.0f * kDoorHalf.x * kDoorHalf.x + 4.0f * kDoorHalf.z * kDoorHalf.z) +
    kDoorMass * kDoorHalf.x * kDoorHalf.x;
constexpr float kPushSpin = kPushImpulse / kDoorInertia; // 1.496 rad/s

// Both worlds damp angular velocity at 5 % per second, so the angle turned
// in one second falls a little short of the initial rate.
constexpr float kDamping = 0.05f;
float angle_after(float spin, float seconds) {
    return spin * (1.0f - std::exp(-kDamping * seconds)) / kDamping;
}

} // namespace

TEST_CASE("a pendulum on a rod swings with the period its length sets, and the rod keeps its length") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            const Vec3 pivot{0.0f, 3.0f, 0.0f};
            const float rod = 1.0f;
            const float amplitude = 10.0f * kPi / 180.0f;
            const BodyHandle bob =
                add_bob(*world, pivot + Vec3{rod * std::sin(amplitude), -rod * std::cos(amplitude), 0.0f});
            REQUIRE(bob);

            JointDesc desc;
            desc.type = JointType::Distance;
            desc.a = bob;
            desc.anchor_b = pivot;
            desc.length = -1.0f; // whatever it is now: the rod
            const JointHandle joint = world->create_joint(desc);
            REQUIRE(joint);
            CHECK(world->valid(joint));
            CHECK(world->joint_count() == 1);
            CHECK(world->hinge_angle(joint) == 0.0f); // not a hinge

            // The bob swings from one side to the other: the time between the
            // turns of its x velocity is half the period.
            std::vector<float> turns;
            float previous_vx = 0.0f;
            float worst_rod = 0.0f;
            const int steps = static_cast<int>(6.0f / kStep);
            for (int i = 0; i < steps; ++i) {
                world->step(kStep);
                const BodyState s = world->body_state(bob);
                worst_rod = std::max(worst_rod, std::abs(length(s.position - pivot) - rod));
                const float vx = s.linear_velocity.x;
                if (previous_vx != 0.0f && vx != 0.0f && (previous_vx < 0.0f) != (vx < 0.0f)) {
                    turns.push_back(static_cast<float>(i + 1) * kStep);
                }
                previous_vx = vx;
            }
            CHECK(worst_rod < 0.02f);
            REQUIRE(turns.size() >= 4);
            const float half_period = (turns.back() - turns.front()) / static_cast<float>(turns.size() - 1);
            // T = 2 pi sqrt(L / g) (1 + amplitude^2 / 16 + ...)
            const float expected = kPi * std::sqrt(rod / kGravity) * (1.0f + amplitude * amplitude / 16.0f);
            MESSAGE(std::string(backend.name) << ": half period " << half_period << " s, expected "
                                              << expected << " s; rod length error " << worst_rod * 1000.0f
                                              << " mm");
            CHECK(half_period == doctest::Approx(expected).epsilon(0.03));
            // The whole swing stays in the plane it started in.
            CHECK(std::abs(world->body_state(bob).position.z) < 1e-3f);
        }
    }
}

TEST_CASE("a hanging chain keeps every link its length, however it swings") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            const Vec3 top{0.0f, 4.0f, 0.0f};
            const float link = 0.4f;
            constexpr int kLinks = 5;
            BodyHandle bobs[kLinks];
            for (int i = 0; i < kLinks; ++i) {
                bobs[i] = add_bob(*world, top - Vec3{0.0f, link * static_cast<float>(i + 1), 0.0f}, 0.5f);
                REQUIRE(bobs[i]);
                JointDesc desc;
                desc.type = JointType::Distance;
                desc.a = bobs[i];
                if (i == 0) {
                    desc.anchor_b = top;
                } else {
                    desc.b = bobs[i - 1];
                }
                desc.length = link;
                REQUIRE(world->create_joint(desc));
            }
            CHECK(world->joint_count() == kLinks);
            // Set it swinging: a sideways kick to the bottom bob.
            world->add_impulse(bobs[kLinks - 1], Vec3{1.0f, 0.0f, 0.0f});

            float worst = 0.0f;
            float fastest = 0.0f;
            const int steps = static_cast<int>(5.0f / kStep);
            for (int i = 0; i < steps; ++i) {
                world->step(kStep);
                Vec3 above = top;
                for (const BodyHandle bob : bobs) {
                    const BodyState s = world->body_state(bob);
                    worst = std::max(worst, std::abs(length(s.position - above) - link));
                    fastest = std::max(fastest, length(s.linear_velocity));
                    above = s.position;
                }
            }
            MESSAGE(std::string(backend.name) << ": worst link error " << worst * 1000.0f
                                              << " mm, fastest bob " << fastest << " m/s");
            CHECK(worst < 0.02f);
            // The kick gave the bottom bob 2 m/s; nothing gains energy from the joints.
            CHECK(fastest < 3.0f);
            // And the chain hangs below its anchor, never above it.
            for (const BodyHandle bob : bobs) {
                CHECK(world->body_state(bob).position.y < top.y);
            }
        }
    }
}

TEST_CASE("a door on a hinge turns about its pin and nothing else") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            const BodyHandle door = add_door(*world);
            REQUIRE(door);
            const JointHandle hinge = hinge_door(*world, door);
            REQUIRE(hinge);

            // Gravity runs along the axis: the pin carries the door and it hangs still.
            run(*world, 0.5f);
            CHECK(length(door_pin(*world, door) - kPin) < 0.005f);
            CHECK(std::abs(world->hinge_angle(hinge)) < 0.01f);
            CHECK(length(world->body_state(door).position - Vec3{0.5f, 1.0f, 0.0f}) < 0.005f);

            // A push at the free edge, towards -z: a torque about -y, so the
            // door turns right-handed about -y and the angle goes negative.
            world->add_impulse_at(door, Vec3{0.0f, 0.0f, kPushImpulse}, Vec3{1.0f, 1.0f, 0.0f});
            float previous = world->hinge_angle(hinge);
            float worst_pin = 0.0f;
            float least_upright = 1.0f;
            const int steps = static_cast<int>(1.0f / kStep);
            for (int i = 0; i < steps; ++i) {
                world->step(kStep);
                const float angle = world->hinge_angle(hinge);
                CHECK(angle <= previous + 1e-4f);
                previous = angle;
                worst_pin = std::max(worst_pin, length(door_pin(*world, door) - kPin));
                least_upright = std::min(least_upright, door_upright(*world, door));
            }
            const float expected = -angle_after(kPushSpin, 1.0f);
            MESSAGE(std::string(backend.name) << ": angle after 1 s " << previous << " rad, expected "
                                              << expected << "; pin drift " << worst_pin * 1000.0f << " mm");
            CHECK(previous == doctest::Approx(expected).epsilon(0.08));
            CHECK(worst_pin < 0.01f);
            CHECK(least_upright > 0.999f);
            // The free edge went where the angle says it did.
            const BodyState s = world->body_state(door);
            const Vec3 edge = s.position + s.rotation.rotate(Vec3{0.5f, 0.0f, 0.0f});
            CHECK(std::abs(edge.x - std::cos(previous)) < 0.02f);
            CHECK(std::abs(edge.z + std::sin(previous)) < 0.02f);
        }
    }
}

TEST_CASE("a limited hinge stops at each limit and stays there") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            const BodyHandle door = add_door(*world);
            REQUIRE(door);
            // Opens one way only, a quarter turn; closed is as far as it goes the other way.
            const JointHandle hinge = hinge_door(*world, door, true, -kPi / 2.0f, 0.0f);
            REQUIRE(hinge);

            world->add_impulse_at(door, Vec3{0.0f, 0.0f, kPushImpulse}, Vec3{1.0f, 1.0f, 0.0f});
            float lowest = 0.0f;
            const int steps = static_cast<int>(2.0f / kStep);
            for (int i = 0; i < steps; ++i) {
                world->step(kStep);
                lowest = std::min(lowest, world->hinge_angle(hinge));
            }
            // It swung out to the limit (at 1.5 rad/s it gets there in about a
            // second) and stopped. Jolt's door comes off the stop at 5 % of its
            // speed, its limit and anchor rows being solved one after the other;
            // ours are one block and it stays put.
            const float turning = length(world->body_state(door).angular_velocity);
            MESSAGE(std::string(backend.name) << ": stopped at " << world->hinge_angle(hinge)
                                              << " rad, turning at " << turning << " rad/s");
            CHECK(lowest > -kPi / 2.0f - 0.05f);
            CHECK(std::abs(world->hinge_angle(hinge) + kPi / 2.0f) < 0.1f);
            CHECK(length(world->body_state(door).angular_velocity) < 0.1f);
            CHECK(length(door_pin(*world, door) - kPin) < 0.01f);
            CHECK(door_upright(*world, door) > 0.999f);

            // Now it lies along +z; a push back at its free edge closes it, and
            // closed is the other limit.
            world->add_impulse_at(door, Vec3{kPushImpulse, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 1.0f});
            float highest = -kPi;
            for (int i = 0; i < steps; ++i) {
                world->step(kStep);
                highest = std::max(highest, world->hinge_angle(hinge));
            }
            CHECK(highest < 0.05f);
            CHECK(std::abs(world->hinge_angle(hinge)) < 0.1f);
            CHECK(length(world->body_state(door).angular_velocity) < 0.1f);
            CHECK(length(world->body_state(door).position - Vec3{0.5f, 1.0f, 0.0f}) < 0.06f);
        }
    }
}

TEST_CASE("two boxes on one hinge fall together and stay pinned") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            REQUIRE(add_floor(*world));
            BodyDesc box;
            box.shape = Shape::box(Vec3{0.25f});
            box.position = Vec3{-0.3f, 1.0f, 0.0f};
            const BodyHandle left = world->create_body(box);
            box.position = Vec3{0.3f, 1.0f, 0.0f};
            const BodyHandle right = world->create_body(box);
            REQUIRE(left);
            REQUIRE(right);

            // Pinned to each other across the gap between them, about z.
            JointDesc desc;
            desc.type = JointType::Hinge;
            desc.a = left;
            desc.b = right;
            desc.anchor_a = Vec3{0.3f, 0.0f, 0.0f};
            desc.anchor_b = Vec3{-0.3f, 0.0f, 0.0f};
            desc.axis_a = Vec3{0.0f, 0.0f, 1.0f};
            desc.axis_b = Vec3{0.0f, 0.0f, 1.0f};
            const JointHandle hinge = world->create_joint(desc);
            REQUIRE(hinge);

            const auto pin_gap = [&] {
                const BodyState a = world->body_state(left);
                const BodyState b = world->body_state(right);
                return length((a.position + a.rotation.rotate(desc.anchor_a)) -
                              (b.position + b.rotation.rotate(desc.anchor_b)));
            };
            const auto axes_aligned = [&] {
                const BodyState a = world->body_state(left);
                const BodyState b = world->body_state(right);
                return dot(a.rotation.rotate(desc.axis_a), b.rotation.rotate(desc.axis_b));
            };

            // Dropped flat, they land flat.
            run(*world, 2.0f);
            CHECK(world->body_state(left).position.y == doctest::Approx(0.25f).epsilon(0.05));
            CHECK(world->body_state(right).position.y == doctest::Approx(0.25f).epsilon(0.05));
            CHECK(pin_gap() < 0.01f);
            CHECK(std::abs(world->hinge_angle(hinge)) < 0.02f);

            // Flick the right box's far end up: it swings about the pin (the
            // right box turning +z relative to the left reads as a negative
            // angle, A relative to B), then drops back flat.
            world->add_impulse_at(right, Vec3{0.0f, 150.0f, 0.0f}, Vec3{0.55f, 0.25f, 0.0f});
            float lowest = 0.0f;
            float worst_gap = 0.0f;
            float least_aligned = 1.0f;
            const int steps = static_cast<int>(4.0f / kStep);
            for (int i = 0; i < steps; ++i) {
                world->step(kStep);
                lowest = std::min(lowest, world->hinge_angle(hinge));
                worst_gap = std::max(worst_gap, pin_gap());
                least_aligned = std::min(least_aligned, axes_aligned());
            }
            MESSAGE(std::string(backend.name) << ": swung to " << lowest << " rad, worst pin gap "
                                              << worst_gap * 1000.0f << " mm, axes aligned >= "
                                              << least_aligned);
            CHECK(lowest < -0.25f);
            CHECK(worst_gap < 0.01f);
            CHECK(least_aligned > 0.999f);
            const BodyState a = world->body_state(left);
            const BodyState b = world->body_state(right);
            CHECK(a.position.y == doctest::Approx(0.25f).epsilon(0.05));
            CHECK(b.position.y == doctest::Approx(0.25f).epsilon(0.05));
            CHECK(std::abs(world->hinge_angle(hinge)) < 0.05f);
            CHECK_FALSE(a.active);
            CHECK_FALSE(b.active);
        }
    }
}

TEST_CASE("joints go with their bodies, and bodies go free without their joints") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            const Vec3 pivot{0.0f, 3.0f, 0.0f};
            const BodyHandle bob = add_bob(*world, pivot - Vec3{0.0f, 1.0f, 0.0f});
            const BodyHandle other = add_bob(*world, pivot - Vec3{0.0f, 2.0f, 0.0f});
            REQUIRE(bob);
            REQUIRE(other);
            JointDesc desc;
            desc.type = JointType::Distance;
            desc.a = bob;
            desc.anchor_b = pivot;
            const JointHandle rod = world->create_joint(desc);
            desc.a = other;
            desc.b = bob;
            desc.anchor_b = Vec3{0.0f};
            const JointHandle link = world->create_joint(desc);
            REQUIRE(rod);
            REQUIRE(link);
            CHECK(world->joint_count() == 2);

            // Hanging still: nothing falls, and after a while everything sleeps.
            run(*world, 2.0f);
            CHECK(world->body_state(other).position.y == doctest::Approx(1.0f).epsilon(0.02));
            CHECK_FALSE(world->body_state(other).active);

            SUBCASE("destroying the joint lets the body fall, asleep or not") {
                CHECK(world->destroy_joint(link));
                CHECK_FALSE(world->valid(link));
                CHECK_FALSE(world->destroy_joint(link)); // stale now
                CHECK(world->joint_count() == 1);
                run(*world, 0.5f);
                // 1/2 g t^2 = 1.23 m, less a little damping.
                CHECK(world->body_state(other).position.y < 0.0f);
                CHECK(world->body_state(bob).position.y == doctest::Approx(2.0f).epsilon(0.02));
            }
            SUBCASE("destroying a body takes its joints with it") {
                CHECK(world->destroy_body(bob));
                CHECK_FALSE(world->valid(rod));
                CHECK_FALSE(world->valid(link));
                CHECK(world->joint_count() == 0);
                CHECK(world->body_count() == 1);
                run(*world, 0.5f);
                CHECK(world->body_state(other).position.y < 0.0f);
            }
            SUBCASE("a joint to a stale body is refused") {
                CHECK(world->destroy_body(other));
                desc.a = other;
                desc.b = bob;
                CHECK_FALSE(world->create_joint(desc));
                desc.a = bob;
                desc.b = other;
                CHECK_FALSE(world->create_joint(desc));
                CHECK(world->joint_count() == 1);
            }
        }
    }
}

TEST_CASE("a body with its rotation locked keeps its tilt whatever hits it") {
    for (const Backend& backend : kBackends) {
        SUBCASE(backend.name) {
            auto world = backend.make({});
            REQUIRE(add_floor(*world));
            BodyDesc box;
            box.shape = Shape::box(Vec3{0.5f, 0.2f, 0.5f});
            box.position = Vec3{0.0f, 1.5f, 0.0f};
            box.rotation = Quat::from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, 0.3f);
            box.lock_rotation = true;
            const BodyHandle locked = world->create_body(box);
            REQUIRE(locked);
            // A knock on one corner, which would spin a free body.
            world->add_impulse_at(locked, Vec3{0.0f, 0.0f, 5.0f}, Vec3{0.5f, 1.5f, 0.0f});

            run(*world, 2.0f);
            const BodyState s = world->body_state(locked);
            const Vec3 up = s.rotation.rotate(Vec3{0.0f, 1.0f, 0.0f});
            const Vec3 expected = box.rotation.rotate(Vec3{0.0f, 1.0f, 0.0f});
            CHECK(length(up - expected) < 1e-3f);
            CHECK(length(s.angular_velocity) < 1e-3f);
            // It landed on its lowest corner and stayed tilted, propped on nothing.
            CHECK(s.position.y > 0.2f);
            CHECK(s.position.y < 0.5f);
        }
    }
}
