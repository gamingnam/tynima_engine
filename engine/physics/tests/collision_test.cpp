#include <doctest/doctest.h>
#include <tynima/core/memory.h>
#include <tynima/physics/collision.h>
#include <tynima/physics/physics.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace tynima::physics;
using tynima::math::Aabb;
using tynima::math::kPi;
using tynima::math::Quat;
using tynima::math::Vec3;

namespace {

constexpr float kSpeculative = 0.02f;

Convex box_at(Vec3 half, Vec3 position, Quat rotation = Quat::identity()) {
    return Convex{Shape::box(half), Pose{position, rotation}};
}
Convex sphere_at(float radius, Vec3 position) {
    return Convex{Shape::sphere(radius), Pose{position, Quat::identity()}};
}
Convex capsule_at(float radius, float half_height, Vec3 position, Quat rotation = Quat::identity()) {
    return Convex{Shape::capsule(radius, half_height), Pose{position, rotation}};
}
// A 20 m slab whose top face is y = 0: the floor.
Convex floor_slab() {
    return box_at(Vec3{10.0f, 0.5f, 10.0f}, Vec3{0.0f, -0.5f, 0.0f});
}

bool near(const Vec3& a, const Vec3& b, float tolerance = 1e-4f) {
    const Vec3 d = a - b;
    return dot(d, d) <= tolerance * tolerance;
}
bool near(float a, float b, float tolerance = 1e-4f) {
    return std::fabs(a - b) <= tolerance;
}

// Points of the manifold on B, sorted by x then z, for checking corners.
std::vector<Vec3> sorted_points_on_b(const Manifold& m) {
    std::vector<Vec3> points;
    for (std::uint32_t i = 0; i < m.count; ++i) {
        points.push_back(m.points[i].on_b);
    }
    std::sort(points.begin(), points.end(), [](const Vec3& p, const Vec3& q) {
        return p.x != q.x ? p.x < q.x : p.z < q.z;
    });
    return points;
}

struct Random {
    std::mt19937 engine{777};
    float unit() { return static_cast<float>(engine() & 0xFFFFFF) / static_cast<float>(0x1000000); }
    float range(float lo, float hi) { return lo + (hi - lo) * unit(); }
    Vec3 vec(float lo, float hi) { return Vec3{range(lo, hi), range(lo, hi), range(lo, hi)}; }
    Quat rotation() { return Quat::from_axis_angle(normalize(vec(-1.0f, 1.0f)), range(0.0f, 2.0f * kPi)); }
};

} // namespace

TEST_CASE("support mappings: the farthest point of each core, in world space") {
    const Quat turn = Quat::from_axis_angle(Vec3::unit_z(), kPi * 0.5f); // x -> y
    const Convex box = box_at(Vec3{1.0f, 2.0f, 3.0f}, Vec3{10.0f, 0.0f, 0.0f}, turn);
    // Local +x (extent 1) now points along world +y; local +y (extent 2) along world -x.
    const Vec3 top = box.support(Vec3::unit_y());
    const bool top_is_a_corner = near(top, Vec3{8.0f, 1.0f, 3.0f}) || near(top, Vec3{12.0f, 1.0f, 3.0f});
    CHECK(top_is_a_corner);
    CHECK(near(box.support(Vec3{0.0f, 1.0f, 0.0f}).y, 1.0f));
    CHECK(near(box.support(Vec3{-1.0f, 0.0f, 0.0f}).x, 8.0f));
    CHECK(box.radius() == 0.0f);

    const Convex sphere = sphere_at(0.5f, Vec3{1.0f, 2.0f, 3.0f});
    CHECK(near(sphere.support(Vec3{0.0f, 0.0f, 1.0f}), Vec3{1.0f, 2.0f, 3.0f})); // the core is the centre
    CHECK(sphere.radius() == 0.5f);

    const Convex capsule = capsule_at(0.25f, 1.0f, Vec3{0.0f, 5.0f, 0.0f}, turn); // axis along world -x
    CHECK(near(capsule.support(Vec3::unit_x()), Vec3{1.0f, 5.0f, 0.0f}));
    CHECK(near(capsule.support(Vec3::unit_x() * -1.0f), Vec3{-1.0f, 5.0f, 0.0f}));
    CHECK(capsule.radius() == 0.25f);

    // Offset centres move the core.
    Convex offset = box_at(Vec3{0.5f}, Vec3{0.0f});
    offset.shape.center = Vec3{0.0f, 0.5f, 0.0f};
    CHECK(near(offset.support(Vec3::unit_y() * -1.0f).y, 0.0f));
    CHECK(near(offset.support(Vec3::unit_y()).y, 1.0f));

    // Bounds include the radius and the rotation.
    const Aabb sb = sphere.bounds();
    CHECK(near(sb.min, Vec3{0.5f, 1.5f, 2.5f}));
    const Aabb cb = capsule.bounds();
    CHECK(near(cb.min, Vec3{-1.25f, 4.75f, -0.25f}));
    CHECK(near(cb.max, Vec3{1.25f, 5.25f, 0.25f}));
    const Convex diamond =
        box_at(Vec3{1.0f, 1.0f, 1.0f}, Vec3{0.0f}, Quat::from_axis_angle(Vec3::unit_y(), kPi * 0.25f));
    CHECK(near(diamond.bounds().max.x, std::sqrt(2.0f), 1e-4f));
}

