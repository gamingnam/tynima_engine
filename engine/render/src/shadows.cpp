#include <tynima/render/shadows.h>

#include <algorithm>
#include <cmath>

namespace tynima::render {

using math::Mat4;
using math::Vec3;

CascadeSet fit_cascades(const Camera& camera, float aspect, const Vec3& light_direction,
                        const CascadeSettings& settings) noexcept {
    CascadeSet set;
    set.count = std::clamp<std::uint32_t>(settings.count, 1, kMaxCascades);
    const float near = std::max(camera.near, 1e-3f);
    const float far = std::max(settings.max_distance, near * 2.0f);

    // Where the slices end: between uniform steps (good far away) and
    // logarithmic ones (good up close), the blend Zhang et al. called the
    // practical split scheme.
    float splits[kMaxCascades + 1];
    splits[0] = near;
    for (std::uint32_t i = 1; i < set.count; ++i) {
        const float p = static_cast<float>(i) / static_cast<float>(set.count);
        const float logarithmic = near * std::pow(far / near, p);
        const float uniform = near + (far - near) * p;
        splits[i] = uniform + (logarithmic - uniform) * settings.split_blend;
    }
    splits[set.count] = far;

    // The light's view: it looks along -light_direction, from anywhere, since
    // an orthographic projection only cares about orientation. The
    // translation comes per cascade, from the slice's centre.
    const Vec3 to_light = normalize(light_direction);
    const Vec3 up = std::fabs(to_light.y) > 0.99f ? Vec3::unit_z() : Vec3::unit_y();
    const Mat4 light_rotation = math::look_at(Vec3::zero(), to_light * -1.0f, up);

    const Vec3 forward = camera.forward();
    const float tan_half_y = std::tan(0.5f * camera.fov_y);
    const float tan_half_x = tan_half_y * aspect;
    const float k = tan_half_x * tan_half_x + tan_half_y * tan_half_y; // corner offset², per unit distance²
    const float map_size = static_cast<float>(std::max<std::uint32_t>(settings.map_size, 1));

    for (std::uint32_t i = 0; i < set.count; ++i) {
        const float n = splits[i];
        const float f = splits[i + 1];
        // The smallest sphere around the slice's eight corners is centred on
        // the view axis, at the distance where the near and far corners are
        // equally far from it — or at the far plane, for a slice so wide that
        // point would lie beyond it.
        const float z = std::min(0.5f * (n + f) * (1.0f + k), f);
        const float to_near = std::sqrt((n - z) * (n - z) + n * n * k);
        const float to_far = std::sqrt((f - z) * (f - z) + f * f * k);
        const float radius = std::max(to_near, to_far);
        const Vec3 center = camera.position + forward * z;

        // The box around the sphere, in light space, snapped to the texel
        // grid so the map's contents shift by whole texels as the camera moves.
        const float texel = 2.0f * radius / map_size;
        const Vec3 c = math::transform_point(light_rotation, center);
        const float snapped_x = std::floor(c.x / texel) * texel;
        const float snapped_y = std::floor(c.y / texel) * texel;
        // Along the light: the light view looks down -z, so what is nearer the
        // light has the larger z. Casters up to `caster_margin` nearer than
        // the sphere still count; nothing farther than the sphere's back
        // matters, as it cannot shadow the slice.
        const float near_distance = -(c.z + radius + settings.caster_margin);
        const float far_distance = -(c.z - radius);
        // orthographic() maps its first distance to depth 0 and its second to
        // depth 1; handing it far then near is what makes this reverse-Z.
        const Mat4 projection = math::orthographic(snapped_x - radius, snapped_x + radius, snapped_y - radius,
                                                   snapped_y + radius, far_distance, near_distance);
        Cascade& cascade = set.cascades[i];
        cascade.view_projection = projection * light_rotation;
        cascade.split_far = f;
        cascade.texel_size = texel;
        cascade.depth_range = far_distance - near_distance;
    }
    return set;
}

} // namespace tynima::render
