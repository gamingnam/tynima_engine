#include <doctest/doctest.h>
#include <tynima/core/math.h>

using namespace tynima::math;

static_assert(dot(Vec3::unit_x(), Vec3::unit_x()) == 1.0f, "vector math is constexpr");
static_assert(cross(Vec3::unit_x(), Vec3::unit_y()) == Vec3::unit_z(), "right-handed cross product");
static_assert(sizeof(Vec3) == 3 * sizeof(float), "Vec3 is exactly three floats");
static_assert(sizeof(Vec4) == 4 * sizeof(float), "Vec4 is exactly four floats");

TEST_CASE("vector arithmetic") {
    const Vec3 a{1.0f, 2.0f, 3.0f};
    const Vec3 b{4.0f, 5.0f, 6.0f};
    CHECK(a + b == Vec3{5.0f, 7.0f, 9.0f});
    CHECK(b - a == Vec3{3.0f, 3.0f, 3.0f});
    CHECK(a * 2.0f == Vec3{2.0f, 4.0f, 6.0f});
    CHECK(2.0f * a == a * 2.0f);
    CHECK(a * b == Vec3{4.0f, 10.0f, 18.0f}); // component-wise
    CHECK(b / 2.0f == Vec3{2.0f, 2.5f, 3.0f});
    CHECK(-a == Vec3{-1.0f, -2.0f, -3.0f});

    Vec3 c = a;
    c += b;
    c *= 0.5f;
    CHECK(c == Vec3{2.5f, 3.5f, 4.5f});

    CHECK(a[0] == 1.0f);
    CHECK(a[2] == 3.0f);
    CHECK(Vec4{a, 1.0f}.xyz() == a);
    CHECK(a.xy() == Vec2{1.0f, 2.0f});
}

TEST_CASE("dot and cross follow the right-hand rule") {
    CHECK(dot(Vec3{1.0f, 2.0f, 3.0f}, Vec3{4.0f, 5.0f, 6.0f}) == 32.0f);
    CHECK(dot(Vec3::unit_x(), Vec3::unit_y()) == 0.0f);

    CHECK(cross(Vec3::unit_x(), Vec3::unit_y()) == Vec3::unit_z());
    CHECK(cross(Vec3::unit_y(), Vec3::unit_z()) == Vec3::unit_x());
    CHECK(cross(Vec3::unit_z(), Vec3::unit_x()) == Vec3::unit_y());
    CHECK(cross(Vec3::unit_y(), Vec3::unit_x()) == -Vec3::unit_z()); // anticommutative
    CHECK(cross(Vec3::unit_x(), Vec3::unit_x()) == Vec3::zero());
}

TEST_CASE("length and normalize") {
    const Vec3 v{3.0f, 4.0f, 0.0f};
    CHECK(length_squared(v) == 25.0f);
    CHECK(length(v) == doctest::Approx(5.0f));
    CHECK(approx_equal(normalize(v), Vec3{0.6f, 0.8f, 0.0f}));
    CHECK(length(normalize(Vec3{1.0f, 2.0f, 3.0f})) == doctest::Approx(1.0f));
    CHECK(normalize(Vec3::zero()) == Vec3::zero()); // never NaN
    CHECK(distance(Vec3::zero(), v) == doctest::Approx(5.0f));
}

TEST_CASE("lerp, min, max, reflect") {
    const Vec3 a{0.0f, 0.0f, 0.0f};
    const Vec3 b{10.0f, 20.0f, 30.0f};
    CHECK(lerp(a, b, 0.0f) == a);
    CHECK(lerp(a, b, 1.0f) == b);
    CHECK(lerp(a, b, 0.5f) == Vec3{5.0f, 10.0f, 15.0f});
    CHECK(min(Vec3{1.0f, 5.0f, 3.0f}, Vec3{2.0f, 4.0f, 6.0f}) == Vec3{1.0f, 4.0f, 3.0f});
    CHECK(max(Vec3{1.0f, 5.0f, 3.0f}, Vec3{2.0f, 4.0f, 6.0f}) == Vec3{2.0f, 5.0f, 6.0f});
    // A ray coming down at 45 degrees bounces off the floor going up at 45.
    CHECK(approx_equal(reflect(Vec3{1.0f, -1.0f, 0.0f}, Vec3::unit_y()), Vec3{1.0f, 1.0f, 0.0f}));
}

TEST_CASE("scalar helpers") {
    CHECK(radians(180.0f) == doctest::Approx(kPi));
    CHECK(degrees(kHalfPi) == doctest::Approx(90.0f));
    CHECK(clamp(5.0f, 0.0f, 1.0f) == 1.0f);
    CHECK(clamp(-5.0f, 0.0f, 1.0f) == 0.0f);
    CHECK(saturate(0.25f) == 0.25f);
    CHECK(lerp(10.0f, 20.0f, 0.25f) == 12.5f);
    CHECK(approx_equal(1.0f, 1.0f + 1e-6f));
    CHECK_FALSE(approx_equal(1.0f, 1.001f));
    CHECK(approx_equal(Vec3{1.0f, 2.0f, 3.0f}, Vec3{1.0f, 2.0f, 3.0f + 1e-6f}));
}