TEST_CASE("supporting faces: a box gives the face that faces the direction, a capsule its axis") {
    const Convex box = box_at(Vec3{1.0f, 2.0f, 3.0f}, Vec3{0.0f});
    Vec3 face[4];
    REQUIRE(box.supporting_face(Vec3{0.2f, -1.0f, 0.1f}, face) == 4);
    for (const Vec3& p : face) {
        CHECK(p.y == -2.0f); // the -y face
        CHECK(std::fabs(p.x) == 1.0f);
        CHECK(std::fabs(p.z) == 3.0f);
    }
    const Convex lying =
        capsule_at(0.2f, 1.0f, Vec3{0.0f}, Quat::from_axis_angle(Vec3::unit_z(), kPi * 0.5f));
    REQUIRE(lying.supporting_face(Vec3::unit_y() * -1.0f, face) == 2);
    const bool at_an_end = near(face[0].x, -1.0f) || near(face[0].x, 1.0f);
    CHECK(at_an_end);
    const Convex standing = capsule_at(0.2f, 1.0f, Vec3{0.0f});
    CHECK(standing.supporting_face(Vec3::unit_y() * -1.0f, face) == 0); // an end cap: a point contact
    CHECK(sphere_at(1.0f, Vec3{0.0f}).supporting_face(Vec3::unit_y(), face) == 0);
}

