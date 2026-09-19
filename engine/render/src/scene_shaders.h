#pragma once

// The scene's shaders, private to render/. MSL, compiled by the device at
// run time, assembled from pieces so the forward path and the deferred one
// light a pixel with the same code: kMslCommon (the uniform structs and the
// BRDF), kMslMeshVertex (the vertex shader every mesh pass shares),
// kMslSurface (a material sampled into a Surface), kMslLighting (a Surface
// lit by the sun, its shadow and the cell's point lights), then one fragment
// function per path — forward, G-buffer, and the fused and split lighting
// draws. kShadowMsl is the depth-only pass for the cascades.
//
// Bindings, on both backends: vertex uniforms at [[buffer(0)]]; attributes
// from render::vertex_layout() through [[stage_in]]. Fragment uniforms:
// frame at [[buffer(0)]], material at [[buffer(1)]], shadows and clusters
// after them; the material maps at [[texture(0..4)]] share sampler 0, the
// shadow maps follow. Every texel is linear by the time it is sampled (sRGB
// formats decode in hardware), lighting happens in linear, and the post
// stack encodes on the way out.
namespace tynima::render::shaders {

constexpr const char* kMslCommon = R"(
#include <metal_stdlib>
using namespace metal;

struct FrameUniforms {
    float4 camera_position; // xyz
    float4 camera_forward;  // xyz: the view direction, for a pixel's depth
    float4 light_direction; // xyz: towards the light; w: intensity
    float4 light_color;     // rgb; w: ambient intensity
    float4 params;          // x: shading model, y: debug view
};

struct MaterialUniforms {
    float4 base_color_factor;
    float4 factors;         // x: metallic, y: roughness, z: occlusion strength, w: normal scale
    float4 emissive_factor; // rgb
};

struct ShadowUniforms {
    float4x4 cascade_matrix[4]; // world -> light clip, reverse-Z
    float4 cascade_param[4];    // x: texel size (m), y: depth per metre, z: texel size in uv
    float4 settings;            // x: cascades, y: strength (0: off), z: normal offset, w: bias (in texels)
};

// The clustered point lights: render/clusters.h, exactly.
struct ClusterUniforms {
    float4 grid;   // tiles_x, tiles_y, slices, light count
    float4 depth;  // slice_scale, slice_bias, near, far
    float4 screen; // width, height, tan_half_x, tan_half_y
    float4x4 view;
};

// What the deferred lighting pass needs to put a pixel back in the world.
struct DeferredUniforms {
    float4x4 inverse_view;
    float4 params; // x, y: tan of the half angles; z, w: the screen in pixels
};

struct PointLight {
    float4 position_radius;
    float4 color;
};

struct ClusterLights {
    uint count;
    uint indices[31];
};

// What a material amounts to at one pixel, whichever path sampled it.
struct Surface {
    float3 albedo;
    float3 n;           // the shading normal, mapped
    float3 geometric_n; // the mesh's own, for the shadow bias
    float3 t;           // the tangent, for one debug view
    float metallic;
    float roughness;
    float occlusion;
    float3 emissive;
    float alpha;
};

constant float kPi = 3.14159265358979;

// GGX / Trowbridge-Reitz normal distribution: how many microfacets face h.
float d_ggx(float n_dot_h, float alpha) {
    float a2 = alpha * alpha;
    float d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}

// Height-correlated Smith visibility (includes the 1 / (4 n.l n.v) term).
float v_smith_ggx_correlated(float n_dot_v, float n_dot_l, float alpha) {
    float a2 = alpha * alpha;
    float gv = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2);
    float gl = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

// Schlick's Fresnel: reflectance rises to 1 at grazing angles.
float3 f_schlick(float v_dot_h, float3 f0) {
    float f = pow(1.0 - v_dot_h, 5.0);
    return f0 + (float3(1.0) - f0) * f;
}

// What a surface reflects of light from `l` towards `v`, per unit of
// irradiance: the BRDF, before the cosine. Every light — the sun and each
// point light — goes through this once.
float3 surface_brdf(int mode, float3 n, float3 v, float3 l, float3 base, float metallic, float roughness) {
    float3 h = normalize(l + v);
    float n_dot_l = saturate(dot(n, l));
    float n_dot_v = max(dot(n, v), 1e-4);
    float n_dot_h = saturate(dot(n, h));
    float v_dot_h = saturate(dot(v, h));
    float alpha = roughness * roughness;
    if (mode == 1) {
        // Normalized Blinn-Phong. Shininess is derived from roughness so both
        // models agree on how glossy a surface is; what differs is the shape
        // of the highlight and the missing Fresnel and masking terms.
        float shininess = 2.0 / (alpha * alpha) - 2.0;
        float3 spec_color = mix(float3(0.04), base, metallic);
        float3 diffuse = base * (1.0 - metallic) / kPi;
        float3 specular = spec_color * pow(n_dot_h, shininess) * (shininess + 8.0) / (8.0 * kPi);
        return diffuse + specular;
    }
    // Cook-Torrance with the metallic workflow: dielectrics reflect 4% at
    // normal incidence and keep their albedo as diffuse; metals reflect
    // their albedo as specular and have no diffuse at all.
    float3 f0 = mix(float3(0.04), base, metallic);
    float3 diffuse_color = base * (1.0 - metallic);
    float3 f = f_schlick(v_dot_h, f0);
    float d = d_ggx(n_dot_h, alpha);
    float vis = v_smith_ggx_correlated(n_dot_v, n_dot_l, alpha);
    return (float3(1.0) - f) * diffuse_color / kPi + d * vis * f;
}
)";

