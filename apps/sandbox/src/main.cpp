// Engine developer's playground. A pile of glTF models dropped on a floor
// by the physics world, as entities in the scene World, lit with
// Cook-Torrance, a reverse-Z depth buffer, an sRGB swapchain, and a fly
// camera. Falls back to Phase 0's triangle when there is no model.
//
//   tynima-sandbox [--headless] [--frames N] [--model path.glb]
//
// Controls: hold the right mouse button to look; W/A/S/D move, Q/E descend
// and climb, Shift runs; R drops the pile again; Space (the game module)
// launches it; Escape quits.
#include <tynima/assets/gltf.h>
#include <tynima/core/arena.h>
#include <tynima/core/assert.h>
#include <tynima/core/jobs.h>
#include <tynima/core/log.h>
#include <tynima/core/memory.h>
#include <tynima/core/profile.h>
#include <tynima/core/version.h>
#include <tynima/physics/physics.h>
#include <tynima/platform/events.h>
#include <tynima/platform/input.h>
#include <tynima/platform/platform.h>
#include <tynima/platform/time.h>
#include <tynima/platform/window.h>
#include <tynima/render/camera.h>
#include <tynima/render/mesh.h>
#include <tynima/render/model.h>
#include <tynima/rhi/device.h>
#include <tynima/scene/components.h>
#include <tynima/scene/systems.h>
#include <tynima/scene/world.h>
#include <tynima/sdk/game_module.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace platform = tynima::platform;
namespace rhi = tynima::rhi;
namespace render = tynima::render;
namespace assets = tynima::assets;
namespace scene = tynima::scene;
namespace physics = tynima::physics;
using namespace tynima::math;

namespace {

// Model slots the MeshRenderer component indexes into.
constexpr std::uint32_t kModelBottle = 0;
constexpr std::uint32_t kModelFloor = 1;

constexpr float kFloorHalfWidth = 6.0f;
constexpr float kFloorHalfThickness = 0.25f;

// The pile: the model dropped over a 5x5 footprint in four waves, every one
// shoved sideways, tilted and given a little spin, so they hit each other on
// the way down and end up in a heap rather than in neat standing stacks.
struct Pile {
    static constexpr int kSide = 5;
    static constexpr int kLayers = 4;
    std::vector<scene::Entity> entities;
    std::vector<scene::Transform> rest_poses; // where R puts them back
    float footprint = 1.0f;                   // half the width of the drop area, metres
    float drop_height = 1.0f;                 // the top of the highest wave
};

// Deterministic noise in [0, 1): the same pile drops the same way every run,
// which is what makes the headless run and the determinism work of task 7
// meaningful.
float noise(std::uint32_t n, std::uint32_t salt) {
    std::uint32_t h = n * 2654435761u + salt * 40503u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0x1000000u);
}

// Every body is a box around the mesh's bounds, offset from the entity's
// origin the way the mesh is; the entity's Transform is the body's pose.
Pile populate_pile(scene::World& world, physics::PhysicsWorld& physics, const render::MeshData& mesh) {
    Pile pile;
    const Vec3 size = mesh.bounds_max - mesh.bounds_min;
    const Vec3 half_extents = size * 0.5f;
    const Vec3 center = (mesh.bounds_min + mesh.bounds_max) * 0.5f;
    const float spacing = std::max(size.x, size.z) * 1.4f;
    const float wave_height = size.y * 1.8f;
    const float half = static_cast<float>(Pile::kSide - 1) * 0.5f;
    pile.footprint = (half + 0.5f) * spacing;
    pile.drop_height = size.y * 2.0f + static_cast<float>(Pile::kLayers) * wave_height;
    for (int layer = 0; layer < Pile::kLayers; ++layer) {
        for (int z = 0; z < Pile::kSide; ++z) {
            for (int x = 0; x < Pile::kSide; ++x) {
                const auto n = static_cast<std::uint32_t>((layer * Pile::kSide + z) * Pile::kSide + x);
                // Up to nearly half a slot sideways: a bottle often lands on
                // the edge of the one below, or between two, and topples.
                const float shove_x = (noise(n, 1) - 0.5f) * spacing * 0.9f;
                const float shove_z = (noise(n, 2) - 0.5f) * spacing * 0.9f;
                const float lift = noise(n, 3) * wave_height * 0.5f; // no two in a column arrive together
                const Vec3 position{(static_cast<float>(x) - half) * spacing + shove_x,
                                    size.y * 2.0f + static_cast<float>(layer) * wave_height + lift -
                                        mesh.bounds_min.y,
                                    (static_cast<float>(z) - half) * spacing + shove_z};
                // A tilt of up to ~30 degrees about a random horizontal axis, then a random yaw.
                const float tilt_direction = noise(n, 4) * kTwoPi;
                const Vec3 tilt_axis{std::cos(tilt_direction), 0.0f, std::sin(tilt_direction)};
                const Quat rotation = Quat::from_axis_angle(Vec3::unit_y(), noise(n, 5) * kTwoPi) *
                                      Quat::from_axis_angle(tilt_axis, (noise(n, 6) - 0.5f) * 1.0f);
                physics::BodyDesc body;
                body.shape = physics::Shape::box(half_extents, center);
                body.position = position;
                body.rotation = rotation;
                body.angular_velocity =
                    Vec3{noise(n, 7) - 0.5f, noise(n, 8) - 0.5f, noise(n, 9) - 0.5f} * 3.0f; // a little spin
                body.mass = 0.6f; // a full 0.6 l bottle
                body.friction = 0.5f;
                body.restitution = 0.1f;
                const physics::BodyHandle handle = physics.create_body(body);
                const scene::Transform pose{.position = position, .rotation = rotation};
                const scene::Entity entity = world.create(pose, scene::LocalToWorld{},
                                                          scene::MeshRenderer{.model = kModelBottle},
                                                          scene::RigidBody{handle});
                pile.entities.push_back(entity);
                pile.rest_poses.push_back(pose);
            }
        }
    }
    return pile;
}