TEST_CASE("GJK: distances and closest points between cores") {
    SUBCASE("two spheres: the cores are points") {
        const GjkResult g = gjk(sphere_at(1.0f, Vec3{0.0f}), sphere_at(1.0f, Vec3{3.0f, 4.0f, 0.0f}));
        CHECK_FALSE(g.intersecting);
        CHECK(near(g.distance, 5.0f));
        CHECK(near(g.on_a, Vec3{0.0f}));
        CHECK(near(g.on_b, Vec3{3.0f, 4.0f, 0.0f}));
    }
    SUBCASE("two boxes, a gap along x") {
        const GjkResult g = gjk(box_at(Vec3{0.5f}, Vec3{0.0f}), box_at(Vec3{0.5f}, Vec3{1.5f, 0.2f, -0.1f}));
        CHECK_FALSE(g.intersecting);
        CHECK(near(g.distance, 0.5f));
        CHECK(near(g.on_a.x, 0.5f));
        CHECK(near(g.on_b.x, 1.0f));
    }
    SUBCASE("a box and a sphere: point to box") {
        const GjkResult g = gjk(box_at(Vec3{0.5f}, Vec3{0.0f}), sphere_at(0.5f, Vec3{2.0f, 0.3f, 0.2f}));
        CHECK_FALSE(g.intersecting);
        CHECK(near(g.distance, 1.5f));
        CHECK(near(g.on_a, Vec3{0.5f, 0.3f, 0.2f}));
        CHECK(near(g.on_b, Vec3{2.0f, 0.3f, 0.2f}));
    }
    SUBCASE("a sphere off a box's corner") {
        const GjkResult g = gjk(box_at(Vec3{0.5f}, Vec3{0.0f}), sphere_at(0.1f, Vec3{1.5f, 1.5f, 1.5f}));
        CHECK(near(g.distance, std::sqrt(3.0f), 1e-4f));
        CHECK(near(g.on_a, Vec3{0.5f}, 1e-4f));
    }
    SUBCASE("two capsules, parallel and crossed") {
        const GjkResult parallel =
            gjk(capsule_at(0.1f, 1.0f, Vec3{0.0f}), capsule_at(0.1f, 1.0f, Vec3{1.0f, 0.5f, 0.0f}));
        CHECK(near(parallel.distance, 1.0f));
        // B's axis along x, 2 m away along z: the segments cross at right angles.
        const Quat along_x = Quat::from_axis_angle(Vec3::unit_z(), kPi * 0.5f);
        const GjkResult crossed =
            gjk(capsule_at(0.1f, 1.0f, Vec3{0.0f}), capsule_at(0.1f, 1.0f, Vec3{0.3f, 0.0f, 2.0f}, along_x));
        CHECK(near(crossed.distance, 2.0f, 1e-4f));
        CHECK(near(crossed.on_a, Vec3{0.0f, 0.0f, 0.0f}, 1e-3f));
        CHECK(near(crossed.on_b, Vec3{0.0f, 0.0f, 2.0f}, 1e-3f));
    }
    SUBCASE("a rotated box's corner near a face") {
        const Quat diamond = Quat::from_axis_angle(Vec3::unit_y(), kPi * 0.25f);
        const float corner = 0.5f * std::sqrt(2.0f);
        const GjkResult g = gjk(box_at(Vec3{0.5f}, Vec3{0.0f}),
                                box_at(Vec3{0.5f}, Vec3{0.5f + corner + 0.1f, 0.0f, 0.0f}, diamond));
        CHECK_FALSE(g.intersecting);
        CHECK(near(g.distance, 0.1f, 1e-4f));
        CHECK(near(g.on_a.x, 0.5f, 1e-4f));
    }
    SUBCASE("overlapping cores are reported, not measured") {
        const Convex unit = box_at(Vec3{0.5f}, Vec3{0.0f});
        CHECK(gjk(unit, box_at(Vec3{0.5f}, Vec3{0.9f, 0.0f, 0.0f})).intersecting);
        CHECK(gjk(unit, sphere_at(0.2f, Vec3{0.4f, 0.0f, 0.0f})).intersecting);
        CHECK(gjk(unit, box_at(Vec3{0.5f}, Vec3{1.0f, 0.0f, 0.0f})).intersecting); // touching
        CHECK_FALSE(gjk(unit, sphere_at(0.2f, Vec3{0.6f, 0.0f, 0.0f})).intersecting);
    }
    SUBCASE("random pairs: the distance is symmetric and never negative") {
        Random random;
        for (int i = 0; i < 200; ++i) {
            const Convex a = box_at(random.vec(0.1f, 1.0f), random.vec(-2.0f, 2.0f), random.rotation());
            const Convex b = i % 2 == 0
                                 ? box_at(random.vec(0.1f, 1.0f), random.vec(-2.0f, 2.0f), random.rotation())
                                 : capsule_at(random.range(0.05f, 0.5f), random.range(0.1f, 1.0f),
                                              random.vec(-2.0f, 2.0f), random.rotation());
            const GjkResult ab = gjk(a, b);
            const GjkResult ba = gjk(b, a);
            CHECK(ab.intersecting == ba.intersecting);
            if (!ab.intersecting) {
                CHECK(ab.distance >= 0.0f);
                CHECK(near(ab.distance, ba.distance, 1e-3f));
                CHECK(near(ab.distance, length(ab.on_b - ab.on_a), 1e-3f));
            }
        }
    }
}

