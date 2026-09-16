#pragma once

#include <tynima/core/math.h>
#include <tynima/rhi/device.h>

#include <cstdint>

// Clustered lighting: the view frustum is cut into a grid of screen tiles
// by depth slices (froxels), a compute pass lists which point lights reach
// each cell, and a pixel shades only the lights in its own cell — so a
// hundred lights cost what the handful near each pixel cost.
namespace tynima::render {

inline constexpr std::uint32_t kMaxLightsPerCluster = 31; // with the count, 32 words: a 128-byte cell

// One light, as the GPU sees it: where, how far it reaches, and its
// radiance. Beyond `radius` the light contributes nothing.
struct PointLight {
    math::Vec4 position_radius; // world position, radius (m)
    math::Vec4 color;           // linear radiance, unused w
};
static_assert(sizeof(PointLight) == 32, "matches the shaders' PointLight");

// One cell's light list.
struct ClusterLights {
    std::uint32_t count = 0;
    std::uint32_t indices[kMaxLightsPerCluster]{};
};
static_assert(sizeof(ClusterLights) == 128, "matches the shaders' ClusterLights");

struct ClusterGridSettings {
    std::uint32_t tiles_x = 16;
    std::uint32_t tiles_y = 9;
    std::uint32_t slices = 24;
    float max_distance = 80.0f; // metres; lights past it are not shaded
};

// The grid: tiles across the screen, slices along the view direction
// spaced exponentially (equal ratios, so near slices are thin and far ones
// thick), from the camera's near plane to max_distance.
struct ClusterGrid {
    std::uint32_t tiles_x = 0;
    std::uint32_t tiles_y = 0;
    std::uint32_t slices = 0;
    float near = 0.0f;
    float far = 0.0f;
    float slice_scale = 0.0f; // slice = floor(log(depth) * slice_scale + slice_bias)
    float slice_bias = 0.0f;

    [[nodiscard]] static ClusterGrid make(const ClusterGridSettings& settings, float camera_near) noexcept;

    [[nodiscard]] std::uint32_t cluster_count() const noexcept { return tiles_x * tiles_y * slices; }
    [[nodiscard]] std::uint32_t index(std::uint32_t tile_x, std::uint32_t tile_y,
                                      std::uint32_t slice) const noexcept {
        return (slice * tiles_y + tile_y) * tiles_x + tile_x;
    }
    // The slice a view-space depth (metres in front of the camera) falls in, clamped.
    [[nodiscard]] std::uint32_t slice_of(float view_depth) const noexcept;
    // The depth where a slice starts; slice_near(slices) is the far end.
    [[nodiscard]] float slice_near(std::uint32_t slice) const noexcept;
    // A cell's box in view space (x right, y up, the camera looking down
    // -z), given the tangents of the camera's half angles.
    [[nodiscard]] math::Aabb bounds(std::uint32_t tile_x, std::uint32_t tile_y, std::uint32_t slice,
                                    float tan_half_x, float tan_half_y) const noexcept;
};

// What the light-assignment kernel and the shading pass are given.
struct ClusterUniforms {
    math::Vec4 grid;   // tiles_x, tiles_y, slices, light count
    math::Vec4 depth;  // slice_scale, slice_bias, near, far
    math::Vec4 screen; // width, height (pixels), tan_half_x, tan_half_y
    math::Mat4 view;   // world -> view
};
static_assert(sizeof(ClusterUniforms) == 112, "matches the shaders' ClusterUniforms");

[[nodiscard]] ClusterUniforms cluster_uniforms(const ClusterGrid& grid, std::uint32_t light_count,
                                               float width, float height, float tan_half_x, float tan_half_y,
                                               const math::Mat4& view) noexcept;

// The assignment, on the CPU: for every cell, the lights whose spheres
// touch its box, at most kMaxLightsPerCluster of them in light order. What
// the kernel computes; the reference the tests hold it to.
void assign_lights(const ClusterGrid& grid, const math::Mat4& view, float tan_half_x, float tan_half_y,
                   const PointLight* lights, std::uint32_t light_count, ClusterLights* out) noexcept;

// The same assignment as a compute kernel in MSL, one thread per cell:
// [[buffer(0)]] ClusterUniforms, [[buffer(1)]] the lights (read-only),
// [[buffer(2)]] the cells (written). 64 threads a group.
[[nodiscard]] const char* cluster_kernel_msl() noexcept;
[[nodiscard]] rhi::ComputePipelineDesc cluster_kernel_pipeline_desc() noexcept;

} // namespace tynima::render