// Puts every body back where the pile started and lets it fall again, with
// the same spins as the first time: R replays the drop exactly.
void reset_pile(scene::World& world, physics::PhysicsWorld& physics, const Pile& pile) {
    for (std::size_t i = 0; i < pile.entities.size(); ++i) {
        if (const auto* body = world.get<scene::RigidBody>(pile.entities[i])) {
            const auto n = static_cast<std::uint32_t>(i);
            physics.set_velocity(body->body, Vec3{0.0f},
                                 Vec3{noise(n, 7) - 0.5f, noise(n, 8) - 0.5f, noise(n, 9) - 0.5f} * 3.0f);
            physics.set_transform(body->body, pile.rest_poses[i].position, pile.rest_poses[i].rotation);
        }
    }
}

// How the pile ended up: how many bottles still stand, how far it spread,
// how high it is. One line to compare a run against another — the same
// scene through our own solver later must come out about the same.
void report_pile(scene::World& world, const Pile& pile) {
    std::uint32_t upright = 0, toppled = 0;
    float spread = 0.0f, top = 0.0f;
    for (const scene::Entity entity : pile.entities) {
        const auto* transform = world.get<scene::Transform>(entity);
        if (transform == nullptr) {
            continue;
        }
        const Vec3 up = transform->rotation.rotate(Vec3::unit_y());
        (up.y > 0.7f ? upright : toppled)++;
        spread = std::max(spread, std::sqrt(transform->position.x * transform->position.x +
                                            transform->position.z * transform->position.z));
        top = std::max(top, transform->position.y);
    }
    TY_LOG_INFO("scene", "pile: %u upright, %u toppled, spread %.2f m, top at %.2f m", upright, toppled,
                static_cast<double>(spread), static_cast<double>(top));
}

// A closed box with hard edges: 24 vertices, 6 quads, UVs per face, tangents
// from those. The floor is one of these, stretched.
render::MeshData make_box_mesh(const Vec3& half_extents) {
    render::MeshData mesh;
    const Vec3 h = half_extents;
    struct Face {
        Vec3 normal, u_axis, v_axis;
    };
    const Face faces[6] = {
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},  // top
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},  // bottom
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},  // +x
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},  // -x
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},   // +z
        {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}, // -z
    };
    for (const Face& face : faces) {
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        // Each axis is a signed unit vector: scaling it component-wise by the
        // half extents gives the face centre and its two half-edges.
        const Vec3 origin = face.normal * h;
        const Vec3 du = face.u_axis * h;
        const Vec3 dv = face.v_axis * h;
        const float tile = 2.0f; // UV repeats per metre, so a big floor still shows texture
        const Vec2 uv_scale{length(du) * tile, length(dv) * tile};
        for (int corner = 0; corner < 4; ++corner) {
            const float su = (corner == 1 || corner == 2) ? 1.0f : -1.0f;
            const float sv = (corner >= 2) ? 1.0f : -1.0f;
            const Vec2 uv{(su + 1.0f) * 0.5f * uv_scale.x, (1.0f - sv) * 0.5f * uv_scale.y};
            mesh.vertices.push_back({.position = origin + du * su + dv * sv,
                                     .normal = face.normal,
                                     .uv = uv,
                                     .tangent = Vec4{face.u_axis, 1.0f}});
        }
        for (const std::uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) {
            mesh.indices.push_back(base + i);
        }
    }
    mesh.submeshes.push_back(
        {.first_index = 0, .index_count = static_cast<std::uint32_t>(mesh.indices.size()), .material = 0});
    mesh.compute_bounds();
    return mesh;
}

render::ModelData make_floor_model() {
    render::ModelData floor;
    floor.mesh = make_box_mesh(Vec3{kFloorHalfWidth, kFloorHalfThickness, kFloorHalfWidth});
    render::MaterialData material;
    material.base_color_factor = Vec4{0.42f, 0.42f, 0.40f, 1.0f};
    material.metallic_factor = 0.0f;
    material.roughness_factor = 0.85f;
    floor.materials.push_back(material);
    return floor;
}

#ifndef TYNIMA_SANDBOX_GAME_MODULE
#define TYNIMA_SANDBOX_GAME_MODULE ""
#endif

#ifndef NDEBUG
constexpr bool kGpuDebug = true; // Metal validation: catches API misuse loudly
#else
constexpr bool kGpuDebug = false;
#endif

#ifndef TYNIMA_SANDBOX_ASSETS_DIR
#define TYNIMA_SANDBOX_ASSETS_DIR "."
#endif

struct Options {
    bool headless = false;
    long max_frames = -1; // -1: run until closed
    std::string model = TYNIMA_SANDBOX_ASSETS_DIR "/WaterBottle.glb";
};

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") == 0) {
            options.headless = true;
        } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            options.max_frames = std::strtol(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            options.model = argv[++i];
        } else {
            std::fprintf(stderr, "usage: tynima-sandbox [--headless] [--frames N] [--model path.glb]\n");
            std::exit(2);
        }
    }
    return options;
}

// ------------------------------------------------------------------ shaders

// Lit, textured mesh with two shading models and a set of debug views.
// Vertex uniforms at [[buffer(0)]]; attributes come from render::vertex_layout()
// through [[stage_in]]. Fragment uniforms: frame at [[buffer(0)]], material at
// [[buffer(1)]]; the four material maps at [[texture(0..3)]] share sampler 0.
// Every texel is linear by the time it is sampled (sRGB formats decode in
// hardware), lighting happens in linear, and the swapchain encodes on the way
// out. Blinn-Phong is here for comparison; Cook-Torrance is the real thing.
constexpr const char* kMeshMsl = R"(
#include <metal_stdlib>
using namespace metal;

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