TEST_CASE("EPA: how deep and which way out") {
    SUBCASE("two boxes overlapping along x") {
        const Convex a = box_at(Vec3{0.5f}, Vec3{0.0f});
        const Convex b = box_at(Vec3{0.5f}, Vec3{0.9f, 0.0f, 0.0f});
        const GjkResult g = gjk(a, b);
        REQUIRE(g.intersecting);
        const EpaResult e = epa(a, b, g.simplex);
        REQUIRE(e.ok);
        CHECK(near(e.normal, Vec3::unit_x(), 1e-3f));
        CHECK(near(e.depth, 0.1f, 1e-4f));
        CHECK(near(e.on_a.x, 0.5f, 1e-3f));
        CHECK(near(e.on_b.x, 0.4f, 1e-3f));
    }
    SUBCASE("the shallowest axis wins") {
        const Convex a = box_at(Vec3{0.5f}, Vec3{0.0f});
        const Convex b = box_at(Vec3{0.5f}, Vec3{0.3f, 0.95f, 0.1f});
        const GjkResult g = gjk(a, b);
        REQUIRE(g.intersecting);
        const EpaResult e = epa(a, b, g.simplex);
        REQUIRE(e.ok);
        CHECK(near(e.normal, Vec3::unit_y(), 1e-3f));
        CHECK(near(e.depth, 0.05f, 1e-4f));
    }
    SUBCASE("a point inside a box") {
        const Convex a = box_at(Vec3{0.5f}, Vec3{0.0f});
        const Convex b = sphere_at(0.2f, Vec3{0.4f, 0.1f, -0.1f});
        const GjkResult g = gjk(a, b);
        REQUIRE(g.intersecting);
        const EpaResult e = epa(a, b, g.simplex);
        REQUIRE(e.ok);
        CHECK(near(e.normal, Vec3::unit_x(), 1e-3f));
        CHECK(near(e.depth, 0.1f, 1e-4f));
        CHECK(near(e.on_b, Vec3{0.4f, 0.1f, -0.1f}, 1e-3f));
        CHECK(near(e.on_a, Vec3{0.5f, 0.1f, -0.1f}, 1e-3f));
    }
    SUBCASE("a rotated corner pushed into a face") {
        const Quat diamond = Quat::from_axis_angle(Vec3::unit_y(), kPi * 0.25f);
        const float corner = 0.5f * std::sqrt(2.0f);
        const Convex a = box_at(Vec3{0.5f}, Vec3{0.0f});
        const Convex b = box_at(Vec3{0.5f}, Vec3{0.5f + corner - 0.05f, 0.0f, 0.0f}, diamond);
        const GjkResult g = gjk(a, b);
        REQUIRE(g.intersecting);
        const EpaResult e = epa(a, b, g.simplex);
        REQUIRE(e.ok);
        CHECK(near(e.normal, Vec3::unit_x(), 1e-3f));
        CHECK(near(e.depth, 0.05f, 1e-3f));
    }
    SUBCASE("exactly touching faces: depth zero, the face normal") {
        const Convex a = box_at(Vec3{0.5f}, Vec3{0.0f});
        const Convex b = box_at(Vec3{0.5f}, Vec3{0.0f, 1.0f, 0.0f});
        const GjkResult g = gjk(a, b);
        REQUIRE(g.intersecting);
        const EpaResult e = epa(a, b, g.simplex);
        REQUIRE(e.ok);
        CHECK(near(e.normal, Vec3::unit_y(), 1e-3f));
        CHECK(near(e.depth, 0.0f, 1e-4f));
    }
    SUBCASE("random overlaps: pushing B out by the answer separates the cores") {
        Random random;
        int checked = 0;
        for (int i = 0; i < 300 && checked < 100; ++i) {
            const Convex a = box_at(random.vec(0.2f, 1.0f), Vec3{0.0f}, random.rotation());
            Convex b = box_at(random.vec(0.2f, 1.0f), random.vec(-0.6f, 0.6f), random.rotation());
            const GjkResult g = gjk(a, b);
            if (!g.intersecting) {
                continue;
            }
            ++checked;
            const EpaResult e = epa(a, b, g.simplex);
            REQUIRE(e.ok);
            CHECK(near(length(e.normal), 1.0f, 1e-4f));
            CHECK(e.depth >= 0.0f);
            // Move B out along the normal by the depth (and a hair): apart, just.
            b.pose.position += e.normal * (e.depth + 1e-3f);
            const GjkResult after = gjk(a, b);
            CHECK_FALSE(after.intersecting);
            if (!after.intersecting) {
                CHECK(after.distance < 5e-3f);
            }
        }
        CHECK(checked == 100);
    }
}

