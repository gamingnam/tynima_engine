#pragma once

#include <tynima/core/math/scalar.h>

#include <cmath>
#include <cstdint>
#include <cstring>

// Trigonometry that gives the same bits on every platform. The C library's
// sin, cos and atan2 are each correct to a rounding or so, but which
// rounding depends on the library, so a simulation using them diverges
// between macOS and Windows after a few thousand steps. These are polynomial
// approximations (Cephes', as Jolt uses them): plain IEEE adds, multiplies
// and one division, which are exact everywhere — as long as nothing fuses
// them (the build sets -ffp-contract=off) — and accurate to about one unit
// in the last place, which is all a game needs. Named sine, cosine, arctan
// so they never collide with <cmath>'s under a using-directive.
namespace tynima::math {

namespace detail {

[[nodiscard]] inline std::uint32_t float_bits(float f) noexcept {
    std::uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    return u;
}
[[nodiscard]] inline float float_from_bits(std::uint32_t u) noexcept {
    float f;
    std::memcpy(&f, &u, sizeof f);
    return f;
}

} // namespace detail

// sin and cos of x at once; accurate for |x| up to a few thousand radians,
// beyond which the range reduction runs out of bits (as libm's would not).
inline void sin_cos(float x, float& s, float& c) noexcept {
    // Work on |x|: cos is even, and sin gets its sign back at the end.
    const std::uint32_t sin_sign = detail::float_bits(x) & 0x80000000u;
    x = detail::float_from_bits(detail::float_bits(x) ^ sin_sign);
    // The quadrant nearest x, then x relative to it: x - q * pi/2 done in
    // three parts (Cody-Waite) so no significant bits are lost.
    const auto quadrant = static_cast<std::uint32_t>(0.6366197723675814f * x + 0.5f);
    const auto q = static_cast<float>(quadrant);
    x = ((x - q * 1.5703125f) - q * 0.0004837512969970703125f) - q * 7.549789948768648e-8f;
    const float x2 = x * x;
    // Taylor about 0, |x| <= pi/4: cos to x^8, sin to x^7.
    const float cos_x = ((2.443315711809948e-5f * x2 - 1.388731625493765e-3f) * x2 + 4.166664568298827e-2f) *
                            x2 * x2 -
                        0.5f * x2 + 1.0f;
    const float sin_x = ((-1.9515295891e-4f * x2 + 8.3321608736e-3f) * x2 - 1.6666654611e-1f) * x2 * x + x;
    // Quadrant 0: sin, cos; 1: cos, -sin; 2: -sin, -cos; 3: -cos, sin.
    const bool swap = (quadrant & 1u) != 0u;
    float s_out = swap ? cos_x : sin_x;
    float c_out = swap ? sin_x : cos_x;
    const std::uint32_t sin_flip = ((quadrant & 2u) != 0u) ? 0x80000000u : 0u;        // quadrants 2, 3
    const std::uint32_t cos_flip = (((quadrant + 1u) & 2u) != 0u) ? 0x80000000u : 0u; // quadrants 1, 2
    s_out = detail::float_from_bits(detail::float_bits(s_out) ^ sin_flip ^ sin_sign);
    c_out = detail::float_from_bits(detail::float_bits(c_out) ^ cos_flip);
    s = s_out;
    c = c_out;
}

[[nodiscard]] inline float sine(float x) noexcept {
    float s, c;
    sin_cos(x, s, c);
    return s;
}

[[nodiscard]] inline float cosine(float x) noexcept {
    float s, c;
    sin_cos(x, s, c);
    return c;
}

[[nodiscard]] inline float tangent(float x) noexcept {
    float s, c;
    sin_cos(x, s, c);
    return s / c;
}

// atan of x, in (-pi/2, pi/2).
[[nodiscard]] inline float arctan(float x) noexcept {
    const std::uint32_t sign = detail::float_bits(x) & 0x80000000u;
    x = detail::float_from_bits(detail::float_bits(x) ^ sign);
    float y = 0.0f;
    // Fold the argument below tan(pi/8) with the addition formula, in two steps.
    if (x > 2.414213562373095f) { // tan(3 pi / 8)
        y = kHalfPi;
        x = -1.0f / (x + 1.17549435e-38f); // + FLT_MIN: x is never quite zero here anyway
    } else if (x > 0.4142135623730950f) { // tan(pi / 8)
        y = 0.25f * kPi;
        x = (x - 1.0f) / (x + 1.0f);
    }
    const float z = x * x;
    float poly = 8.05374449538e-2f * z - 1.38776856032e-1f;
    poly = (poly * z + 1.99777106478e-1f) * z - 3.33329491539e-1f;
    y += poly * z * x + x;
    return detail::float_from_bits(detail::float_bits(y) ^ sign);
}

// atan2(y, x): the angle of (x, y) from the +x axis, in [-pi, pi], every
// quadrant right and no division by zero (arctan2(0, 0) is 0).
[[nodiscard]] inline float arctan2(float y, float x) noexcept {
    const std::uint32_t y_sign = detail::float_bits(y) & 0x80000000u;
    const std::uint32_t x_sign = detail::float_bits(x) & 0x80000000u;
    const float y_abs = detail::float_from_bits(detail::float_bits(y) ^ y_sign);
    const float x_abs = detail::float_from_bits(detail::float_bits(x) ^ x_sign);
    // Always the smaller over the larger, so the quotient is in [0, 1].
    const bool x_over_y = x_abs < y_abs;
    const float numerator = x_over_y ? x_abs : y_abs;
    const float denominator = x_over_y ? y_abs : x_abs;
    float angle = denominator > 0.0f ? arctan(numerator / denominator) : 0.0f;
    if (x_over_y) {
        angle = kHalfPi - angle;
    }
    // Into the right quadrant: x < 0 mirrors about the y axis, y < 0 about the x axis.
    if (x_sign != 0u) {
        angle = kPi - angle;
    }
    return detail::float_from_bits(detail::float_bits(angle) ^ y_sign);
}

// asin and acos through arctan2 and a square root (which is exact), for
// x in [-1, 1]; anything outside is clamped.
[[nodiscard]] inline float arcsin(float x) noexcept {
    x = clamp(x, -1.0f, 1.0f);
    return arctan2(x, std::sqrt(1.0f - x * x));
}

[[nodiscard]] inline float arccos(float x) noexcept {
    x = clamp(x, -1.0f, 1.0f);
    return arctan2(std::sqrt(1.0f - x * x), x);
}

} // namespace tynima::math