struct FrameUniforms {
    float4 camera_position; // xyz
    float4 light_direction; // xyz: towards the light; w: intensity
    float4 light_color;     // rgb; w: ambient intensity
    float4 params;          // x: encode sRGB here, y: shading model, z: debug view, w: tonemap
};

struct MaterialUniforms {
    float4 base_color_factor;
    float4 factors;         // x: metallic, y: roughness, z: occlusion strength, w: normal scale
    float4 emissive_factor; // rgb
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

// Narkowicz's ACES fit: the post stack's job eventually, in the shader for now.
float3 tonemap_aces(float3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

fragment float4 fs_main(VSOut in [[stage_in]],
                        texture2d<float> base_color_map [[texture(0)]],
                        texture2d<float> metallic_roughness_map [[texture(1)]],
                        texture2d<float> occlusion_map [[texture(2)]],
                        texture2d<float> emissive_map [[texture(3)]],
                        texture2d<float> normal_map [[texture(4)]],
                        sampler map_sampler [[sampler(0)]],
                        constant FrameUniforms& frame [[buffer(0)]],
                        constant MaterialUniforms& m [[buffer(1)]]) {
    float4 base = base_color_map.sample(map_sampler, in.uv) * m.base_color_factor;
    float4 mr = metallic_roughness_map.sample(map_sampler, in.uv);
    float metallic = mr.b * m.factors.x;                        // glTF: metallic in B
    float roughness = clamp(mr.g * m.factors.y, 0.045, 1.0);    // roughness in G; 0 would make GGX blow up
    float occlusion = mix(1.0, occlusion_map.sample(map_sampler, in.uv).r, m.factors.z);
    float3 emissive = emissive_map.sample(map_sampler, in.uv).rgb * m.emissive_factor.rgb;

    // Tangent frame: re-orthogonalize the interpolated tangent against the
    // interpolated normal, then the bitangent follows from the handedness.
    float3 geometric_n = normalize(in.world_normal);
    float3 t = in.world_tangent.xyz;
    t = normalize(t - geometric_n * dot(geometric_n, t));
    float3 b = cross(geometric_n, t) * (in.world_tangent.w < 0.0 ? -1.0 : 1.0);
    float3 nm = normal_map.sample(map_sampler, in.uv).xyz * 2.0 - 1.0; // tangent space, +z straight up
    nm.xy *= m.factors.w;
    float3 n = normalize(t * nm.x + b * nm.y + geometric_n * nm.z);

    float3 v = normalize(frame.camera_position.xyz - in.world_position);
    float3 l = normalize(frame.light_direction.xyz);
    float3 h = normalize(l + v);
    float n_dot_l = saturate(dot(n, l));
    float n_dot_v = max(dot(n, v), 1e-4);
    float n_dot_h = saturate(dot(n, h));
    float v_dot_h = saturate(dot(v, h));
    float3 radiance = frame.light_color.rgb * frame.light_direction.w;
    float ambient = frame.light_color.w;

    int mode = int(frame.params.y);
    int debug_view = int(frame.params.z);
    float3 color;
    if (debug_view == 1) {
        color = n * 0.5 + 0.5;
    } else if (debug_view == 2) {
        color = float3(metallic, roughness, 0.0);
    } else if (debug_view == 3) {
        color = float3(occlusion);
    } else if (debug_view == 4) {
        color = geometric_n * 0.5 + 0.5;
    } else if (debug_view == 5) {
        color = t * 0.5 + 0.5;
    } else if (mode == 0) {
        color = base.rgb;
    } else if (mode == 1) {
        // Normalized Blinn-Phong. Shininess is derived from roughness so both
        // models agree on how glossy a surface is; what differs is the shape
        // of the highlight and the missing Fresnel and masking terms.
        float alpha = roughness * roughness;
        float shininess = 2.0 / (alpha * alpha) - 2.0;
        float3 spec_color = mix(float3(0.04), base.rgb, metallic);
        float3 diffuse = base.rgb * (1.0 - metallic) / kPi;
        float3 specular = spec_color * pow(n_dot_h, shininess) * (shininess + 8.0) / (8.0 * kPi);
        color = (diffuse + specular) * radiance * n_dot_l + base.rgb * ambient * occlusion + emissive;
    } else {
        // Cook-Torrance with the metallic workflow: dielectrics reflect 4% at
        // normal incidence and keep their albedo as diffuse; metals reflect
        // their albedo as specular and have no diffuse at all.
        float alpha = roughness * roughness;
        float3 f0 = mix(float3(0.04), base.rgb, metallic);
        float3 diffuse_color = base.rgb * (1.0 - metallic);
        float3 f = f_schlick(v_dot_h, f0);
        float d = d_ggx(n_dot_h, alpha);
        float vis = v_smith_ggx_correlated(n_dot_v, n_dot_l, alpha);
        float3 specular = d * vis * f;
        float3 diffuse = (float3(1.0) - f) * diffuse_color / kPi;
        float3 direct = (diffuse + specular) * radiance * n_dot_l;
        // A uniform grey environment stands in for image-based lighting (Phase 4).
        float3 indirect = (diffuse_color + f0 * 0.5) * ambient * occlusion;
        color = direct + indirect + emissive;
    }
    if (frame.params.w > 0.5 && debug_view == 0) {
        color = tonemap_aces(color);
    }
    if (frame.params.x > 0.5) {
        color = pow(color, float3(1.0 / 2.2));
    }
    return float4(color, base.a);
}
)";

// Phase 0's triangle, kept as the fallback when there is nothing to load.
constexpr const char* kTriangleMsl = R"(
#include <metal_stdlib>
using namespace metal;

struct VSOut {
    float4 position [[position]];
    float4 color;
};

constant float2 kPositions[3] = { float2(-0.6, -0.5), float2(0.6, -0.5), float2(0.0, 0.6) };
constant float3 kColors[3]    = { float3(0.91, 0.64, 0.14), float3(0.29, 0.77, 0.87), float3(0.94, 0.94, 0.96) };

vertex VSOut vs_main(uint vid [[vertex_id]]) {
    VSOut out;
    out.position = float4(kPositions[vid], 0.0, 1.0);
    out.color = float4(kColors[vid], 1.0);
    return out;
}

fragment float4 fs_main(VSOut in [[stage_in]]) {
    return in.color;
}
)";