TEST_CASE("collide: manifolds for the contacts a pile is made of") {
    const Convex floor = floor_slab();
    Manifold m;

    SUBCASE("a box resting on the floor: its four corners") {
        // A millimetre into the floor.
        const Convex box = box_at(Vec3{0.5f, 0.25f, 0.5f}, Vec3{1.0f, 0.25f - 0.001f, 2.0f});
        REQUIRE(collide(floor, box, kSpeculative, m));
        CHECK(near(m.normal, Vec3::unit_y(), 1e-4f));
        REQUIRE(m.count == 4);
        const std::vector<Vec3> corners = sorted_points_on_b(m);
        CHECK(near(corners[0], Vec3{0.5f, -0.001f, 1.5f}, 1e-3f));
        CHECK(near(corners[1], Vec3{0.5f, -0.001f, 2.5f}, 1e-3f));
        CHECK(near(corners[2], Vec3{1.5f, -0.001f, 1.5f}, 1e-3f));
        CHECK(near(corners[3], Vec3{1.5f, -0.001f, 2.5f}, 1e-3f));
        for (std::uint32_t i = 0; i < m.count; ++i) {
            CHECK(near(m.points[i].depth, 0.001f, 1e-4f));
            CHECK(near(m.points[i].on_a.y, 0.0f, 1e-4f)); // on the floor's top face
            CHECK(near(box.pose.to_world(m.points[i].local_b), m.points[i].on_b, 1e-4f));
            CHECK(m.points[i].normal_impulse == 0.0f);
        }
    }
    SUBCASE("hovering within the speculative distance: the same four points, negative depth") {
        const Convex box = box_at(Vec3{0.5f, 0.25f, 0.5f}, Vec3{0.0f, 0.25f + 0.005f, 0.0f});
        REQUIRE(collide(floor, box, kSpeculative, m));
        REQUIRE(m.count == 4);
        for (std::uint32_t i = 0; i < m.count; ++i) {
            CHECK(near(m.points[i].depth, -0.005f, 1e-4f));
        }
        const Convex higher = box_at(Vec3{0.5f, 0.25f, 0.5f}, Vec3{0.0f, 0.25f + 0.05f, 0.0f});
        CHECK_FALSE(collide(floor, higher, kSpeculative, m));
        CHECK(m.count == 0);
    }
    SUBCASE("a tilted box on its edge: two points") {
        const Quat tilt = Quat::from_axis_angle(Vec3::unit_z(), 0.5f);
        // The lowest edge sits hx * sin + hy * cos below the centre.
        const float drop = 0.5f * std::sin(0.5f) + 0.25f * std::cos(0.5f);
        const Convex box = box_at(Vec3{0.5f, 0.25f, 0.5f}, Vec3{0.0f, drop - 0.001f, 0.0f}, tilt);
        REQUIRE(collide(floor, box, kSpeculative, m));
        CHECK(near(m.normal, Vec3::unit_y(), 1e-3f));
        REQUIRE(m.count == 2);
        for (std::uint32_t i = 0; i < m.count; ++i) {
            CHECK(near(m.points[i].depth, 0.001f, 1e-3f));
            CHECK(near(std::fabs(m.points[i].on_b.z), 0.5f, 1e-3f)); // the edge runs along z
        }
    }
    SUBCASE("a sphere on the floor: one point, exact") {
        const Convex ball = sphere_at(0.3f, Vec3{2.0f, 0.3f - 0.002f, -1.0f});
        REQUIRE(collide(floor, ball, kSpeculative, m));
        REQUIRE(m.count == 1);
        CHECK(near(m.normal, Vec3::unit_y(), 1e-5f));
        CHECK(near(m.points[0].depth, 0.002f, 1e-5f));
        CHECK(near(m.points[0].on_b, Vec3{2.0f, -0.002f, -1.0f}, 1e-4f));
        CHECK(near(m.points[0].on_a, Vec3{2.0f, 0.0f, -1.0f}, 1e-4f));
    }
    SUBCASE("a capsule lying down: both ends; standing up: one") {
        const Quat lying = Quat::from_axis_angle(Vec3::unit_z(), kPi * 0.5f);
        const Convex pill = capsule_at(0.2f, 0.5f, Vec3{0.0f, 0.2f - 0.001f, 0.0f}, lying);
        REQUIRE(collide(floor, pill, kSpeculative, m));
        REQUIRE(m.count == 2);
        for (std::uint32_t i = 0; i < m.count; ++i) {
            CHECK(near(m.points[i].depth, 0.001f, 1e-4f));
            CHECK(near(std::fabs(m.points[i].on_b.x), 0.5f, 1e-3f));
            CHECK(near(m.points[i].on_b.y, -0.001f, 1e-4f));
        }
        const Convex standing = capsule_at(0.2f, 0.5f, Vec3{0.0f, 0.7f - 0.001f, 0.0f});
        REQUIRE(collide(floor, standing, kSpeculative, m));
        CHECK(m.count == 1);
        CHECK(near(m.points[0].depth, 0.001f, 1e-4f));
    }
    SUBCASE("a smaller box on a bigger one: the small one's corners") {
        const Convex big = box_at(Vec3{1.0f, 0.5f, 1.0f}, Vec3{0.0f, 0.5f, 0.0f});
        const Convex small = box_at(Vec3{0.2f, 0.2f, 0.2f}, Vec3{0.3f, 1.2f - 0.001f, -0.2f});
        REQUIRE(collide(big, small, kSpeculative, m));
        REQUIRE(m.count == 4);
        CHECK(near(m.normal, Vec3::unit_y(), 1e-4f));
        const std::vector<Vec3> corners = sorted_points_on_b(m);
        CHECK(near(corners[0], Vec3{0.1f, 0.999f, -0.4f}, 1e-3f));
        CHECK(near(corners[3], Vec3{0.5f, 0.999f, 0.0f}, 1e-3f));
    }
    SUBCASE("a box turned 45 degrees on another: an octagon cut down to four") {
        const Convex lower = box_at(Vec3{0.5f, 0.5f, 0.5f}, Vec3{0.0f, 0.5f, 0.0f});
        const Convex upper = box_at(Vec3{0.5f, 0.5f, 0.5f}, Vec3{0.0f, 1.5f - 0.001f, 0.0f},
                                    Quat::from_axis_angle(Vec3::unit_y(), kPi * 0.25f));
        REQUIRE(collide(lower, upper, kSpeculative, m));
        CHECK(m.count == 4);
        for (std::uint32_t i = 0; i < m.count; ++i) {
            CHECK(near(m.points[i].depth, 0.001f, 1e-3f));
            CHECK(std::fabs(m.points[i].on_b.x) <= 0.5f + 1e-3f); // inside the lower box's footprint
            CHECK(std::fabs(m.points[i].on_b.z) <= 0.5f + 1e-3f);
        }
    }
    SUBCASE("order matters only for the direction: swapping A and B flips the normal") {
        const Convex box = box_at(Vec3{0.5f, 0.25f, 0.5f}, Vec3{0.0f, 0.25f - 0.001f, 0.0f});
        Manifold ab, ba;
        REQUIRE(collide(floor, box, kSpeculative, ab));
        REQUIRE(collide(box, floor, kSpeculative, ba));
        CHECK(near(ab.normal, ba.normal * -1.0f, 1e-4f));
        CHECK(ab.count == ba.count);
        CHECK(near(ab.points[0].depth, ba.points[0].depth, 1e-4f));
    }
    SUBCASE("nothing allocates") {
        const Convex box = box_at(Vec3{0.5f, 0.25f, 0.5f}, Vec3{0.0f, 0.25f - 0.001f, 0.0f},
                                  Quat::from_axis_angle(Vec3::unit_y(), 0.3f));
        const tynima::core::HeapAllocationScope heap;
        bool hit = collide(floor, box, kSpeculative, m);
        hit = hit && collide(box, sphere_at(0.1f, Vec3{0.0f, 0.55f, 0.0f}), kSpeculative, m);
        const std::uint64_t allocations = heap.allocations();
        CHECK(allocations == 0);
        CHECK(hit);
    }
}

