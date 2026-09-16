#include <doctest/doctest.h>
#include <tynima/platform/platform.h>
#include <tynima/render/camera.h>
#include <tynima/render/clusters.h>

#include <cmath>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace tynima;
using namespace tynima::math;
using render::ClusterGrid;
using render::ClusterGridSettings;
using render::ClusterLights;
using render::PointLight;

namespace {

constexpr float kAspect = 16.0f / 9.0f;

struct View {
    render::Camera camera;
    float tan_half_y = std::tan(0.5f * camera.fov_y);
    float tan_half_x = tan_half_y * kAspect;
};

// Lights scattered through the frustum out to the grid's far end, at
// every size from a candle to a room.
std::vector<PointLight> scatter(const ClusterGrid& grid, const View& view, std::uint32_t count,
                                unsigned seed) {
    std::mt19937 engine{seed};
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    std::vector<PointLight> lights;
    for (std::uint32_t i = 0; i < count; ++i) {
        const float depth = grid.near + (grid.far - grid.near) * unit(engine);
        const float x = (unit(engine) * 2.0f - 1.0f) * view.tan_half_x * depth;
        const float y = (unit(engine) * 2.0f - 1.0f) * view.tan_half_y * depth;
        const Vec3 world = view.camera.position + view.camera.right() * x + view.camera.up() * y +
                           view.camera.forward() * depth;
        const float reach = 0.2f + unit(engine) * 6.0f;
        lights.push_back({{world, reach}, {unit(engine), unit(engine), unit(engine), 0.0f}});
    }
    return lights;
}

} // namespace

TEST_CASE("slices are spaced by equal ratios and round-trip through slice_of") {
    const ClusterGrid grid = ClusterGrid::make({.slices = 24, .max_distance = 80.0f}, 0.1f);
    CHECK(grid.slice_near(0) == doctest::Approx(0.1f));
    CHECK(grid.slice_near(24) == doctest::Approx(80.0f));
    for (std::uint32_t s = 1; s < 24; ++s) {
        const float ratio = grid.slice_near(s + 1) / grid.slice_near(s);
        CHECK(ratio == doctest::Approx(grid.slice_near(1) / grid.slice_near(0)).epsilon(1e-4));
        // Just inside a slice's start maps to it; just before, to the one below.
        CHECK(grid.slice_of(grid.slice_near(s) * 1.001f) == s);
        CHECK(grid.slice_of(grid.slice_near(s) * 0.999f) == s - 1);
    }
    CHECK(grid.slice_of(0.0f) == 0);
    CHECK(grid.slice_of(1000.0f) == 23);
    CHECK(grid.cluster_count() == 16u * 9u * 24u);
    CHECK(grid.index(15, 8, 23) == grid.cluster_count() - 1);
    CHECK(grid.index(1, 0, 0) == 1);
    CHECK(grid.index(0, 1, 0) == 16);
    CHECK(grid.index(0, 0, 1) == 16 * 9);
}

TEST_CASE("a cell's box spans exactly its tile and its slice") {
    const View view;
    const ClusterGridSettings settings{.tiles_x = 4, .tiles_y = 2, .slices = 3, .max_distance = 27.0f};
    const ClusterGrid grid = ClusterGrid::make(settings, 1.0f);
    // Slices at 1, 3, 9, 27 m: equal ratios of three.
    const Aabb box = grid.bounds(0, 0, 1, view.tan_half_x, view.tan_half_y);
    CHECK(box.max.z == doctest::Approx(-3.0f));
    CHECK(box.min.z == doctest::Approx(-9.0f));
    // Tile (0, 0) is the top-left quarter-width: NDC x in [-1, -0.5], y in [0, 1].
    CHECK(box.min.x == doctest::Approx(-1.0f * view.tan_half_x * 9.0f));
    CHECK(box.max.x == doctest::Approx(-0.5f * view.tan_half_x * 3.0f));
    CHECK(box.min.y == doctest::Approx(0.0f));
    CHECK(box.max.y == doctest::Approx(1.0f * view.tan_half_y * 9.0f));
    // The cells tile the frustum slice: the union of the four tiles of a row
    // spans the whole width at the far depth.
    float left = 1e9f, right = -1e9f;
    for (std::uint32_t tx = 0; tx < 4; ++tx) {
        const Aabb b = grid.bounds(tx, 1, 2, view.tan_half_x, view.tan_half_y);
        left = std::min(left, b.min.x);
        right = std::max(right, b.max.x);
    }
    CHECK(left == doctest::Approx(-view.tan_half_x * 27.0f));
    CHECK(right == doctest::Approx(view.tan_half_x * 27.0f));
}