constexpr const char* kMslMeshVertex = R"(
struct VertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 uv       [[attribute(2)]];
    float4 tangent  [[attribute(3)]]; // xyz along +u, w = handedness
};

struct Uniforms {
    float4x4 mvp;
    float4x4 model;
};

struct VSOut {
    float4 position [[position]];
    float3 world_position;
    float3 world_normal;
    float4 world_tangent;
    float2 uv;
};

vertex VSOut vs_main(VertexIn in [[stage_in]], constant Uniforms& u [[buffer(0)]]) {
    VSOut out;
    float4 world = u.model * float4(in.position, 1.0);
    out.position = u.mvp * float4(in.position, 1.0);
    out.world_position = world.xyz;
    out.world_normal = (u.model * float4(in.normal, 0.0)).xyz;
    out.world_tangent = float4((u.model * float4(in.tangent.xyz, 0.0)).xyz, in.tangent.w);
    out.uv = in.uv;
    return out;
}
)";

// The material at a pixel: its five maps sampled and its tangent frame
// re-orthogonalized, into a Surface.
constexpr const char* kMslSurface = R"(
Surface sample_surface(VSOut in, texture2d<float> base_color_map, texture2d<float> metallic_roughness_map,
                       texture2d<float> occlusion_map, texture2d<float> emissive_map,
                       texture2d<float> normal_map, sampler map_sampler, constant MaterialUniforms& m) {
    Surface s;
    float4 base = base_color_map.sample(map_sampler, in.uv) * m.base_color_factor;
    float4 mr = metallic_roughness_map.sample(map_sampler, in.uv);
    s.albedo = base.rgb;
    s.alpha = base.a;
    s.metallic = mr.b * m.factors.x;                        // glTF: metallic in B
    s.roughness = clamp(mr.g * m.factors.y, 0.045, 1.0);    // roughness in G; 0 would make GGX blow up
    s.occlusion = mix(1.0, occlusion_map.sample(map_sampler, in.uv).r, m.factors.z);
    s.emissive = emissive_map.sample(map_sampler, in.uv).rgb * m.emissive_factor.rgb;

    // Tangent frame: re-orthogonalize the interpolated tangent against the
    // interpolated normal, then the bitangent follows from the handedness.
    s.geometric_n = normalize(in.world_normal);
    float3 t = in.world_tangent.xyz;
    t = normalize(t - s.geometric_n * dot(s.geometric_n, t));
    float3 b = cross(s.geometric_n, t) * (in.world_tangent.w < 0.0 ? -1.0 : 1.0);
    float3 nm = normal_map.sample(map_sampler, in.uv).xyz * 2.0 - 1.0; // tangent space, +z straight up
    nm.xy *= m.factors.w;
    s.n = normalize(t * nm.x + b * nm.y + s.geometric_n * nm.z);
    s.t = t;
    return s;
}
)";