TEST_CASE("the manifold cache carries impulses across frames and forgets pairs that part") {
    ManifoldCache cache(8);
    const BodyHandle a{1, 1}, b{2, 1}, c{3, 1};
    const Convex floor = floor_slab();
    Convex box = box_at(Vec3{0.5f, 0.25f, 0.5f}, Vec3{0.0f, 0.25f - 0.001f, 0.0f});
    Manifold fresh;

    cache.begin_frame();
    REQUIRE(collide(floor, box, kSpeculative, fresh));
    Manifold* stored = cache.update(a, b, fresh);
    REQUIRE(stored != nullptr);
    CHECK(cache.count() == 1);
    stored->points[0].normal_impulse = 5.0f; // what a solver would leave behind
    stored->points[1].tangent_impulse[0] = -2.0f;
    const Vec3 first_local = stored->points[0].local_b;
    REQUIRE(collide(floor, sphere_at(0.1f, Vec3{3.0f, 0.099f, 0.0f}), kSpeculative, fresh));
    REQUIRE(cache.update(a, c, fresh) != nullptr);
    cache.end_frame();
    CHECK(cache.count() == 2);

    // Next frame the box slid 5 mm: the same corners, matched, keep their impulses.
    cache.begin_frame();
    box.pose.position.x += 0.005f;
    REQUIRE(collide(floor, box, kSpeculative, fresh));
    stored = cache.update(a, b, fresh);
    REQUIRE(stored != nullptr);
    float carried = 0.0f, carried_tangent = 0.0f;
    for (std::uint32_t i = 0; i < stored->count; ++i) {
        carried += stored->points[i].normal_impulse;
        carried_tangent += stored->points[i].tangent_impulse[0];
        if (near(stored->points[i].local_b, first_local, 1e-3f)) {
            CHECK(stored->points[i].normal_impulse == 5.0f);
        }
    }
    CHECK(carried == 5.0f);
    CHECK(carried_tangent == -2.0f);
    cache.end_frame(); // the sphere pair was not updated: gone
    CHECK(cache.count() == 1);
    CHECK(cache.find(a, c) == nullptr);
    CHECK(cache.find(a, b) != nullptr);

    // A jump of more than the match distance is a new point.
    cache.begin_frame();
    box.pose.position.x += 0.5f;
    REQUIRE(collide(floor, box, kSpeculative, fresh));
    stored = cache.update(a, b, fresh);
    REQUIRE(stored != nullptr);
    for (std::uint32_t i = 0; i < stored->count; ++i) {
        CHECK(stored->points[i].normal_impulse == 0.0f);
    }
    cache.end_frame();

    // Capacity is fixed; each() sees what is there.
    for (std::uint32_t i = 0; i < 10; ++i) {
        (void)cache.update(BodyHandle{10 + i, 1}, BodyHandle{20 + i, 1}, fresh);
    }
    CHECK(cache.count() == 8);
    CHECK(cache.update(BodyHandle{99, 1}, BodyHandle{98, 1}, fresh) == nullptr);
    int seen = 0;
    cache.each([&](BodyHandle, BodyHandle, Manifold& manifold) { seen += manifold.count > 0 ? 1 : 0; });
    CHECK(seen == 8);
}