// Mirrors `struct Uniforms` in kMeshMsl: two column-major float4x4, 128 bytes.
struct MeshUniforms {
    Mat4 mvp;
    Mat4 model;
};
static_assert(sizeof(MeshUniforms) == 128, "matches the MSL Uniforms struct");

// Mirror the MSL uniform structs: float4 members only, so C++ and Metal agree on layout.
struct FrameUniforms {
    Vec4 camera_position;
    Vec4 light_direction;
    Vec4 light_color;
    Vec4 params;
};
static_assert(sizeof(FrameUniforms) == 64, "matches the MSL FrameUniforms struct");

struct MaterialUniforms {
    Vec4 base_color_factor;
    Vec4 factors;
    Vec4 emissive_factor;
};
static_assert(sizeof(MaterialUniforms) == 48, "matches the MSL MaterialUniforms struct");

// What the keys toggle. Printed whenever it changes.
struct Shading {
    int model = 2;      // 0 unlit, 1 Blinn-Phong, 2 Cook-Torrance
    int debug_view = 0; // 0 lit, 1 shading normals, 2 metallic/roughness, 3 occlusion, 4 vertex normals, 5 tangents
    bool tonemap = true;
    float light_azimuth = radians(35.0f);   // around +y, from +z
    float light_elevation = radians(50.0f); // above the horizon
    float light_intensity = 3.0f;           // linear radiance; >1 is what tonemapping is for
    float ambient = 0.10f;

    Vec3 light_direction() const {
        return {std::cos(light_elevation) * std::sin(light_azimuth), std::sin(light_elevation),
                std::cos(light_elevation) * std::cos(light_azimuth)};
    }

    void print() const {
        static constexpr const char* kModels[] = {"unlit", "blinn-phong", "cook-torrance"};
        static constexpr const char* kViews[] = {"lit", "shading normals (mapped)", "metallic (r) / roughness (g)",
                                                 "occlusion", "vertex normals", "tangents"};
        float azimuth = std::fmod(degrees(light_azimuth), 360.0f);
        if (azimuth < 0.0f) azimuth += 360.0f;
        TY_LOG_INFO("shading", "%s | view %s | tonemap %s | light az %.0f el %.0f", kModels[model],
                    kViews[debug_view], tonemap ? "aces" : "off", static_cast<double>(azimuth),
                    static_cast<double>(degrees(light_elevation)));
    }

    // Returns true when something changed.
    bool update(const platform::Input& input, float dt) {
        using platform::Key;
        const int old_model = model, old_view = debug_view;
        const bool old_tonemap = tonemap;
        if (input.key_pressed(Key::Digit1)) model = 0;
        if (input.key_pressed(Key::Digit2)) model = 1;
        if (input.key_pressed(Key::Digit3)) model = 2;
        if (input.key_pressed(Key::N)) debug_view = debug_view == 1 ? 0 : 1;
        if (input.key_pressed(Key::M)) debug_view = debug_view == 2 ? 0 : 2;
        if (input.key_pressed(Key::O)) debug_view = debug_view == 3 ? 0 : 3;
        if (input.key_pressed(Key::V)) debug_view = debug_view == 4 ? 0 : 4;
        if (input.key_pressed(Key::B)) debug_view = debug_view == 5 ? 0 : 5;
        if (input.key_pressed(Key::Digit0)) debug_view = 0;
        if (input.key_pressed(Key::T)) tonemap = !tonemap;
        const float turn = radians(60.0f) * dt;
        bool moved = false;
        if (input.key_down(Key::Left)) { light_azimuth -= turn; moved = true; }
        if (input.key_down(Key::Right)) { light_azimuth += turn; moved = true; }
        if (input.key_down(Key::Up)) { light_elevation = std::min(light_elevation + turn, radians(89.0f)); moved = true; }
        if (input.key_down(Key::Down)) { light_elevation = std::max(light_elevation - turn, radians(-10.0f)); moved = true; }
        const bool changed = model != old_model || debug_view != old_view || tonemap != old_tonemap;
        if (changed) print();
        return changed || moved;
    }
};

// --------------------------------------------------------------- fly camera

struct FlyCamera {
    render::Camera camera;
    float yaw = 0.0f;   // radians about world +y
    float pitch = 0.0f; // radians about local +x
    float speed = 1.0f; // metres per second
    bool looking = false;

    void frame(const Vec3& center, float radius) {
        camera.position = center + Vec3{0.0f, radius * 0.35f, radius * 2.4f};
        camera.look_at(center);
        // Recover yaw/pitch from the resulting forward so mouse-look continues from here.
        const Vec3 f = camera.forward();
        yaw = std::atan2(-f.x, -f.z);
        pitch = std::asin(clamp(f.y, -1.0f, 1.0f));
        camera.near = std::max(radius * 0.02f, 0.005f);
        speed = std::max(radius * 1.5f, 0.2f);
    }

