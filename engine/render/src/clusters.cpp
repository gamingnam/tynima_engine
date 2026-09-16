#include <tynima/render/clusters.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tynima::render {

using math::Aabb;
using math::Mat4;
using math::Vec3;
using math::Vec4;

ClusterGrid ClusterGrid::make(const ClusterGridSettings& settings, float camera_near) noexcept {
    ClusterGrid grid;
    grid.tiles_x = std::max<std::uint32_t>(settings.tiles_x, 1);
    grid.tiles_y = std::max<std::uint32_t>(settings.tiles_y, 1);
    grid.slices = std::max<std::uint32_t>(settings.slices, 1);
    grid.near = std::max(camera_near, 1e-3f);
    grid.far = std::max(settings.max_distance, grid.near * 2.0f);
    // slice = slices * log(depth / near) / log(far / near), split into a
    // scale and a bias so the shader does one multiply-add.
    const float log_ratio = std::log(grid.far / grid.near);
    grid.slice_scale = static_cast<float>(grid.slices) / log_ratio;
    grid.slice_bias = -static_cast<float>(grid.slices) * std::log(grid.near) / log_ratio;
    return grid;
}

std::uint32_t ClusterGrid::slice_of(float view_depth) const noexcept {
    if (view_depth <= near) {
        return 0;
    }
    const float s = std::floor(std::log(view_depth) * slice_scale + slice_bias);
    return static_cast<std::uint32_t>(std::clamp(s, 0.0f, static_cast<float>(slices - 1)));
}

float ClusterGrid::slice_near(std::uint32_t slice) const noexcept {
    return near * std::pow(far / near, static_cast<float>(slice) / static_cast<float>(slices));
}

Aabb ClusterGrid::bounds(std::uint32_t tile_x, std::uint32_t tile_y, std::uint32_t slice, float tan_half_x,
                         float tan_half_y) const noexcept {
    // The tile's rectangle in NDC: x runs left to right, y from the top row down.
    const float x0 = -1.0f + 2.0f * static_cast<float>(tile_x) / static_cast<float>(tiles_x);
    const float x1 = -1.0f + 2.0f * static_cast<float>(tile_x + 1) / static_cast<float>(tiles_x);
    const float y0 = 1.0f - 2.0f * static_cast<float>(tile_y + 1) / static_cast<float>(tiles_y);
    const float y1 = 1.0f - 2.0f * static_cast<float>(tile_y) / static_cast<float>(tiles_y);
    const float d0 = slice_near(slice);
    const float d1 = slice_near(slice + 1);
    // In view space a point at NDC (x, y) and depth d sits at (x tan_x d, y tan_y d, -d);
    // over the two depths the box is the extreme of each corner.
    Aabb box = Aabb::empty();
    for (const float d : {d0, d1}) {
        for (const float x : {x0, x1}) {
            for (const float y : {y0, y1}) {
                const Vec3 corner{x * tan_half_x * d, y * tan_half_y * d, -d};
                box.min = math::min(box.min, corner);
                box.max = math::max(box.max, corner);
            }
        }
    }
    return box;
}

ClusterUniforms cluster_uniforms(const ClusterGrid& grid, std::uint32_t light_count, float width,
                                 float height, float tan_half_x, float tan_half_y,
                                 const Mat4& view) noexcept {
    return ClusterUniforms{
        .grid = Vec4{static_cast<float>(grid.tiles_x), static_cast<float>(grid.tiles_y),
                     static_cast<float>(grid.slices), static_cast<float>(light_count)},
        .depth = Vec4{grid.slice_scale, grid.slice_bias, grid.near, grid.far},
        .screen = Vec4{width, height, tan_half_x, tan_half_y},
        .view = view,
    };
}