// A Surface at a world position, lit: the sun through its shadow map, the
// point lights of the pixel's cluster cell, a uniform environment — or one
// of the debug views instead.
constexpr const char* kMslLighting = R"(
// 3x3 taps of the hardware's own 2x2 compare: a soft edge four texels wide.
float shadow_pcf(depth2d<float> map, sampler s, float2 uv, float depth, float texel_uv) {
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            sum += map.sample_compare(s, uv + float2(x, y) * texel_uv, depth);
        }
    }
    return sum / 9.0;
}

// Shadow: from the first cascade whose map holds this point. The lookup
// point is pushed off the surface along the normal, more where the light
// grazes it, and the depth nudged towards the light — the two biases that
// keep a lit surface from shadowing itself.
float shadow_at(float3 world_position, float3 geometric_n, float3 l, constant ShadowUniforms& shadows,
                depth2d<float> shadow_map0, depth2d<float> shadow_map1, depth2d<float> shadow_map2,
                depth2d<float> shadow_map3, sampler shadow_sampler, thread int& cascade) {
    float shadow = 1.0;
    cascade = -1;
    if (shadows.settings.y <= 0.0) {
        return shadow;
    }
    float grazing = 1.0 - saturate(dot(geometric_n, l));
    for (int c = 0; c < int(shadows.settings.x); ++c) {
        float4 param = shadows.cascade_param[c];
        float3 lookup = world_position + geometric_n * (param.x * shadows.settings.z * grazing);
        float4 lc = shadows.cascade_matrix[c] * float4(lookup, 1.0);
        float2 uv = float2(lc.x * 0.5 + 0.5, 0.5 - lc.y * 0.5);
        float border = param.z * 2.0; // the taps must stay inside the map
        bool inside = uv.x > border && uv.x < 1.0 - border && uv.y > border && uv.y < 1.0 - border;
        if (inside && lc.z > 0.0 && lc.z < 1.0) {
            float depth = lc.z + param.x * shadows.settings.w * param.y;
            if (c == 0) {
                shadow = shadow_pcf(shadow_map0, shadow_sampler, uv, depth, param.z);
            } else if (c == 1) {
                shadow = shadow_pcf(shadow_map1, shadow_sampler, uv, depth, param.z);
            } else if (c == 2) {
                shadow = shadow_pcf(shadow_map2, shadow_sampler, uv, depth, param.z);
            } else {
                shadow = shadow_pcf(shadow_map3, shadow_sampler, uv, depth, param.z);
            }
            cascade = c;
            break;
        }
    }
    return 1.0 - (1.0 - shadow) * shadows.settings.y;
}

// The cluster cell a pixel is in: the tile under the window position, the
// slice at its view depth.
uint cluster_cell(float2 window_position, float view_depth, constant ClusterUniforms& clusters) {
    float depth = max(view_depth, clusters.depth.z);
    uint slice = uint(clamp(floor(log(depth) * clusters.depth.x + clusters.depth.y), 0.0,
                            clusters.grid.z - 1.0));
    uint tx = min(uint(window_position.x / clusters.screen.x * clusters.grid.x), uint(clusters.grid.x) - 1);
    uint ty = min(uint(window_position.y / clusters.screen.y * clusters.grid.y), uint(clusters.grid.y) - 1);
    return (slice * uint(clusters.grid.y) + ty) * uint(clusters.grid.x) + tx;
}