    void update(const platform::Input& input, platform::Window& window, float dt) {
        if (input.mouse_pressed(platform::MouseButton::Right)) {
            looking = true;
            window.set_relative_mouse_mode(true);
        }
        if (input.mouse_released(platform::MouseButton::Right)) {
            looking = false;
            window.set_relative_mouse_mode(false);
        }
        if (looking) {
            constexpr float kSensitivity = 0.0025f; // radians per pixel
            yaw -= input.mouse_dx() * kSensitivity;
            pitch = clamp(pitch - input.mouse_dy() * kSensitivity, radians(-89.0f), radians(89.0f));
        }
        camera.rotation = Quat::from_axis_angle(Vec3::unit_y(), yaw) * Quat::from_axis_angle(Vec3::unit_x(), pitch);

        Vec3 move = Vec3::zero();
        if (input.key_down(platform::Key::W)) move += camera.forward();
        if (input.key_down(platform::Key::S)) move -= camera.forward();
        if (input.key_down(platform::Key::D)) move += camera.right();
        if (input.key_down(platform::Key::A)) move -= camera.right();
        if (input.key_down(platform::Key::E)) move += Vec3::unit_y();
        if (input.key_down(platform::Key::Q)) move -= Vec3::unit_y();
        if (length_squared(move) > 0.0f) {
            const bool run = input.key_down(platform::Key::LeftShift) || input.key_down(platform::Key::RightShift);
            camera.position += normalize(move) * (speed * (run ? 4.0f : 1.0f) * dt);
        }
    }
};

// ----------------------------------------------------------------- renderer

struct Renderer {
    std::unique_ptr<rhi::Device> device;
    rhi::PipelineHandle mesh_pipeline;
    rhi::PipelineHandle triangle_pipeline;
    rhi::TextureHandle depth;
    render::FallbackTextures fallbacks;
    rhi::SamplerHandle sampler;
    render::Model models[2]; // kModelBottle, kModelFloor

    Renderer() = default;
    Renderer(Renderer&& other) noexcept
        : device(std::move(other.device)), mesh_pipeline(std::exchange(other.mesh_pipeline, {})),
          triangle_pipeline(std::exchange(other.triangle_pipeline, {})), depth(std::exchange(other.depth, {})),
          fallbacks(std::exchange(other.fallbacks, render::FallbackTextures{})),
          sampler(std::exchange(other.sampler, {})),
          models{std::exchange(other.models[0], render::Model{}),
                 std::exchange(other.models[1], render::Model{})} {}
    Renderer& operator=(Renderer&&) = delete;
    ~Renderer() { destroy(); }

    // The depth texture tracks the swapchain size: recreate it when that changes.
    rhi::TextureHandle depth_for(std::uint32_t width, std::uint32_t height) {
        if (depth) {
            const rhi::Extent2D extent = device->texture_extent(depth);
            if (extent.width == width && extent.height == height) {
                return depth;
            }
            device->destroy_texture(depth);
            depth = {};
        }
        depth = device->create_texture({.format = device->preferred_depth_format(), .width = width, .height = height});
        if (!depth) {
            TY_LOG_ERROR("gpu", "depth texture failed: %s", platform::last_error());
        }
        return depth;
    }

    // GPU objects go before their device, and the device before the window.
    void destroy() noexcept {
        if (device != nullptr) {
            for (render::Model& model : models) {
                render::destroy_model(*device, model);
            }
            device->destroy_sampler(sampler);
            render::destroy_fallback_textures(*device, fallbacks);
            device->destroy_graphics_pipeline(mesh_pipeline);
            device->destroy_graphics_pipeline(triangle_pipeline);
            device->destroy_texture(depth);
            mesh_pipeline = triangle_pipeline = {};
            depth = {};
            sampler = {};
            device.reset();
        }
    }
};

rhi::PipelineHandle make_pipeline(rhi::Device& device, const char* msl, const rhi::GraphicsPipelineDesc& base,
                                  std::uint32_t vertex_uniforms, std::uint32_t fragment_uniforms,
                                  std::uint32_t fragment_samplers) {
    const rhi::ShaderDesc common{.format = rhi::ShaderFormat::Msl, .code = msl, .code_size = std::strlen(msl)};
    rhi::ShaderDesc vs_desc = common;
    vs_desc.stage = rhi::ShaderStage::Vertex;
    vs_desc.entry_point = "vs_main";
    vs_desc.num_uniform_buffers = vertex_uniforms;
    rhi::ShaderDesc fs_desc = common;
    fs_desc.stage = rhi::ShaderStage::Fragment;
    fs_desc.entry_point = "fs_main";
    fs_desc.num_uniform_buffers = fragment_uniforms;
    fs_desc.num_samplers = fragment_samplers;

    const rhi::ShaderHandle vs = device.create_shader(vs_desc);
    const rhi::ShaderHandle fs = device.create_shader(fs_desc);
    rhi::PipelineHandle pipeline;
    if (vs && fs) {
        rhi::GraphicsPipelineDesc desc = base;
        desc.vertex_shader = vs;
        desc.fragment_shader = fs;
        pipeline = device.create_graphics_pipeline(desc);
    }
    if (!pipeline) {
        TY_LOG_ERROR("gpu", "pipeline failed: %s", platform::last_error());
    }
    device.destroy_shader(vs); // the pipeline holds what it needs
    device.destroy_shader(fs);
    return pipeline;
}

