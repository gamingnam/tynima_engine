#include <doctest/doctest.h>
#include <tynima/core/math.h>

using namespace tynima::math;

TEST_CASE("axis rotations follow the right-hand rule") {
    CHECK(approx_equal(transform_vector(rotation_y(radians(90.0f)), Vec3::unit_x()), -Vec3::unit_z()));
    CHECK(approx_equal(transform_vector(rotation_x(radians(90.0f)), Vec3::unit_y()), Vec3::unit_z()));
    CHECK(approx_equal(transform_vector(rotation_z(radians(90.0f)), Vec3::unit_x()), Vec3::unit_y()));
    CHECK(approx_equal(rotation_y(0.6f), rotation(Quat::from_axis_angle(Vec3::unit_y(), 0.6f))));
}

TEST_CASE("points translate, vectors do not") {
    const Mat4 t = translation({10.0f, 20.0f, 30.0f});
    CHECK(transform_point(t, Vec3{1.0f, 1.0f, 1.0f}) == Vec3{11.0f, 21.0f, 31.0f});
    CHECK(transform_vector(t, Vec3{1.0f, 1.0f, 1.0f}) == Vec3{1.0f, 1.0f, 1.0f});
    CHECK(t.translation() == Vec3{10.0f, 20.0f, 30.0f});
}

TEST_CASE("trs scales in local space, then rotates, then translates") {
    const Vec3 t{1.0f, 2.0f, 3.0f};
    const Quat r = Quat::from_axis_angle(Vec3::unit_z(), radians(90.0f));
    const Vec3 s{2.0f, 1.0f, 1.0f};
    const Mat4 m = trs(t, r, s);
    CHECK(approx_equal(m, translation(t) * rotation(r) * scaling(s)));
    // Local +x is stretched to length 2, then turned onto +y, then moved.
    CHECK(approx_equal(transform_point(m, Vec3::unit_x()), Vec3{1.0f, 4.0f, 3.0f}));
    CHECK(m.translation() == t);
}

TEST_CASE("look_at puts the camera at the origin looking down -z") {
    CHECK(approx_equal(look_at(Vec3::zero(), -Vec3::unit_z(), Vec3::unit_y()), Mat4::identity()));

    const Vec3 eye{0.0f, 0.0f, 5.0f};
    const Mat4 view = look_at(eye, Vec3::zero(), Vec3::unit_y());
    CHECK(approx_equal(transform_point(view, eye), Vec3::zero()));
    CHECK(approx_equal(transform_point(view, Vec3::zero()), Vec3{0.0f, 0.0f, -5.0f})); // in front: negative z
    CHECK(approx_equal(transform_point(view, Vec3{1.0f, 0.0f, 0.0f}), Vec3{1.0f, 0.0f, -5.0f})); // right stays right
    CHECK(approx_equal(transform_point(view, Vec3{0.0f, 1.0f, 0.0f}), Vec3{0.0f, 1.0f, -5.0f})); // up stays up
    CHECK(approx_equal(transform_point(inverse(view), Vec3::zero()), eye));

    // A camera off to the side, looking back at the origin, still sees it straight ahead.
    const Mat4 side = look_at({3.0f, 4.0f, 0.0f}, Vec3::zero(), Vec3::unit_y());
    const Vec3 v = transform_point(side, Vec3::zero());
    CHECK(v.x == doctest::Approx(0.0f));
    CHECK(v.y == doctest::Approx(0.0f));
    CHECK(v.z == doctest::Approx(-5.0f));
}

