#include <doctest/doctest.h>
#include <tynima/core/math.h>

using namespace tynima::math;

static_assert(determinant(Mat4::identity()) == 1.0f, "matrix math is constexpr");
static_assert(sizeof(Mat4) == 16 * sizeof(float), "Mat4 is sixteen floats, column-major");

TEST_CASE("storage is column-major and from_rows reads like the textbook") {
    const Mat4 m = Mat4::from_rows({1.0f, 2.0f, 3.0f, 4.0f}, {5.0f, 6.0f, 7.0f, 8.0f}, {9.0f, 10.0f, 11.0f, 12.0f},
                                   {13.0f, 14.0f, 15.0f, 16.0f});
    CHECK(m(0, 1) == 2.0f);  // row 0, column 1
    CHECK(m(1, 0) == 5.0f);  // row 1, column 0
    CHECK(m.cols[1][0] == 2.0f); // column 1 holds the second entry of every row
    CHECK(m.cols[0] == Vec4{1.0f, 5.0f, 9.0f, 13.0f});
    CHECK(m.row(2) == Vec4{9.0f, 10.0f, 11.0f, 12.0f});
    CHECK(m.translation() == Vec3{4.0f, 8.0f, 12.0f});

    // The memory a GPU sees is the columns back to back.
    const float* flat = &m.cols[0].x;
    CHECK(flat[0] == 1.0f);
    CHECK(flat[1] == 5.0f);
    CHECK(flat[4] == 2.0f);
}

TEST_CASE("identity, transpose, multiplication") {
    const Mat4 m = Mat4::from_rows({1.0f, 2.0f, 3.0f, 4.0f}, {5.0f, 6.0f, 7.0f, 8.0f}, {9.0f, 10.0f, 11.0f, 12.0f},
                                   {13.0f, 14.0f, 15.0f, 16.0f});
    CHECK(Mat4::identity() * m == m);
    CHECK(m * Mat4::identity() == m);
    CHECK(transpose(transpose(m)) == m);
    CHECK(transpose(m)(1, 0) == m(0, 1));

    const Vec4 v{1.0f, 0.0f, 0.0f, 0.0f};
    CHECK(m * v == m.cols[0]); // M * e0 picks out the first column

    // Translate-then-scale is not scale-then-translate.
    const Mat4 t = translation({1.0f, 0.0f, 0.0f});
    const Mat4 s = scaling({2.0f, 2.0f, 2.0f});
    CHECK(transform_point(s * t, Vec3::zero()) == Vec3{2.0f, 0.0f, 0.0f}); // translate first, then scale
    CHECK(transform_point(t * s, Vec3::zero()) == Vec3{1.0f, 0.0f, 0.0f}); // scale first, then translate
    CHECK(s * t != t * s);
}

TEST_CASE("determinant") {
    CHECK(determinant(Mat3::identity()) == 1.0f);
    CHECK(determinant(Mat3::scaling({2.0f, 3.0f, 4.0f})) == 24.0f);
    CHECK(determinant(scaling({2.0f, 3.0f, 4.0f})) == 24.0f);
    CHECK(determinant(translation({5.0f, 6.0f, 7.0f})) == 1.0f);
    // A reflection has a negative determinant.
    CHECK(determinant(scaling({-1.0f, 1.0f, 1.0f})) == -1.0f);
    // Two equal rows: singular.
    CHECK(determinant(Mat4::from_rows({1.0f, 2.0f, 3.0f, 4.0f}, {1.0f, 2.0f, 3.0f, 4.0f}, {0.0f, 1.0f, 0.0f, 0.0f},
                                      {0.0f, 0.0f, 0.0f, 1.0f})) == 0.0f);
}

TEST_CASE("inverse") {
    CHECK(inverse(Mat4::identity()) == Mat4::identity());
    CHECK(approx_equal(inverse(translation({1.0f, 2.0f, 3.0f})), translation({-1.0f, -2.0f, -3.0f})));
    CHECK(approx_equal(inverse(scaling({2.0f, 4.0f, 8.0f})), scaling({0.5f, 0.25f, 0.125f})));

    const Mat4 m = trs({1.0f, -2.0f, 3.0f}, Quat::from_axis_angle(normalize(Vec3{1.0f, 1.0f, 0.0f}), 0.9f),
                       {2.0f, 3.0f, 0.5f});
    CHECK(approx_equal(m * inverse(m), Mat4::identity()));
    CHECK(approx_equal(inverse(m) * m, Mat4::identity()));

    const Mat3 r = Quat::from_axis_angle(Vec3::unit_z(), 0.4f).to_mat3() * Mat3::scaling({2.0f, 1.0f, 3.0f});
    CHECK(approx_equal(r * inverse(r), Mat3::identity()));
    // A rotation's inverse is its transpose.
    const Mat3 rot = Quat::from_axis_angle(Vec3::unit_y(), 1.1f).to_mat3();
    CHECK(approx_equal(inverse(rot), transpose(rot)));

    // Singular: no inverse exists; we hand back the identity rather than infinities.
    CHECK(inverse(scaling({1.0f, 0.0f, 1.0f})) == Mat4::identity());
    CHECK(inverse(Mat3::scaling({0.0f, 1.0f, 1.0f})) == Mat3::identity());
}

TEST_CASE("upper3x3 and from_mat3 round-trip the rotation block") {
    const Mat4 m = trs({4.0f, 5.0f, 6.0f}, Quat::from_axis_angle(Vec3::unit_x(), 0.3f), Vec3::one());
    const Mat3 u = m.upper3x3();
    // Undo the translation on the left (world side); on the right it would
    // apply in local space, before the rotation, and not cancel.
    CHECK(approx_equal(Mat4::from_mat3(u), translation({-4.0f, -5.0f, -6.0f}) * m));
    CHECK_FALSE(approx_equal(Mat4::from_mat3(u), m * translation({-4.0f, -5.0f, -6.0f})));
}