// Everything GPU-side. On failure the sandbox keeps running without drawing,
// and says why — a missing backend is a message, not a crash.
Renderer create_renderer(platform::Window& window, const render::ModelData* model_data) {
    Renderer r;
    r.device = rhi::Device::create({.debug = kGpuDebug});
    if (r.device == nullptr) {
        TY_LOG_ERROR("gpu", "unavailable: %s", platform::last_error());
        return r;
    }
    TY_LOG_INFO("gpu", "%s, wants %s shaders, depth %s", r.device->backend_name(),
                rhi::shader_format_name(r.device->shader_format()),
                rhi::texture_format_name(r.device->preferred_depth_format()));
    if (!r.device->attach_window(window)) {
        TY_LOG_ERROR("gpu", "cannot present to this window: %s", platform::last_error());
        r.device.reset();
        return r;
    }
    TY_LOG_INFO("swap", "%s", r.device->swapchain_is_linear() ? "sRGB-encoded by the display hardware"
                                                             : "plain SDR; the shader encodes sRGB itself");
    if (r.device->shader_format() != rhi::ShaderFormat::Msl) {
        TY_LOG_WARN("gpu", "the sandbox only carries MSL until SDL_shadercross lands; drawing nothing");
        return r;
    }

    rhi::GraphicsPipelineDesc mesh_desc;
    mesh_desc.vertex_layout = render::vertex_layout();
    mesh_desc.cull = rhi::CullMode::Back;
    mesh_desc.depth = {.test = true, .write = true, .compare = rhi::CompareOp::Greater}; // reverse-Z
    mesh_desc.has_depth_target = true;
    r.mesh_pipeline = make_pipeline(*r.device, kMeshMsl, mesh_desc, 1, 2, 5);

    rhi::GraphicsPipelineDesc triangle_desc;
    triangle_desc.has_depth_target = true; // same pass, so the same targets, even with the test off
    r.triangle_pipeline = make_pipeline(*r.device, kTriangleMsl, triangle_desc, 0, 0, 0);

    r.sampler = r.device->create_sampler({.max_anisotropy = 8.0f});
    if (!render::create_fallback_textures(*r.device, r.fallbacks) || !r.sampler) {
        TY_LOG_ERROR("gpu", "fallback textures or sampler failed: %s", platform::last_error());
        return r;
    }
    if (model_data != nullptr) {
        const double t0 = platform::now_seconds();
        if (!render::upload_model(*r.device, *model_data, r.fallbacks, r.models[kModelBottle])) {
            TY_LOG_ERROR("gpu", "model upload failed: %s", platform::last_error());
        } else {
            std::size_t uploaded = 0;
            for (const rhi::TextureHandle t : r.models[kModelBottle].textures) {
                uploaded += t ? 1 : 0;
            }
            TY_LOG_INFO("gpu", "%zu of %zu textures uploaded with mipmaps in %.2f s", uploaded,
                        r.models[kModelBottle].textures.size(), platform::now_seconds() - t0);
        }
        const render::ModelData floor = make_floor_model();
        if (!render::upload_model(*r.device, floor, r.fallbacks, r.models[kModelFloor])) {
            TY_LOG_ERROR("gpu", "floor upload failed: %s", platform::last_error());
        }
    }
    return r;
}

