#include <doctest/doctest.h>
#include <tynima/core/math.h>

#include <cmath>
#include <cstring>

using namespace tynima::math;

namespace {

// The bit pattern, so "the same answer" means the same answer.
std::uint32_t bits(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    return u;
}

} // namespace

TEST_CASE("deterministic sin and cos match the C library to a rounding over many turns") {
    float worst = 0.0f;
    for (int i = -20000; i <= 20000; ++i) {
        const float x = static_cast<float>(i) * 0.001f; // -20..20 radians, past three turns each way
        float s, c;
        sin_cos(x, s, c);
        worst = std::max(worst, std::abs(s - std::sin(x)));
        worst = std::max(worst, std::abs(c - std::cos(x)));
        CHECK(sine(x) == s);
        CHECK(cosine(x) == c);
    }
    MESSAGE("worst sin/cos error " << worst);
    CHECK(worst < 4e-7f);
    // The identities hold exactly where they should.
    CHECK(sine(0.0f) == 0.0f);
    CHECK(cosine(0.0f) == 1.0f);
    CHECK(bits(sine(-0.0f)) == bits(-0.0f)); // the sign comes back
    CHECK(sine(-1.3f) == -sin(1.3f));
    CHECK(cosine(-1.3f) == cos(1.3f));
    CHECK(tangent(0.7f) == doctest::Approx(std::tan(0.7f)).epsilon(1e-6));
    // Quadrant boundaries, which the range reduction has to get right.
    CHECK(sine(kHalfPi) == doctest::Approx(1.0f));
    CHECK(cosine(kPi) == doctest::Approx(-1.0f));
    CHECK(sine(3.0f * kHalfPi) == doctest::Approx(-1.0f));
    CHECK(cosine(kTwoPi) == doctest::Approx(1.0f));
}

TEST_CASE("deterministic atan and atan2 cover every quadrant") {
    float worst = 0.0f;
    for (int i = -2000; i <= 2000; ++i) {
        const float x = static_cast<float>(i) * 0.05f; // -100..100
        worst = std::max(worst, std::abs(arctan(x) - std::atan(x)));
    }
    CHECK(worst < 4e-7f);
    CHECK(arctan(0.0f) == 0.0f);
    CHECK(arctan(-2.0f) == -arctan(2.0f));

    worst = 0.0f;
    for (int i = 0; i < 720; ++i) {
        const float angle = static_cast<float>(i) * (kTwoPi / 720.0f) - kPi;
        for (const float r : {0.001f, 1.0f, 1000.0f}) {
            const float x = r * std::cos(angle);
            const float y = r * std::sin(angle);
            float error = std::abs(arctan2(y, x) - std::atan2(y, x));
            if (error > kPi) {
                error = kTwoPi - error; // the two sides of the cut at +-pi
            }
            worst = std::max(worst, error);
        }
    }
    MESSAGE("worst atan2 error " << worst);
    CHECK(worst < 1e-6f);
    // The axes and the origin, where a division would be the wrong idea.
    CHECK(arctan2(0.0f, 1.0f) == 0.0f);
    CHECK(arctan2(1.0f, 0.0f) == doctest::Approx(kHalfPi));
    CHECK(arctan2(-1.0f, 0.0f) == doctest::Approx(-kHalfPi));
    CHECK(arctan2(0.0f, -1.0f) == doctest::Approx(kPi));
    CHECK(arctan2(-0.0f, -1.0f) == doctest::Approx(-kPi));
    CHECK(arctan2(0.0f, 0.0f) == 0.0f);

    for (int i = -100; i <= 100; ++i) {
        const float x = static_cast<float>(i) * 0.01f;
        CHECK(arcsin(x) == doctest::Approx(std::asin(x)).epsilon(2e-6));
        CHECK(arccos(x) == doctest::Approx(std::acos(x)).epsilon(2e-6));
    }
    CHECK(arcsin(1.0f) == doctest::Approx(kHalfPi));
    CHECK(arccos(-1.0f) == doctest::Approx(kPi));
    CHECK(arcsin(7.0f) == doctest::Approx(kHalfPi)); // clamped, not NaN
}

TEST_CASE("rotations built from angles are the same bits every time") {
    // Not a proof of cross-platform sameness — that needs another platform —
    // but the property everything else rests on: no hidden state, no
    // library call.
    const Quat a = Quat::from_axis_angle(normalize(Vec3{0.3f, 1.0f, 0.2f}), 0.37f);
    const Quat b = Quat::from_axis_angle(normalize(Vec3{0.3f, 1.0f, 0.2f}), 0.37f);
    CHECK(bits(a.x) == bits(b.x));
    CHECK(bits(a.w) == bits(b.w));
    CHECK(length(a) == doctest::Approx(1.0f));
    const Mat4 r = rotation_y(kHalfPi);
    CHECK(approx_equal(transform_point(r, Vec3{1, 0, 0}), Vec3{0, 0, -1}));
}