// Runs a pile through Jolt, and for every contact Jolt reports asks our
// narrowphase the same question about the same two shapes at the same
// poses. Jolt detects contacts at the poses a step starts from, so those
// are recorded before each sampled step.
namespace {

struct PileBody {
    BodyHandle handle;
    Shape shape;
    Pose pose; // as of the start of the sampled step
};

struct Agreement {
    int contacts = 0;
    int found = 0;      // we also say they touch
    int normals = 0;    // ...and agree on the direction
    int depths = 0;     // ...and on how deep
    double depth_error = 0.0;
    double our_ms = 0.0;
};

Agreement compare_with_jolt(PhysicsWorld& world, std::vector<PileBody>& bodies, int frames,
                            float depth_tolerance, float normal_tolerance) {
    Agreement result;
    std::vector<PileBody*> by_index(4096, nullptr);
    for (PileBody& body : bodies) {
        by_index[body.handle.index] = &body;
    }
    for (int frame = 0; frame < frames; ++frame) {
        const bool sample = frame >= 10 && frame % 5 == 0; // let it fall a moment first
        if (sample) {
            for (PileBody& body : bodies) {
                const BodyState state = world.body_state(body.handle);
                body.pose = Pose{state.position, state.rotation};
            }
        }
        world.step(1.0f / 60.0f);
        if (!sample) {
            continue;
        }
        world.each_contact([&](const Contact& c) {
            const PileBody* pa = by_index[c.a.index];
            const PileBody* pb = by_index[c.b.index];
            if (pa == nullptr || pb == nullptr) {
                return;
            }
            const Convex a{pa->shape, pa->pose};
            const Convex b{pb->shape, pb->pose};
            Manifold m;
            const auto t0 = std::chrono::steady_clock::now();
            const bool hit = collide(a, b, kSpeculative + depth_tolerance, m);
            result.our_ms +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ++result.contacts;
            if (!hit) {
                return;
            }
            ++result.found;
            float deepest = m.points[0].depth;
            for (std::uint32_t i = 1; i < m.count; ++i) {
                deepest = std::max(deepest, m.points[i].depth);
            }
            if (dot(m.normal, c.normal) > normal_tolerance) {
                ++result.normals;
            }
            result.depth_error += std::fabs(static_cast<double>(deepest - c.depth));
            if (std::fabs(deepest - c.depth) <= depth_tolerance) {
                ++result.depths;
            }
        });
    }
    return result;
}

} // namespace

