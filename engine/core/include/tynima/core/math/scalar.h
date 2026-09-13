#pragma once

namespace tynima::math {

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kTwoPi = 2.0f * kPi;
inline constexpr float kHalfPi = 0.5f * kPi;

// Default tolerance for approx_equal(). Loose enough for a few chained float
// ops, tight enough to catch a wrong sign or a transposed matrix.
inline constexpr float kEpsilon = 1e-5f;

[[nodiscard]] constexpr float radians(float deg) noexcept {
    return deg * (kPi / 180.0f);
}
[[nodiscard]] constexpr float degrees(float rad) noexcept {
    return rad * (180.0f / kPi);
}

[[nodiscard]] constexpr float lerp(float a, float b, float t) noexcept {
    return a + (b - a) * t;
}
[[nodiscard]] constexpr float clamp(float v, float lo, float hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}
[[nodiscard]] constexpr float saturate(float v) noexcept {
    return clamp(v, 0.0f, 1.0f);
}

[[nodiscard]] constexpr bool approx_equal(float a, float b, float epsilon = kEpsilon) noexcept {
    const float d = a - b;
    return (d < 0.0f ? -d : d) <= epsilon;
}

} // namespace tynima::math
