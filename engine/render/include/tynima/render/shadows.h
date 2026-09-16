#pragma once

#include <tynima/core/math.h>
#include <tynima/render/camera.h>

#include <cstdint>

// Cascaded shadow maps for one directional light: the view frustum is cut
// into slices along the view direction, and each slice gets its own shadow
// map fitted just around it, so the texels near the camera are small and the
// ones far away are large — resolution spent where it shows.
namespace tynima::render {

inline constexpr std::uint32_t kMaxCascades = 4;

struct CascadeSettings {
    std::uint32_t count = 4;
    std::uint32_t map_size = 2048;  // texels per side, each cascade
    float max_distance = 60.0f;     // metres from the camera the last cascade reaches
    float split_blend = 0.7f;       // 0: uniform slices, 1: logarithmic (the "practical split scheme")
    float caster_margin = 30.0f;    // metres towards the light, past a slice, from which casters still count
};

// One cascade: an orthographic light-space projection, reverse-Z like the
// camera's (depth 1 nearest the light, 0 farthest) so the same GREATER test
// and the same clear serve both.
struct Cascade {
    math::Mat4 view_projection; // world -> light clip
    float split_far = 0.0f;     // view-space distance where the slice ends
    float texel_size = 0.0f;    // metres per shadow-map texel
    float depth_range = 0.0f;   // metres between the light's near and far planes: one unit of depth
};

struct CascadeSet {
    Cascade cascades[kMaxCascades];
    std::uint32_t count = 0;
};

// Fits `settings.count` cascades to the camera's frustum out to
// `settings.max_distance`. `light_direction` points towards the light. Each
// cascade's box is fitted around the bounding sphere of its slice rather than
// the slice itself, so it keeps its size as the camera turns, and its
// position is snapped to the shadow map's texel grid, so edges do not
// shimmer as the camera moves.
[[nodiscard]] CascadeSet fit_cascades(const Camera& camera, float aspect, const math::Vec3& light_direction,
                                      const CascadeSettings& settings) noexcept;

} // namespace tynima::render
