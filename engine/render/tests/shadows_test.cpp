#include <doctest/doctest.h>
#include <tynima/render/shadows.h>

#include <cmath>
#include <initializer_list>

using namespace tynima;
using namespace tynima::math;
using render::Cascade;
using render::CascadeSet;
using render::CascadeSettings;

namespace {

constexpr float kAspect = 16.0f / 9.0f;
const Vec3 kLight = normalize(Vec3{0.4f, 1.0f, 0.3f}); // towards the light

render::Camera posed(const Vec3& position, float yaw) {
    render::Camera camera;
    camera.position = position;
    camera.rotation = Quat::from_axis_angle(Vec3::unit_y(), yaw);
    return camera;
}

// The eight corners of the view frustum between two distances.
void slice_corners(const render::Camera& camera, float near, float far, Vec3 out[8]) {
    const float tan_y = std::tan(0.5f * camera.fov_y);
    const float tan_x = tan_y * kAspect;
    int n = 0;
    for (const float d : {near, far}) {
        for (const float sx : {-1.0f, 1.0f}) {
            for (const float sy : {-1.0f, 1.0f}) {
                out[n++] = camera.position + camera.forward() * d + camera.right() * (sx * d * tan_x) +
                           camera.up() * (sy * d * tan_y);
            }
        }
    }
}

} // namespace

TEST_CASE("the slices tile the view distance, nearer ones thinner") {
    const render::Camera camera = posed({1.0f, 2.0f, 3.0f}, 0.3f);
    const CascadeSettings settings{.count = 4, .max_distance = 60.0f, .split_blend = 0.7f};
    const CascadeSet set = render::fit_cascades(camera, kAspect, kLight, settings);
    REQUIRE(set.count == 4);
    float previous = camera.near;
    for (std::uint32_t i = 0; i < set.count; ++i) {
        CHECK(set.cascades[i].split_far > previous);
        previous = set.cascades[i].split_far;
    }
    CHECK(set.cascades[3].split_far == doctest::Approx(60.0f));
    // Each slice is wider than the one before, and each cascade's texels bigger.
    for (std::uint32_t i = 1; i < set.count; ++i) {
        const float width = set.cascades[i].split_far - set.cascades[i - 1].split_far;
        const float previous_width =
            set.cascades[i - 1].split_far - (i >= 2 ? set.cascades[i - 2].split_far : camera.near);
        CHECK(width > previous_width);
        CHECK(set.cascades[i].texel_size > set.cascades[i - 1].texel_size);
    }
    // With the blend at 0 the slices are uniform.
    const CascadeSet uniform =
        render::fit_cascades(camera, kAspect, kLight, {.count = 4, .split_blend = 0.0f});
    CHECK(uniform.cascades[0].split_far == doctest::Approx(camera.near + (60.0f - camera.near) * 0.25f));
}

TEST_CASE("every corner of a slice projects inside its cascade, at a depth the map can hold") {
    for (const float yaw : {0.0f, 1.1f, 2.9f}) {
        const render::Camera camera = posed({-4.0f, 1.5f, 7.0f}, yaw);
        const CascadeSettings settings{.count = 4, .map_size = 1024, .max_distance = 40.0f};
        const CascadeSet set = render::fit_cascades(camera, kAspect, kLight, settings);
        float near = camera.near;
        for (std::uint32_t i = 0; i < set.count; ++i) {
            const Cascade& cascade = set.cascades[i];
            Vec3 corners[8];
            slice_corners(camera, near, cascade.split_far, corners);
            for (const Vec3& corner : corners) {
                const Vec4 clip = cascade.view_projection * Vec4{corner, 1.0f};
                CHECK(clip.w == doctest::Approx(1.0f)); // orthographic
                CHECK(std::fabs(clip.x) <= 1.0f + 1e-4f);
                CHECK(std::fabs(clip.y) <= 1.0f + 1e-4f);
                CHECK(clip.z >= -1e-4f);
                CHECK(clip.z <= 1.0f + 1e-4f);
            }
            near = cascade.split_far;
        }
    }
}

TEST_CASE("depth is reverse-Z: what is nearer the light is deeper into 1") {
    const render::Camera camera = posed({0.0f, 1.0f, 5.0f}, 0.0f);
    const CascadeSet set = render::fit_cascades(camera, kAspect, kLight, {.count = 2});
    const Cascade& cascade = set.cascades[1];
    const Vec3 point = camera.position + camera.forward() * 3.0f;
    const Vec3 nearer_light = point + kLight * 2.0f;
    const float depth_point = (cascade.view_projection * Vec4{point, 1.0f}).z;
    const float depth_nearer = (cascade.view_projection * Vec4{nearer_light, 1.0f}).z;
    CHECK(depth_nearer > depth_point);
    // Two metres along the light is two metres of the depth range.
    CHECK(depth_nearer - depth_point == doctest::Approx(2.0f / cascade.depth_range).epsilon(1e-3));
    // Casters far behind the slice, towards the light, are still in range.
    const Vec3 caster = point + kLight * 25.0f;
    CHECK((cascade.view_projection * Vec4{caster, 1.0f}).z <= 1.0f);
}

TEST_CASE("the box keeps its size as the camera turns, and moves by whole texels as it walks") {
    const CascadeSettings settings{.count = 3, .map_size = 2048};
    const render::Camera a = posed({2.0f, 1.0f, 0.0f}, 0.2f);
    const render::Camera turned = posed({2.0f, 1.0f, 0.0f}, 1.7f);
    const CascadeSet set_a = render::fit_cascades(a, kAspect, kLight, settings);
    const CascadeSet set_turned = render::fit_cascades(turned, kAspect, kLight, settings);
    for (std::uint32_t i = 0; i < set_a.count; ++i) {
        CHECK(set_turned.cascades[i].texel_size == doctest::Approx(set_a.cascades[i].texel_size));
    }

    // A step smaller than a texel: the world origin lands on the same texel
    // of the map, or one whole texel over — never a fraction.
    const render::Camera walked = posed({2.0f + 0.0007f, 1.0f, 0.0f + 0.0004f}, 0.2f);
    const CascadeSet set_walked = render::fit_cascades(walked, kAspect, kLight, settings);
    for (std::uint32_t i = 0; i < set_a.count; ++i) {
        const Vec4 before = set_a.cascades[i].view_projection * Vec4{Vec3::zero(), 1.0f};
        const Vec4 after = set_walked.cascades[i].view_projection * Vec4{Vec3::zero(), 1.0f};
        const float texels_x = (after.x - before.x) * 0.5f * 2048.0f; // clip spans two units over the map
        const float texels_y = (after.y - before.y) * 0.5f * 2048.0f;
        CHECK(std::fabs(texels_x - std::round(texels_x)) < 1e-2f);
        CHECK(std::fabs(texels_y - std::round(texels_y)) < 1e-2f);
        CHECK(std::fabs(texels_x) <= 1.0f + 1e-2f);
        CHECK(std::fabs(texels_y) <= 1.0f + 1e-2f);
    }
}

TEST_CASE("a light straight overhead still gets a well-formed box") {
    const render::Camera camera = posed({0.0f, 1.0f, 0.0f}, 0.0f);
    const CascadeSet set = render::fit_cascades(camera, kAspect, Vec3::unit_y(), {.count = 2});
    for (std::uint32_t i = 0; i < set.count; ++i) {
        const Mat4& m = set.cascades[i].view_projection;
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                CHECK(std::isfinite(m(r, c)));
            }
        }
        CHECK(set.cascades[i].texel_size > 0.0f);
    }
}