void assign_lights(const ClusterGrid& grid, const Mat4& view, float tan_half_x, float tan_half_y,
                   const PointLight* lights, std::uint32_t light_count, ClusterLights* out) noexcept {
    for (std::uint32_t slice = 0; slice < grid.slices; ++slice) {
        for (std::uint32_t ty = 0; ty < grid.tiles_y; ++ty) {
            for (std::uint32_t tx = 0; tx < grid.tiles_x; ++tx) {
                const Aabb box = grid.bounds(tx, ty, slice, tan_half_x, tan_half_y);
                ClusterLights& cell = out[grid.index(tx, ty, slice)];
                cell.count = 0;
                for (std::uint32_t i = 0; i < light_count && cell.count < kMaxLightsPerCluster; ++i) {
                    const Vec3 p = math::transform_point(view, lights[i].position_radius.xyz());
                    const float r = lights[i].position_radius.w;
                    // The sphere touches the box when the box's point nearest
                    // the centre is within the radius.
                    const Vec3 q = math::min(math::max(p, box.min), box.max);
                    if (math::length_squared(p - q) <= r * r) {
                        cell.indices[cell.count++] = i;
                    }
                }
            }
        }
    }
}

namespace {

// The same, one thread per cell. Written against the CPU version above; the
// tests hold the two to each other.
constexpr const char* kClusterKernelMsl = R"(
#include <metal_stdlib>
using namespace metal;

struct PointLight {
    float4 position_radius;
    float4 color;
};

struct ClusterLights {
    uint count;
    uint indices[31];
};

struct ClusterUniforms {
    float4 grid;   // tiles_x, tiles_y, slices, light count
    float4 depth;  // slice_scale, slice_bias, near, far
    float4 screen; // width, height, tan_half_x, tan_half_y
    float4x4 view;
};

kernel void cs_main(constant ClusterUniforms& u [[buffer(0)]],
                    const device PointLight* lights [[buffer(1)]],
                    device ClusterLights* cells [[buffer(2)]],
                    uint id [[thread_position_in_grid]]) {
    uint tiles_x = uint(u.grid.x);
    uint tiles_y = uint(u.grid.y);
    uint slices = uint(u.grid.z);
    if (id >= tiles_x * tiles_y * slices) {
        return;
    }
    uint tx = id % tiles_x;
    uint ty = (id / tiles_x) % tiles_y;
    uint slice = id / (tiles_x * tiles_y);

    // The cell's box in view space: the tile's NDC rectangle at the slice's
    // two depths, x right, y up from the top row down, z towards the viewer.
    float x0 = -1.0 + 2.0 * float(tx) / float(tiles_x);
    float x1 = -1.0 + 2.0 * float(tx + 1) / float(tiles_x);
    float y0 = 1.0 - 2.0 * float(ty + 1) / float(tiles_y);
    float y1 = 1.0 - 2.0 * float(ty) / float(tiles_y);
    float near_depth = u.depth.z;
    float ratio = u.depth.w / u.depth.z;
    float d0 = near_depth * pow(ratio, float(slice) / float(slices));
    float d1 = near_depth * pow(ratio, float(slice + 1) / float(slices));
    // Per unit of depth, the tile's extents; over the two depths, the
    // smallest and largest each reaches.
    float x_lo = min(x0, x1) * u.screen.z;
    float x_hi = max(x0, x1) * u.screen.z;
    float y_lo = min(y0, y1) * u.screen.w;
    float y_hi = max(y0, y1) * u.screen.w;
    float3 lo = float3(min(x_lo * d0, x_lo * d1), min(y_lo * d0, y_lo * d1), -d1);
    float3 hi = float3(max(x_hi * d0, x_hi * d1), max(y_hi * d0, y_hi * d1), -d0);

    uint light_count = uint(u.grid.w);
    uint count = 0;
    for (uint i = 0; i < light_count && count < 31; ++i) {
        float3 p = (u.view * float4(lights[i].position_radius.xyz, 1.0)).xyz;
        float r = lights[i].position_radius.w;
        float3 q = clamp(p, lo, hi);
        float3 away = p - q;
        if (dot(away, away) <= r * r) {
            cells[id].indices[count] = i;
            count += 1;
        }
    }
    cells[id].count = count;
}
)";

} // namespace

const char* cluster_kernel_msl() noexcept {
    return kClusterKernelMsl;
}

rhi::ComputePipelineDesc cluster_kernel_pipeline_desc() noexcept {
    return rhi::ComputePipelineDesc{.format = rhi::ShaderFormat::Msl,
                                    .code = kClusterKernelMsl,
                                    .code_size = std::strlen(kClusterKernelMsl),
                                    .entry_point = "cs_main",
                                    .num_uniform_buffers = 1,
                                    .num_readonly_storage_buffers = 1,
                                    .num_readwrite_storage_buffers = 1,
                                    .threads_x = 64};
}

} // namespace tynima::render