float3 shade(Surface s, float3 world_position, float2 window_position, float view_depth,
             constant FrameUniforms& frame, constant ShadowUniforms& shadows,
             constant ClusterUniforms& clusters, const device PointLight* lights,
             const device ClusterLights* cells, depth2d<float> shadow_map0, depth2d<float> shadow_map1,
             depth2d<float> shadow_map2, depth2d<float> shadow_map3, sampler shadow_sampler) {
    float3 v = normalize(frame.camera_position.xyz - world_position);
    float3 l = normalize(frame.light_direction.xyz);
    float n_dot_l = saturate(dot(s.n, l));
    float3 radiance = frame.light_color.rgb * frame.light_direction.w;
    float ambient = frame.light_color.w;
    int cascade = -1;
    float shadow = shadow_at(world_position, s.geometric_n, l, shadows, shadow_map0, shadow_map1, shadow_map2,
                             shadow_map3, shadow_sampler, cascade);
    uint cell = cluster_cell(window_position, view_depth, clusters);
    uint cell_lights = cells[cell].count;

    int mode = int(frame.params.x);
    int debug_view = int(frame.params.y);
    float3 color;
    if (debug_view == 1) {
        color = s.n * 0.5 + 0.5;
    } else if (debug_view == 2) {
        color = float3(s.metallic, s.roughness, 0.0);
    } else if (debug_view == 3) {
        color = float3(s.occlusion);
    } else if (debug_view == 4) {
        color = s.geometric_n * 0.5 + 0.5;
    } else if (debug_view == 5) {
        color = s.t * 0.5 + 0.5;
    } else if (debug_view == 7) {
        // How many lights the pixel's cell holds: black, blue, green, yellow, red for 0, 4, 8, 12, 16+.
        float heat = min(float(cell_lights) / 16.0, 1.0) * 4.0;
        float3 stops[5] = {float3(0.0), float3(0.0, 0.2, 1.0), float3(0.0, 1.0, 0.2), float3(1.0, 1.0, 0.0),
                           float3(1.0, 0.1, 0.0)};
        int stop = int(heat);
        color = mix(stops[stop], stops[min(stop + 1, 4)], heat - float(stop));
    } else if (mode == 0) {
        color = s.albedo;
    } else {
        // The sun, shadowed; then the cell's point lights, each an inverse
        // square windowed to nothing at its radius (Karis, 2013); then a
        // uniform grey environment standing in for image-based lighting.
        float3 sun_brdf = surface_brdf(mode, s.n, v, l, s.albedo, s.metallic, s.roughness);
        float3 sun = sun_brdf * radiance * n_dot_l * shadow;
        float3 local = float3(0.0);
        for (uint k = 0; k < cell_lights; ++k) {
            PointLight light = lights[cells[cell].indices[k]];
            float3 to_light = light.position_radius.xyz - world_position;
            float d2 = dot(to_light, to_light);
            float r = light.position_radius.w;
            if (d2 >= r * r) {
                continue;
            }
            float3 ll = to_light * rsqrt(d2);
            float ratio = d2 / (r * r);
            float window = saturate(1.0 - ratio * ratio);
            float attenuation = window * window / (d2 + 0.01);
            float3 brdf = surface_brdf(mode, s.n, v, ll, s.albedo, s.metallic, s.roughness);
            local += brdf * light.color.rgb * attenuation * saturate(dot(s.n, ll));
        }
        float3 f0 = mix(float3(0.04), s.albedo, s.metallic);
        float3 indirect = mode == 1 ? s.albedo * ambient * s.occlusion
                                    : (s.albedo * (1.0 - s.metallic) + f0 * 0.5) * ambient * s.occlusion;
        color = sun + local + indirect + s.emissive;
    }
    if (debug_view == 6) {
        // Which cascade shadowed the pixel: red, green, blue, yellow; grey for none.
        float3 tints[5] = {float3(1.0, 0.2, 0.2), float3(0.2, 1.0, 0.2), float3(0.2, 0.4, 1.0),
                           float3(1.0, 1.0, 0.2), float3(0.5)};
        color = mix(color, tints[cascade < 0 ? 4 : cascade], 0.4);
    }
    return color;
}
)";