TEST_CASE("every light lands in the cells it reaches and in no others") {
    View view;
    view.camera.position = {2.0f, 1.5f, -3.0f};
    view.camera.rotation = Quat::from_axis_angle(Vec3::unit_y(), 0.7f);
    const ClusterGrid grid = ClusterGrid::make({}, view.camera.near);
    const std::vector<PointLight> lights = scatter(grid, view, 100, 11);
    std::vector<ClusterLights> cells(grid.cluster_count());
    const Mat4 v = view.camera.view();
    render::assign_lights(grid, v, view.tan_half_x, view.tan_half_y, lights.data(), 100, cells.data());

    // The cell holding a light's centre lists it; a cell farther from the
    // centre than the radius does not.
    std::uint32_t listed = 0, total = 0;
    for (std::uint32_t i = 0; i < lights.size(); ++i) {
        const Vec3 p = transform_point(v, lights[i].position_radius.xyz());
        const float depth = -p.z;
        if (depth <= grid.near || depth >= grid.far) {
            continue;
        }
        const float ndc_x = p.x / (view.tan_half_x * depth);
        const float ndc_y = p.y / (view.tan_half_y * depth);
        if (std::fabs(ndc_x) >= 1.0f || std::fabs(ndc_y) >= 1.0f) {
            continue;
        }
        const auto tx = static_cast<std::uint32_t>((ndc_x * 0.5f + 0.5f) * static_cast<float>(grid.tiles_x));
        const auto ty = static_cast<std::uint32_t>((0.5f - ndc_y * 0.5f) * static_cast<float>(grid.tiles_y));
        const ClusterLights& cell = cells[grid.index(tx, ty, grid.slice_of(depth))];
        bool found = false;
        for (std::uint32_t k = 0; k < cell.count; ++k) {
            found = found || cell.indices[k] == i;
        }
        ++total;
        listed += found ? 1 : 0;
    }
    MESSAGE(listed << " of " << total << " lights inside the grid are listed by their own cell");
    CHECK(listed == total);
    CHECK(total > 50);

    std::uint32_t wrong = 0, entries = 0;
    for (std::uint32_t c = 0; c < grid.cluster_count(); ++c) {
        const std::uint32_t slice = c / (grid.tiles_x * grid.tiles_y);
        const std::uint32_t ty = (c / grid.tiles_x) % grid.tiles_y;
        const std::uint32_t tx = c % grid.tiles_x;
        const Aabb box = grid.bounds(tx, ty, slice, view.tan_half_x, view.tan_half_y);
        for (std::uint32_t k = 0; k < cells[c].count; ++k) {
            const PointLight& light = lights[cells[c].indices[k]];
            const Vec3 p = transform_point(v, light.position_radius.xyz());
            const Vec3 q = math::min(math::max(p, box.min), box.max);
            wrong += length(p - q) <= light.position_radius.w ? 0 : 1;
            ++entries;
        }
    }
    MESSAGE(entries << " cell entries for 100 lights over " << grid.cluster_count() << " cells");
    CHECK(wrong == 0);
    CHECK(entries > 100);
}

TEST_CASE("the kernel on the GPU agrees with the CPU assignment") {
    // Needs a device: skipped where SDL has no GPU backend.
    const bool real_driver = platform::init();
    if (!real_driver) {
        platform::shutdown();
        REQUIRE(platform::init({.headless = true}));
    }
    auto device = rhi::Device::create({.debug = false});
    if (device == nullptr) {
        MESSAGE("skipped: no GPU device (", std::string(platform::last_error()), ")");
        platform::shutdown();
        return;
    }
    {
        View view;
        view.camera.position = {-1.0f, 2.0f, 4.0f};
        view.camera.rotation = Quat::from_axis_angle(normalize(Vec3{0.1f, 1.0f, 0.0f}), -0.4f);
        const ClusterGrid grid = ClusterGrid::make({}, view.camera.near);
        const std::vector<PointLight> lights = scatter(grid, view, 150, 5);
        std::vector<ClusterLights> expected(grid.cluster_count());
        const Mat4 v = view.camera.view();
        render::assign_lights(grid, v, view.tan_half_x, view.tan_half_y, lights.data(), 150, expected.data());

        const rhi::ComputePipelineHandle kernel =
            device->create_compute_pipeline(render::cluster_kernel_pipeline_desc());
        REQUIRE_MESSAGE(static_cast<bool>(kernel), platform::last_error());
        const auto light_bytes = static_cast<std::uint32_t>(lights.size() * sizeof(PointLight));
        const auto cell_bytes = static_cast<std::uint32_t>(expected.size() * sizeof(ClusterLights));
        const rhi::BufferHandle light_buffer =
            device->create_buffer({.usage = rhi::BufferUsage::Storage, .size = light_bytes});
        const rhi::BufferHandle cell_buffer =
            device->create_buffer({.usage = rhi::BufferUsage::Storage, .size = cell_bytes});
        REQUIRE(static_cast<bool>(light_buffer));
        REQUIRE(static_cast<bool>(cell_buffer));

        auto frame = device->begin_frame();
        REQUIRE(frame.has_value());
        REQUIRE(frame->write_buffer(light_buffer, lights.data(), light_bytes));
        rhi::ComputePassDesc desc{.name = "clusters"};
        desc.writes[0] = cell_buffer;
        desc.write_count = 1;
        auto pass = frame->begin_compute_pass(desc);
        REQUIRE_MESSAGE(pass.has_value(), platform::last_error());
        pass->bind_pipeline(kernel);
        pass->bind_storage_buffer(0, light_buffer);
        const render::ClusterUniforms uniforms =
            render::cluster_uniforms(grid, 150, 1280.0f, 720.0f, view.tan_half_x, view.tan_half_y, v);
        pass->push_uniforms(0, &uniforms, sizeof uniforms);
        pass->dispatch((grid.cluster_count() + 63) / 64);
        pass->end();
        frame->submit();

        std::vector<ClusterLights> actual(grid.cluster_count());
        REQUIRE_MESSAGE(device->download_buffer(cell_buffer, actual.data(), cell_bytes),
                        platform::last_error());
        std::uint32_t differing = 0;
        for (std::uint32_t c = 0; c < grid.cluster_count(); ++c) {
            differing += std::memcmp(&actual[c], &expected[c], sizeof(ClusterLights)) == 0 ? 0 : 1;
        }
        MESSAGE(differing << " of " << grid.cluster_count() << " cells differ between the GPU and the CPU");
        // Only a light exactly on a cell's edge can come out differently.
        CHECK(differing * 200 <= grid.cluster_count());

        device->destroy_buffer(light_buffer);
        device->destroy_buffer(cell_buffer);
        device->destroy_compute_pipeline(kernel);
    }
    device.reset();
    platform::shutdown();
}