TEST_CASE("against Jolt: spheres and capsules, where both are exact") {
    auto world = create_jolt_world({});
    std::vector<PileBody> bodies;
    BodyDesc floor;
    floor.shape = Shape::box(Vec3{6.0f, 0.5f, 6.0f});
    floor.position = Vec3{0.0f, -0.5f, 0.0f};
    floor.motion = MotionType::Static;
    bodies.push_back({world->create_body(floor), floor.shape, Pose{}});
    Random random;
    for (int i = 0; i < 100; ++i) {
        BodyDesc body;
        body.shape = i % 2 == 0 ? Shape::sphere(random.range(0.05f, 0.15f))
                                : Shape::capsule(random.range(0.04f, 0.08f), random.range(0.05f, 0.15f));
        body.position = Vec3{random.range(-0.5f, 0.5f), 0.5f + static_cast<float>(i) * 0.12f,
                             random.range(-0.5f, 0.5f)};
        body.rotation = random.rotation();
        body.angular_velocity = random.vec(-3.0f, 3.0f);
        bodies.push_back({world->create_body(body), body.shape, Pose{}});
        REQUIRE(bodies.back().handle);
    }
    const Agreement a = compare_with_jolt(*world, bodies, 240, 0.002f, 0.99f);
    MESSAGE("spheres and capsules: " << a.contacts << " Jolt contacts; we found " << a.found
                                     << ", normals within 8 degrees " << a.normals << ", depths within 2 mm "
                                     << a.depths << ", mean depth error "
                                     << a.depth_error / std::max(a.found, 1) * 1000.0 << " mm, "
                                     << a.our_ms / std::max(a.contacts, 1) * 1000.0 << " us per pair");
    CHECK(a.contacts > 500);
    CHECK(a.found == a.contacts);
    CHECK(a.normals >= a.found * 99 / 100);
    CHECK(a.depths >= a.found * 99 / 100);
}

TEST_CASE("against Jolt: a pile of boxes, within the rounding of Jolt's edges") {
    // Jolt rounds box edges by a convex radius (up to 5 cm; here 2.75 cm);
    // ours are sharp. Face contacts agree exactly, edge and corner contacts
    // to within that rounding — and for a corner that is 2 cm off the sharp
    // one, so about one shallow box-on-box contact in fifteen has its closest
    // feature, and so its direction, elsewhere. The pile is chaotic; a change
    // of one bit anywhere upstream reshuffles which contacts those are.
    auto world = create_jolt_world({});
    std::vector<PileBody> bodies;
    BodyDesc floor;
    floor.shape = Shape::box(Vec3{6.0f, 0.5f, 6.0f});
    floor.position = Vec3{0.0f, -0.5f, 0.0f};
    floor.motion = MotionType::Static;
    bodies.push_back({world->create_body(floor), floor.shape, Pose{}});
    Random random;
    for (int i = 0; i < 100; ++i) {
        BodyDesc body;
        body.shape = Shape::box(Vec3{0.055f, 0.13f, 0.055f});
        body.position = Vec3{random.range(-0.4f, 0.4f), 0.6f + static_cast<float>(i) * 0.12f,
                             random.range(-0.4f, 0.4f)};
        body.rotation = random.rotation();
        body.angular_velocity = random.vec(-2.0f, 2.0f);
        body.mass = 0.6f;
        bodies.push_back({world->create_body(body), body.shape, Pose{}});
        REQUIRE(bodies.back().handle);
    }
    const Agreement a = compare_with_jolt(*world, bodies, 300, 0.03f, 0.9f);
    MESSAGE("boxes: " << a.contacts << " Jolt contacts; we found " << a.found
                      << ", normals within 26 degrees " << a.normals << ", depths within 3 cm " << a.depths
                      << ", mean depth error " << a.depth_error / std::max(a.found, 1) * 1000.0 << " mm, "
                      << a.our_ms / std::max(a.contacts, 1) * 1000.0 << " us per pair");
    CHECK(a.contacts > 1000);
    CHECK(a.found >= a.contacts * 99 / 100);
    CHECK(a.normals >= a.found * 90 / 100);
    CHECK(a.depths >= a.found * 95 / 100);
}