// Forward: the material and the lighting in one fragment, linear light out
// into an HDR target; the tonemap pass makes it a picture.
constexpr const char* kMslForwardFragment = R"(
fragment float4 fs_main(VSOut in [[stage_in]],
                        texture2d<float> base_color_map [[texture(0)]],
                        texture2d<float> metallic_roughness_map [[texture(1)]],
                        texture2d<float> occlusion_map [[texture(2)]],
                        texture2d<float> emissive_map [[texture(3)]],
                        texture2d<float> normal_map [[texture(4)]],
                        depth2d<float> shadow_map0 [[texture(5)]],
                        depth2d<float> shadow_map1 [[texture(6)]],
                        depth2d<float> shadow_map2 [[texture(7)]],
                        depth2d<float> shadow_map3 [[texture(8)]],
                        sampler map_sampler [[sampler(0)]],
                        sampler shadow_sampler [[sampler(5)]],
                        constant FrameUniforms& frame [[buffer(0)]],
                        constant MaterialUniforms& m [[buffer(1)]],
                        constant ShadowUniforms& shadows [[buffer(2)]],
                        constant ClusterUniforms& clusters [[buffer(3)]],
                        const device PointLight* lights [[buffer(4)]],
                        const device ClusterLights* cells [[buffer(5)]]) {
    Surface s = sample_surface(in, base_color_map, metallic_roughness_map, occlusion_map, emissive_map,
                               normal_map, map_sampler, m);
    float view_depth = dot(in.world_position - frame.camera_position.xyz, frame.camera_forward.xyz);
    float3 color = shade(s, in.world_position, in.position.xy, view_depth, frame, shadows, clusters, lights,
                         cells, shadow_map0, shadow_map1, shadow_map2, shadow_map3, shadow_sampler);
    return float4(color, s.alpha);
}
)";

// Deferred, first half: the material into the G-buffer — emissive straight
// into the HDR target, albedo and metallic, the shading normal and
// roughness, the view depth and occlusion — 20 bytes a pixel that, in the
// fused pass, never leave the tile.
constexpr const char* kMslGBufferFragment = R"(
struct GBufferOut {
    float4 hdr [[color(0)]];
    float4 albedo [[color(1)]];
    float4 normal [[color(2)]];
    float2 depth [[color(3)]];
};

fragment GBufferOut fs_main(VSOut in [[stage_in]],
                            texture2d<float> base_color_map [[texture(0)]],
                            texture2d<float> metallic_roughness_map [[texture(1)]],
                            texture2d<float> occlusion_map [[texture(2)]],
                            texture2d<float> emissive_map [[texture(3)]],
                            texture2d<float> normal_map [[texture(4)]],
                            sampler map_sampler [[sampler(0)]],
                            constant FrameUniforms& frame [[buffer(0)]],
                            constant MaterialUniforms& m [[buffer(1)]]) {
    Surface s = sample_surface(in, base_color_map, metallic_roughness_map, occlusion_map, emissive_map,
                               normal_map, map_sampler, m);
    float view_depth = dot(in.world_position - frame.camera_position.xyz, frame.camera_forward.xyz);
    GBufferOut out;
    out.hdr = float4(s.emissive, 1.0);
    out.albedo = float4(s.albedo, s.metallic);
    out.normal = float4(s.n, s.roughness);
    out.depth = float2(view_depth, s.occlusion);
    return out;
}
)";

// Deferred, second half: a pixel put back into the world from its view
// depth and lit. Shared by the fused and the split lighting pass; only
// where the G-buffer comes from differs.
constexpr const char* kMslDeferredLighting = R"(
struct VSOut {
    float4 position [[position]];
    float2 uv;
};

vertex VSOut vs_main(uint vid [[vertex_id]]) {
    float2 corner = float2((vid << 1) & 2, vid & 2);
    VSOut out;
    out.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    out.uv = float2(corner.x, 1.0 - corner.y);
    return out;
}

float4 light_pixel(float4 hdr, float4 albedo, float4 normal, float2 depth, float2 window_position,
                   constant FrameUniforms& frame, constant ShadowUniforms& shadows,
                   constant ClusterUniforms& clusters, constant DeferredUniforms& deferred,
                   const device PointLight* lights, const device ClusterLights* cells,
                   depth2d<float> shadow_map0, depth2d<float> shadow_map1, depth2d<float> shadow_map2,
                   depth2d<float> shadow_map3, sampler shadow_sampler) {
    float view_depth = depth.x;
    if (view_depth <= 0.0) {
        return hdr; // the sky: nothing was drawn here
    }
    // Back into the world: the pixel's ray at its depth, in view space, then out.
    float2 ndc = float2(window_position.x / deferred.params.z * 2.0 - 1.0,
                        1.0 - window_position.y / deferred.params.w * 2.0);
    float3 view = float3(ndc.x * deferred.params.x * view_depth, ndc.y * deferred.params.y * view_depth,
                         -view_depth);
    float3 world_position = (deferred.inverse_view * float4(view, 1.0)).xyz;
    Surface s;
    s.albedo = albedo.rgb;
    s.metallic = albedo.a;
    s.n = normalize(normal.xyz);
    s.geometric_n = s.n;
    s.t = float3(0.0);
    s.roughness = normal.a;
    s.occlusion = depth.y;
    s.emissive = float3(0.0); // already in the HDR target
    s.alpha = 1.0;
    float3 color = shade(s, world_position, window_position, view_depth, frame, shadows, clusters, lights,
                         cells, shadow_map0, shadow_map1, shadow_map2, shadow_map3, shadow_sampler);
    return float4(hdr.rgb + color, 1.0);
}
)";