void report_edges(const platform::Input& input) {
    for (std::size_t i = 0; i < platform::kKeyCount; ++i) {
        const auto key = static_cast<platform::Key>(i);
        if (input.key_pressed(key)) {
            TY_LOG_DEBUG("input", "%s", platform::key_name(key));
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    TY_PROFILE_THREAD("main");
    const Options options = parse_options(argc, argv);

    TY_LOG_INFO("sandbox", "engine %s, log level %s", tynima::core::version_string(),
                tynima::core::log_level_name(tynima::core::log_level()));
    if (!platform::init({.headless = options.headless})) {
        TY_LOG_ERROR("platform", "init failed: %s", platform::last_error());
        return 1;
    }

    auto window = platform::Window::create({.title = "tynima sandbox", .width = 1280, .height = 720});
    if (window == nullptr) {
        TY_LOG_ERROR("window", "creation failed: %s", platform::last_error());
        platform::shutdown();
        return 1;
    }

    TY_LOG_INFO("window", "%dx%d points, %dx%d pixels, density %.2f%s", window->width(), window->height(),
                window->pixel_width(), window->pixel_height(), static_cast<double>(window->pixel_density()),
                options.headless ? " (headless)" : "");
    TY_LOG_INFO("profile", "%s", tynima::core::profiling_compiled()
                                     ? "tracy instrumentation compiled in; connect the Tracy GUI to 127.0.0.1"
                                     : "off (TYNIMA_PROFILE=OFF)");

    tynima::core::JobSystem jobs;
    TY_LOG_INFO("jobs", "%u worker thread(s) + main, for %u performance cores", jobs.worker_count(),
                tynima::core::JobSystem::performance_core_count());

    // Physics steps on the same job system; bodies drive entity Transforms.
    auto physics = physics::create_jolt_world({.max_bodies = 1024, .jobs = &jobs});
    TY_LOG_INFO("physics", "%s, %u worker thread(s)", physics->backend_name(), jobs.worker_count());
    {
        physics::BodyDesc floor;
        floor.shape = physics::Shape::box(Vec3{kFloorHalfWidth, kFloorHalfThickness, kFloorHalfWidth});
        floor.position = Vec3{0.0f, -kFloorHalfThickness, 0.0f}; // top face at y = 0
        floor.motion = physics::MotionType::Static;
        floor.friction = 0.6f;
        (void)physics->create_body(floor);
    }

    // The model loads on the CPU in every mode, so the headless run covers the importer too.
    render::ModelData model_data;
    std::string import_error;
    const double import_start = platform::now_seconds();
    const bool has_model =
        assets::import_gltf_file(options.model.c_str(), model_data, import_error, {.jobs = &jobs});
    const render::MeshData& mesh_data = model_data.mesh;
    if (has_model) {
        const Vec3 size = mesh_data.bounds_max - mesh_data.bounds_min;
        TY_LOG_INFO("model", "%s: %zu vertices, %zu triangles, %zu submeshes, %.3f x %.3f x %.3f m",
                    options.model.c_str(), mesh_data.vertices.size(), mesh_data.indices.size() / 3,
                    mesh_data.submeshes.size(), static_cast<double>(size.x), static_cast<double>(size.y),
                    static_cast<double>(size.z));
        TY_LOG_INFO("model", "%zu materials, %zu images decoded in %.2f s", model_data.materials.size(),
                    model_data.images.size(), platform::now_seconds() - import_start);
        for (const render::ImageData& image : model_data.images) {
            TY_LOG_DEBUG("model", "image %ux%u%s", image.width, image.height, image.srgb ? " sRGB" : "");
        }
    } else {
        TY_LOG_WARN("model", "%s - showing the triangle instead", import_error.c_str());
    }
    TY_LOG_INFO("controls", "right-drag looks, WASD/QE move, Shift runs, R re-drops the pile, "
                            "Space launches it, Escape quits");
    TY_LOG_INFO("controls", "1/2/3 unlit / Blinn-Phong / Cook-Torrance, N/M/O/V/B debug views, T tonemap, "
                            "arrows move the light");

    Renderer renderer = options.headless ? Renderer{} : create_renderer(*window, has_model ? &model_data : nullptr);
    bool reported_swapchain = false;

    // The scene: the floor and a pile of the model, every one a rigid body.
    // Systems run over the World each frame; the renderer draws what the
    // World says is there.
    scene::World world(4096);
    Pile pile;
    if (has_model) {
        (void)world.create(scene::Transform{.position = Vec3{0.0f, -kFloorHalfThickness, 0.0f}},
                           scene::LocalToWorld{}, scene::MeshRenderer{.model = kModelFloor});
        pile = populate_pile(world, *physics, mesh_data);
        TY_LOG_INFO("scene", "%u entities in %u archetype(s), %u chunk(s); %u bodies", world.entity_count(),
                    world.archetype_count(), world.chunk_count(), physics->body_count());
    }

    Shading shading;
    FlyCamera fly;
    if (has_model) {
        // Frame where the pile lands, with the falling column in view above it.
        fly.frame(Vec3{0.0f, pile.drop_height * 0.3f, 0.0f},
                  std::max(pile.footprint * 2.2f, pile.drop_height * 0.6f));
    }

    // Per-frame scratch memory: reset at the top of every frame, never freed
    // piecemeal. Nothing uses it yet; the ECS and render packets will.
    tynima::core::Arena frame_arena(4 * 1024 * 1024);

    platform::Input input;
    std::vector<platform::Event> events;
    events.reserve(64); // a frame's worth; growing later would count as a frame allocation

    // The game module owns the systems that make the scene move. It is a
    // shared library, and it is swapped for a new build whenever one appears.
    tynima_engine engine_context;
    engine_context.world = &world;
    engine_context.input = &input;
    engine_context.physics = physics.get();
    tynima::sdk::GameModule game(TYNIMA_SANDBOX_GAME_MODULE);
    TY_LOG_INFO("game", "loading %s", game.path().c_str());
    if (game.load(engine_context)) {
        TY_LOG_INFO("game", "edit apps/sandbox/game/src/game.cpp, then: cmake --build --preset macos-debug "
                            "--target tynima_sandbox_game");
    } else {
        TY_LOG_ERROR("game", "%s - the scene will not move", game.last_error());
    }
    long frame_count = 0;
    double last_time = platform::now_seconds();
    double last_report = last_time;
    long frames_since_report = 0;
    bool running = true;

    while (running) {
        frame_arena.reset();
        const tynima::core::HeapAllocationScope heap_scope;
        {
            TY_PROFILE_SCOPE_NAMED("events");
            platform::pump_events(input, events);
        }
        for (const auto& event : events) {
            switch (event.type) {
            case platform::EventType::Quit:
            case platform::EventType::WindowClose:
                running = false;
                break;
            case platform::EventType::WindowResized:
                TY_LOG_INFO("window", "resized to %dx%d points, %dx%d pixels", event.width, event.height,
                            event.pixel_width, event.pixel_height);
                break;
            case platform::EventType::WindowFocusGained:
            case platform::EventType::WindowFocusLost:
                break;
            }
        }
        if (input.key_pressed(platform::Key::Escape)) {
            running = false;
        }
        if (input.key_pressed(platform::Key::R)) {
            reset_pile(world, *physics, pile);
        }
        report_edges(input);

        const double now = platform::now_seconds();
        const float dt = static_cast<float>(std::min(now - last_time, 0.1)); // clamp hitches
        last_time = now;
        fly.update(input, *window, dt);
        shading.update(input, dt);

        engine_context.time_seconds = now;
        const bool reloaded = game.poll(engine_context); // a reload allocates; that frame is exempt below
        {
            TY_PROFILE_SCOPE_NAMED("systems");
            game.update(engine_context, dt);
            physics->step(dt); // variable steps until task 6 brings the fixed timestep
            scene::update_bodies(world, *physics);
            scene::update_transforms(world);
        }

        if (renderer.device != nullptr) {
            TY_PROFILE_SCOPE_NAMED("render");
            // begin_frame() blocks for vsync, which is what paces the loop.
            if (auto frame = renderer.device->begin_frame()) {
                if (frame->has_swapchain_image()) {
                    if (!reported_swapchain) {
                        TY_LOG_INFO("swap", "%ux%u pixels", frame->width(), frame->height());
                        reported_swapchain = true;
                    }
                    const rhi::TextureHandle depth = renderer.depth_for(frame->width(), frame->height());
                    if (auto pass = frame->begin_swapchain_pass({.r = 0.09f, .g = 0.10f, .b = 0.12f}, depth, 0.0f)) {
                        if (renderer.models[kModelBottle].mesh.index_count > 0 && renderer.mesh_pipeline) {
                            const float aspect = static_cast<float>(frame->width()) / static_cast<float>(frame->height());
                            const Mat4 view_projection = fly.camera.view_projection(aspect);
                            pass->bind_pipeline(renderer.mesh_pipeline);
                            const Vec3 light = shading.light_direction();
                            const FrameUniforms frame_uniforms{
                                {fly.camera.position, 1.0f},
                                {light, shading.light_intensity},
                                {1.0f, 0.97f, 0.92f, shading.ambient},
                                {renderer.device->swapchain_is_linear() ? 0.0f : 1.0f, static_cast<float>(shading.model),
                                 static_cast<float>(shading.debug_view), shading.tonemap ? 1.0f : 0.0f}};
                            pass->push_fragment_uniforms(0, &frame_uniforms, sizeof(frame_uniforms));
                            // One draw per submesh per entity that has something to draw.
                            std::uint32_t bound_model = 0xFFFFFFFFu;
                            world.each<scene::LocalToWorld, scene::MeshRenderer>(
                                [&](scene::Entity, scene::LocalToWorld& local_to_world, scene::MeshRenderer& mr) {
                                    if (!mr.visible || mr.model > kModelFloor ||
                                        renderer.models[mr.model].mesh.index_count == 0) {
                                        return;
                                    }
                                    const render::Model& model = renderer.models[mr.model];
                                    if (bound_model != mr.model) {
                                        render::bind_mesh(*pass, model.mesh);
                                        bound_model = mr.model;
                                    }
                                    const MeshUniforms uniforms{view_projection * local_to_world.matrix,
                                                                local_to_world.matrix};
                                    pass->push_vertex_uniforms(0, &uniforms, sizeof(uniforms));
                                    for (const render::Submesh& sub : model.mesh.submeshes) {
                                        const render::Material& material = model.materials[sub.material];
                                        const MaterialUniforms material_uniforms{
                                            material.base_color_factor,
                                            {material.metallic_factor, material.roughness_factor,
                                             material.occlusion_strength, material.normal_scale},
                                            {material.emissive_factor, 0.0f}};
                                        pass->bind_fragment_texture(0, material.base_color, renderer.sampler);
                                        pass->bind_fragment_texture(1, material.metallic_roughness, renderer.sampler);
                                        pass->bind_fragment_texture(2, material.occlusion, renderer.sampler);
                                        pass->bind_fragment_texture(3, material.emissive, renderer.sampler);
                                        pass->bind_fragment_texture(4, material.normal, renderer.sampler);
                                        pass->push_fragment_uniforms(1, &material_uniforms, sizeof(material_uniforms));
                                        pass->draw_indexed(sub.index_count, sub.first_index);
                                    }
                                });
                        } else if (renderer.triangle_pipeline) {
                            pass->bind_pipeline(renderer.triangle_pipeline);
                            pass->draw(3);
                        }
                        pass->end();
                    }
                }
                frame->submit();
            } else {
                TY_LOG_ERROR("gpu", "frame failed: %s", platform::last_error());
                running = false;
            }
        } else {
            // Nothing to draw and nothing to wait on: pace the loop by hand.
            platform::sleep_ns(4'000'000);
        }

        // The rule from the roadmap, enforced: after warm-up, engine code
        // allocates nothing on the heap during a frame. The GPU driver and the
        // OS allocate plenty inside the calls we make; that is reported, not judged.
        const std::uint64_t engine_allocations = heap_scope.allocations();
        const std::uint64_t external_allocations = heap_scope.total() - engine_allocations;
        TY_PROFILE_PLOT("engine heap allocations / frame", static_cast<std::int64_t>(engine_allocations));
        TY_PROFILE_PLOT("external heap allocations / frame", static_cast<std::int64_t>(external_allocations));
        TY_PROFILE_PLOT("frame arena bytes", static_cast<std::int64_t>(frame_arena.used()));
        if (frame_count == 60) {
            TY_LOG_INFO("heap", "per frame: engine %llu, external (driver, OS, Jolt) %llu",
                        static_cast<unsigned long long>(engine_allocations),
                        static_cast<unsigned long long>(external_allocations));
        }
        if (frame_count >= 10 && engine_allocations > 0 && !reloaded) {
            TY_LOG_ERROR("heap", "frame %ld: engine code made %llu heap allocation(s)", frame_count,
                         static_cast<unsigned long long>(engine_allocations));
            TY_ASSERT(engine_allocations == 0, "a frame allocated on the heap from engine code");
        }

        TY_PROFILE_FRAME();
        ++frame_count;
        ++frames_since_report;
        if (now - last_report >= 5.0) {
            TY_LOG_INFO("sandbox", "%.0f frames/s",
                        static_cast<double>(frames_since_report) / (now - last_report));
            last_report = now;
            frames_since_report = 0;
        }
        if (options.max_frames >= 0 && frame_count >= options.max_frames) {
            running = false;
        }
    }

    TY_LOG_INFO("sandbox", "ran %ld frames, %u bodies awake", frame_count, physics->active_body_count());
    if (has_model) {
        report_pile(world, pile);
    }
    game.unload(engine_context);
    renderer.destroy(); // GPU objects go before the window they present to
    window.reset();
    platform::shutdown();
    return 0;
}