TEST_CASE("perspective maps the frustum to y-up NDC with depth 0..1") {
    const float fov = radians(60.0f);
    const float aspect = 16.0f / 9.0f;
    const float near = 0.1f;
    const float far = 100.0f;
    const Mat4 p = perspective(fov, aspect, near, far);

    CHECK(approx_equal(project_point(p, Vec3{0.0f, 0.0f, -near}), Vec3{0.0f, 0.0f, 0.0f}));
    CHECK(approx_equal(project_point(p, Vec3{0.0f, 0.0f, -far}), Vec3{0.0f, 0.0f, 1.0f}, 1e-4f));

    // The top edge of the near plane lands on y = +1; the right edge on x = +1.
    const float half_h = near * std::tan(0.5f * fov);
    CHECK(project_point(p, Vec3{0.0f, half_h, -near}).y == doctest::Approx(1.0f));
    CHECK(project_point(p, Vec3{half_h * aspect, 0.0f, -near}).x == doctest::Approx(1.0f));
    CHECK(project_point(p, Vec3{0.0f, -half_h, -near}).y == doctest::Approx(-1.0f));

    // Depth is monotonic: further away is a larger depth value.
    CHECK(project_point(p, Vec3{0.0f, 0.0f, -1.0f}).z < project_point(p, Vec3{0.0f, 0.0f, -10.0f}).z);

    // The projection is invertible: unproject NDC back to view space.
    const Vec3 v{0.7f, -0.2f, -3.0f};
    CHECK(approx_equal(project_point(inverse(p), project_point(p, v)), v, 1e-4f));
}

TEST_CASE("infinite reverse-z perspective puts 1 at the near plane and 0 at infinity") {
    const Mat4 p = perspective_infinite_reverse_z(radians(60.0f), 1.0f, 0.1f);
    CHECK(project_point(p, Vec3{0.0f, 0.0f, -0.1f}).z == doctest::Approx(1.0f));
    CHECK(project_point(p, Vec3{0.0f, 0.0f, -1000.0f}).z == doctest::Approx(0.0001f));
    CHECK(project_point(p, Vec3{0.0f, 0.0f, -1.0f}).z > project_point(p, Vec3{0.0f, 0.0f, -10.0f}).z);
    // x and y are the same as the finite projection.
    const Mat4 finite = perspective(radians(60.0f), 1.0f, 0.1f, 100.0f);
    const Vec3 v{0.3f, 0.2f, -2.0f};
    CHECK(project_point(p, v).xy() == project_point(finite, v).xy());
}

TEST_CASE("orthographic maps the box to NDC with depth 0..1") {
    const Mat4 o = orthographic(-2.0f, 2.0f, -1.0f, 1.0f, 0.5f, 10.0f);
    CHECK(approx_equal(project_point(o, Vec3{-2.0f, -1.0f, -0.5f}), Vec3{-1.0f, -1.0f, 0.0f}));
    CHECK(approx_equal(project_point(o, Vec3{2.0f, 1.0f, -10.0f}), Vec3{1.0f, 1.0f, 1.0f}));
    CHECK(approx_equal(project_point(o, Vec3{0.0f, 0.0f, -5.25f}), Vec3{0.0f, 0.0f, 0.5f}));
    // Off-centre boxes work too (a shadow map cascade is one).
    const Mat4 off = orthographic(10.0f, 20.0f, 30.0f, 40.0f, 1.0f, 2.0f);
    CHECK(approx_equal(project_point(off, Vec3{15.0f, 35.0f, -1.5f}), Vec3{0.0f, 0.0f, 0.5f}));
}

TEST_CASE("a full model-view-projection chain lands where expected") {
    const Mat4 model = trs({0.0f, 0.0f, -5.0f}, Quat::identity(), Vec3::one());
    const Mat4 view = look_at(Vec3::zero(), -Vec3::unit_z(), Vec3::unit_y());
    const Mat4 proj = perspective(radians(90.0f), 1.0f, 0.1f, 100.0f);
    const Mat4 mvp = proj * view * model;
    // The object's origin is dead centre; a point 5 units to its right, at a
    // 90° fov and 5 units away, lands exactly on the right edge of the screen.
    CHECK(approx_equal(project_point(mvp, Vec3::zero()).xy(), Vec2::zero()));
    CHECK(project_point(mvp, Vec3{5.0f, 0.0f, 0.0f}).x == doctest::Approx(1.0f));
    CHECK(project_point(mvp, Vec3{0.0f, 5.0f, 0.0f}).y == doctest::Approx(1.0f));
}
