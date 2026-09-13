#pragma once

#include <tynima/core/math/vec.h>

#include <cstddef>

namespace tynima::math {

// Column-major matrices acting on column vectors: M * v, and A * B applies B
// first. Storage is cols[c][r] — the layout Metal, HLSL and glTF all expect —
// while m(row, col) reads in textbook order. from_rows() exists so a matrix
// can be written down the way it is printed.

struct Mat3 {
    Vec3 cols[3]{};

    constexpr Mat3() noexcept = default;
    constexpr Mat3(const Vec3& c0, const Vec3& c1, const Vec3& c2) noexcept : cols{c0, c1, c2} {}

    [[nodiscard]] static constexpr Mat3 identity() noexcept {
        return {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    }
    [[nodiscard]] static constexpr Mat3 from_rows(const Vec3& r0, const Vec3& r1, const Vec3& r2) noexcept {
        return {{r0.x, r1.x, r2.x}, {r0.y, r1.y, r2.y}, {r0.z, r1.z, r2.z}};
    }
    [[nodiscard]] static constexpr Mat3 scaling(const Vec3& s) noexcept {
        return {{s.x, 0.0f, 0.0f}, {0.0f, s.y, 0.0f}, {0.0f, 0.0f, s.z}};
    }

    constexpr float& operator()(std::size_t row, std::size_t col) noexcept { return cols[col][row]; }
    constexpr float operator()(std::size_t row, std::size_t col) const noexcept { return cols[col][row]; }
    constexpr Vec3& operator[](std::size_t col) noexcept { return cols[col]; }
    constexpr const Vec3& operator[](std::size_t col) const noexcept { return cols[col]; }

    [[nodiscard]] constexpr Vec3 row(std::size_t r) const noexcept { return {cols[0][r], cols[1][r], cols[2][r]}; }
};

struct Mat4 {
    Vec4 cols[4]{};

    constexpr Mat4() noexcept = default;
    constexpr Mat4(const Vec4& c0, const Vec4& c1, const Vec4& c2, const Vec4& c3) noexcept : cols{c0, c1, c2, c3} {}

