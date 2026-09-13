#include <doctest/doctest.h>
#include <tynima/core/math.h>

using namespace tynima::math;

namespace {
const Vec3 kAxis = normalize(Vec3{1.0f, 2.0f, 3.0f});
}

TEST_CASE("identity leaves vectors alone") {
    const Vec3 v{1.0f, -2.0f, 3.5f};
    CHECK(Quat::identity().rotate(v) == v);
    CHECK(Quat::identity().to_mat3() == Mat3::identity());
    CHECK(Quat{} == Quat::identity());
}

TEST_CASE("axis-angle rotations follow the right-hand rule") {
    const Quat about_y = Quat::from_axis_angle(Vec3::unit_y(), radians(90.0f));
    CHECK(approx_equal(about_y.rotate(Vec3::unit_x()), -Vec3::unit_z()));
    CHECK(approx_equal(about_y.rotate(Vec3::unit_z()), Vec3::unit_x()));
    CHECK(approx_equal(about_y.rotate(Vec3::unit_y()), Vec3::unit_y())); // the axis is fixed

    const Quat about_z = Quat::from_axis_angle(Vec3::unit_z(), radians(90.0f));
    CHECK(approx_equal(about_z.rotate(Vec3::unit_x()), Vec3::unit_y()));

    const Quat about_x = Quat::from_axis_angle(Vec3::unit_x(), radians(90.0f));
    CHECK(approx_equal(about_x.rotate(Vec3::unit_y()), Vec3::unit_z()));

    CHECK(length(about_y) == doctest::Approx(1.0f));
}

TEST_CASE("the matrix form agrees with rotate()") {
    const Quat q = Quat::from_axis_angle(kAxis, 0.7f);
    const Vec3 v{0.3f, -1.2f, 2.0f};
    CHECK(approx_equal(q.to_mat3() * v, q.rotate(v)));
    CHECK(approx_equal(transform_vector(q.to_mat4(), v), q.rotate(v)));
    CHECK(approx_equal(rotation(q), rotation_y(0.0f) * q.to_mat4()));
    // A rotation matrix is orthonormal with determinant +1.
    CHECK(determinant(q.to_mat3()) == doctest::Approx(1.0f));
    CHECK(approx_equal(transpose(q.to_mat3()) * q.to_mat3(), Mat3::identity()));
}

TEST_CASE("composition order matches matrices: a * b applies b first") {
    const Quat a = Quat::from_axis_angle(Vec3::unit_y(), 0.8f);
    const Quat b = Quat::from_axis_angle(Vec3::unit_x(), -0.5f);
    const Vec3 v{1.0f, 2.0f, 3.0f};
    CHECK(approx_equal((a * b).rotate(v), a.rotate(b.rotate(v))));
    CHECK(approx_equal((a * b).to_mat3(), a.to_mat3() * b.to_mat3()));
    CHECK_FALSE(approx_equal((a * b).rotate(v), (b * a).rotate(v))); // rotations do not commute
}

TEST_CASE("inverse and conjugate undo a rotation") {
    const Quat q = Quat::from_axis_angle(kAxis, 1.9f);
    const Vec3 v{-4.0f, 0.5f, 2.0f};
    CHECK(approx_equal(q * conjugate(q), Quat::identity()));
    CHECK(approx_equal(q * inverse(q), Quat::identity()));
    CHECK(approx_equal(inverse(q).rotate(q.rotate(v)), v));
    // For a drifted (non-unit) quaternion, inverse() still inverts; conjugate() would not.
    const Quat drifted = q * 1.5f;
    CHECK(approx_equal(drifted * inverse(drifted), Quat::identity()));
    CHECK(approx_equal(normalize(drifted), q));
}

TEST_CASE("slerp is constant-speed along the shortest arc") {
    const Quat a = Quat::identity();
    const Quat b = Quat::from_axis_angle(Vec3::unit_y(), radians(90.0f));
    CHECK(approx_equal(slerp(a, b, 0.0f), a));
    CHECK(approx_equal(slerp(a, b, 1.0f), b));

    const Quat mid = slerp(a, b, 0.5f);
    const float c = std::cos(radians(45.0f));
    CHECK(approx_equal(mid.rotate(Vec3::unit_x()), Vec3{c, 0.0f, -c}));
    CHECK(length(mid) == doctest::Approx(1.0f));

    // q and -q are the same rotation; interpolating between them must not swing round the long way.
    CHECK(approx_equal(slerp(b, -b, 0.5f).rotate(Vec3::unit_x()), b.rotate(Vec3::unit_x())));
    // Nearly-parallel inputs take the nlerp path and stay unit length.
    const Quat almost = Quat::from_axis_angle(Vec3::unit_y(), 1e-4f);
    CHECK(length(slerp(a, almost, 0.5f)) == doctest::Approx(1.0f));

    CHECK(approx_equal(nlerp(a, b, 0.0f), a));
    CHECK(approx_equal(nlerp(a, b, 1.0f), b));
    CHECK(length(nlerp(a, b, 0.3f)) == doctest::Approx(1.0f));
}

TEST_CASE("from_to finds the rotation between two directions") {
    CHECK(approx_equal(Quat::from_to(Vec3::unit_x(), Vec3::unit_y()).rotate(Vec3::unit_x()), Vec3::unit_y()));
    const Vec3 to = normalize(Vec3{-1.0f, 0.5f, 2.0f});
    CHECK(approx_equal(Quat::from_to(Vec3::unit_z(), to).rotate(Vec3::unit_z()), to));
    CHECK(approx_equal(Quat::from_to(kAxis, kAxis), Quat::identity()));
    // Opposite directions: a half turn about some perpendicular axis.
    const Quat flip = Quat::from_to(Vec3::unit_x(), -Vec3::unit_x());
    CHECK(approx_equal(flip.rotate(Vec3::unit_x()), -Vec3::unit_x()));
    CHECK(length(flip) == doctest::Approx(1.0f));
}