// Fused: the G-buffer read straight from the tile through [[color(n)]]
// inputs, in the same render pass that drew it. The attachments it reads
// are never stored, never sampled, never in memory at all.
constexpr const char* kMslFusedLightingFragment = R"(
struct GBufferIn {
    float4 hdr [[color(0)]];
    float4 albedo [[color(1)]];
    float4 normal [[color(2)]];
    float2 depth [[color(3)]];
};

fragment float4 fs_main(VSOut in [[stage_in]], GBufferIn g,
                        depth2d<float> shadow_map0 [[texture(0)]],
                        depth2d<float> shadow_map1 [[texture(1)]],
                        depth2d<float> shadow_map2 [[texture(2)]],
                        depth2d<float> shadow_map3 [[texture(3)]],
                        sampler shadow_sampler [[sampler(0)]],
                        constant FrameUniforms& frame [[buffer(0)]],
                        constant ShadowUniforms& shadows [[buffer(1)]],
                        constant ClusterUniforms& clusters [[buffer(2)]],
                        constant DeferredUniforms& deferred [[buffer(3)]],
                        const device PointLight* lights [[buffer(4)]],
                        const device ClusterLights* cells [[buffer(5)]]) {
    return light_pixel(g.hdr, g.albedo, g.normal, g.depth, in.position.xy, frame, shadows, clusters, deferred,
                       lights, cells, shadow_map0, shadow_map1, shadow_map2, shadow_map3, shadow_sampler);
}
)";

// Split: the G-buffer stored by one pass and sampled by the next — what
// every deferred renderer does on a GPU with no tile memory, and what the
// fused pass is measured against.
constexpr const char* kMslSplitLightingFragment = R"(
fragment float4 fs_main(VSOut in [[stage_in]],
                        depth2d<float> shadow_map0 [[texture(0)]],
                        depth2d<float> shadow_map1 [[texture(1)]],
                        depth2d<float> shadow_map2 [[texture(2)]],
                        depth2d<float> shadow_map3 [[texture(3)]],
                        texture2d<float> g_hdr [[texture(4)]],
                        texture2d<float> g_albedo [[texture(5)]],
                        texture2d<float> g_normal [[texture(6)]],
                        texture2d<float> g_depth [[texture(7)]],
                        sampler shadow_sampler [[sampler(0)]],
                        sampler point_sampler [[sampler(4)]],
                        constant FrameUniforms& frame [[buffer(0)]],
                        constant ShadowUniforms& shadows [[buffer(1)]],
                        constant ClusterUniforms& clusters [[buffer(2)]],
                        constant DeferredUniforms& deferred [[buffer(3)]],
                        const device PointLight* lights [[buffer(4)]],
                        const device ClusterLights* cells [[buffer(5)]]) {
    return light_pixel(g_hdr.sample(point_sampler, in.uv), g_albedo.sample(point_sampler, in.uv),
                       g_normal.sample(point_sampler, in.uv), g_depth.sample(point_sampler, in.uv).xy,
                       in.position.xy, frame, shadows, clusters, deferred, lights, cells, shadow_map0,
                       shadow_map1, shadow_map2, shadow_map3, shadow_sampler);
}
)";

// The shadow map pass: the casters' depth from the light and nothing else.
// The pipeline has no color target, so the fragment function returns none.
constexpr const char* kShadowMsl = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
};

struct Uniforms {
    float4x4 mvp;
    float4x4 model;
};

vertex float4 vs_main(VertexIn in [[stage_in]], constant Uniforms& u [[buffer(0)]]) {
    return u.mvp * float4(in.position, 1.0);
}

fragment void fs_main() {}
)";

} // namespace tynima::render::shaders