    [[nodiscard]] static constexpr Mat4 identity() noexcept {
        return {{1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
    }
    [[nodiscard]] static constexpr Mat4 from_rows(const Vec4& r0, const Vec4& r1, const Vec4& r2, const Vec4& r3) noexcept {
        return {{r0.x, r1.x, r2.x, r3.x}, {r0.y, r1.y, r2.y, r3.y}, {r0.z, r1.z, r2.z, r3.z}, {r0.w, r1.w, r2.w, r3.w}};
    }
    // Upper-left 3x3 in a 4x4 with no translation.
    [[nodiscard]] static constexpr Mat4 from_mat3(const Mat3& m) noexcept {
        return {{m.cols[0], 0.0f}, {m.cols[1], 0.0f}, {m.cols[2], 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
    }

    constexpr float& operator()(std::size_t row, std::size_t col) noexcept { return cols[col][row]; }
    constexpr float operator()(std::size_t row, std::size_t col) const noexcept { return cols[col][row]; }
    constexpr Vec4& operator[](std::size_t col) noexcept { return cols[col]; }
    constexpr const Vec4& operator[](std::size_t col) const noexcept { return cols[col]; }

    [[nodiscard]] constexpr Vec4 row(std::size_t r) const noexcept {
        return {cols[0][r], cols[1][r], cols[2][r], cols[3][r]};
    }
    // The rotation/scale block, for normal matrices and for stripping translation.
    [[nodiscard]] constexpr Mat3 upper3x3() const noexcept {
        return {cols[0].xyz(), cols[1].xyz(), cols[2].xyz()};
    }
    [[nodiscard]] constexpr Vec3 translation() const noexcept { return cols[3].xyz(); }
};

// ---------------------------------------------------------------- operators

[[nodiscard]] constexpr Vec3 operator*(const Mat3& m, const Vec3& v) noexcept {
    return m.cols[0] * v.x + m.cols[1] * v.y + m.cols[2] * v.z;
}
[[nodiscard]] constexpr Mat3 operator*(const Mat3& a, const Mat3& b) noexcept {
    return {a * b.cols[0], a * b.cols[1], a * b.cols[2]};
}
[[nodiscard]] constexpr Mat3 operator*(const Mat3& m, float s) noexcept {
    return {m.cols[0] * s, m.cols[1] * s, m.cols[2] * s};
}
[[nodiscard]] constexpr bool operator==(const Mat3& a, const Mat3& b) noexcept {
    return a.cols[0] == b.cols[0] && a.cols[1] == b.cols[1] && a.cols[2] == b.cols[2];
}
[[nodiscard]] constexpr bool operator!=(const Mat3& a, const Mat3& b) noexcept { return !(a == b); }

[[nodiscard]] constexpr Vec4 operator*(const Mat4& m, const Vec4& v) noexcept {
    return m.cols[0] * v.x + m.cols[1] * v.y + m.cols[2] * v.z + m.cols[3] * v.w;
}
[[nodiscard]] constexpr Mat4 operator*(const Mat4& a, const Mat4& b) noexcept {
    return {a * b.cols[0], a * b.cols[1], a * b.cols[2], a * b.cols[3]};
}
[[nodiscard]] constexpr Mat4 operator*(const Mat4& m, float s) noexcept {
    return {m.cols[0] * s, m.cols[1] * s, m.cols[2] * s, m.cols[3] * s};
}
[[nodiscard]] constexpr bool operator==(const Mat4& a, const Mat4& b) noexcept {
    return a.cols[0] == b.cols[0] && a.cols[1] == b.cols[1] && a.cols[2] == b.cols[2] && a.cols[3] == b.cols[3];
}
[[nodiscard]] constexpr bool operator!=(const Mat4& a, const Mat4& b) noexcept { return !(a == b); }

// ---------------------------------------------------------------- functions

[[nodiscard]] constexpr Mat3 transpose(const Mat3& m) noexcept {
    return {m.row(0), m.row(1), m.row(2)};
}
[[nodiscard]] constexpr Mat4 transpose(const Mat4& m) noexcept {
    return {m.row(0), m.row(1), m.row(2), m.row(3)};
}

// Scalar triple product of the columns.
[[nodiscard]] constexpr float determinant(const Mat3& m) noexcept {
    return dot(m.cols[0], cross(m.cols[1], m.cols[2]));
}

// The 3x3 left after removing one row and one column.
[[nodiscard]] constexpr Mat3 minor(const Mat4& m, std::size_t row, std::size_t col) noexcept {
    Mat3 out;
    std::size_t oc = 0;
    for (std::size_t c = 0; c < 4; ++c) {
        if (c == col) {
            continue;
        }
        std::size_t orow = 0;
        for (std::size_t r = 0; r < 4; ++r) {
            if (r == row) {
                continue;
            }
            out(orow, oc) = m(r, c);
            ++orow;
        }
        ++oc;
    }
    return out;
}

[[nodiscard]] constexpr float cofactor(const Mat4& m, std::size_t row, std::size_t col) noexcept {
    const float sign = ((row + col) % 2 == 0) ? 1.0f : -1.0f;
    return sign * determinant(minor(m, row, col));
}

// Laplace expansion along the first row.
[[nodiscard]] constexpr float determinant(const Mat4& m) noexcept {
    return m(0, 0) * cofactor(m, 0, 0) + m(0, 1) * cofactor(m, 0, 1) + m(0, 2) * cofactor(m, 0, 2) +
           m(0, 3) * cofactor(m, 0, 3);
}

// Adjugate over determinant. A singular matrix has no inverse; this returns
// the identity for it rather than a matrix of infinities, so a degenerate
// scale somewhere does not poison the frame. Check determinant() first when
// singularity is a real possibility.
[[nodiscard]] constexpr Mat3 inverse(const Mat3& m) noexcept {
    const Vec3 c0 = cross(m.cols[1], m.cols[2]);
    const Vec3 c1 = cross(m.cols[2], m.cols[0]);
    const Vec3 c2 = cross(m.cols[0], m.cols[1]);
    const float det = dot(m.cols[0], c0);
    if (approx_equal(det, 0.0f, 1e-12f)) {
        return Mat3::identity();
    }
    // Rows of the inverse are the cross products above, scaled by 1/det.
    return transpose(Mat3{c0, c1, c2}) * (1.0f / det);
}

[[nodiscard]] constexpr Mat4 inverse(const Mat4& m) noexcept {
    Mat4 cof;
    for (std::size_t r = 0; r < 4; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            cof(r, c) = cofactor(m, r, c);
        }
    }
    const float det = m(0, 0) * cof(0, 0) + m(0, 1) * cof(0, 1) + m(0, 2) * cof(0, 2) + m(0, 3) * cof(0, 3);
    if (approx_equal(det, 0.0f, 1e-12f)) {
        return Mat4::identity();
    }
    return transpose(cof) * (1.0f / det);
}

[[nodiscard]] constexpr bool approx_equal(const Mat3& a, const Mat3& b, float epsilon = kEpsilon) noexcept {
    return approx_equal(a.cols[0], b.cols[0], epsilon) && approx_equal(a.cols[1], b.cols[1], epsilon) &&
           approx_equal(a.cols[2], b.cols[2], epsilon);
}
[[nodiscard]] constexpr bool approx_equal(const Mat4& a, const Mat4& b, float epsilon = kEpsilon) noexcept {
    return approx_equal(a.cols[0], b.cols[0], epsilon) && approx_equal(a.cols[1], b.cols[1], epsilon) &&
           approx_equal(a.cols[2], b.cols[2], epsilon) && approx_equal(a.cols[3], b.cols[3], epsilon);
}

} // namespace tynima::math
