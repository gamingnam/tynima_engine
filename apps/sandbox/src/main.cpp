// Engine developer's playground. Phase 1: a glTF model through the importer
// with its base color texture, a reverse-Z depth buffer, an sRGB swapchain,
// and a fly camera. Falls back to Phase 0's triangle when there is no model.
//
//   tynima-sandbox [--headless] [--frames N] [--model path.glb]
//
// Controls: hold the right mouse button to look; W/A/S/D move, Q/E descend
// and climb, Shift runs; Escape quits.
#include <tynima/assets/gltf.h>
#include <tynima/core/profile.h>
#include <tynima/core/version.h>
#include <tynima/platform/events.h>
#include <tynima/platform/input.h>
#include <tynima/platform/platform.h>
#include <tynima/platform/time.h>
#include <tynima/platform/window.h>
#include <tynima/render/camera.h>
#include <tynima/render/mesh.h>
#include <tynima/render/model.h>
#include <tynima/rhi/device.h>

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
using namespace tynima::math;

namespace {

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
        std::printf("shading %s | view %s | tonemap %s | light az %.0f el %.0f\n", kModels[model], kViews[debug_view],
                    tonemap ? "aces" : "off", static_cast<double>(azimuth),
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
    rhi::GraphicsPipeline* mesh_pipeline = nullptr;
    rhi::GraphicsPipeline* triangle_pipeline = nullptr;
    rhi::Texture* depth = nullptr;
    render::FallbackTextures fallbacks;
    rhi::Sampler* sampler = nullptr;
    render::Model model;

    Renderer() = default;
    Renderer(Renderer&& other) noexcept
        : device(std::move(other.device)), mesh_pipeline(std::exchange(other.mesh_pipeline, nullptr)),
          triangle_pipeline(std::exchange(other.triangle_pipeline, nullptr)), depth(std::exchange(other.depth, nullptr)),
          fallbacks(std::exchange(other.fallbacks, render::FallbackTextures{})),
          sampler(std::exchange(other.sampler, nullptr)), model(std::exchange(other.model, render::Model{})) {}
    Renderer& operator=(Renderer&&) = delete;
    ~Renderer() { destroy(); }

    // The depth texture tracks the swapchain size: recreate it when that changes.
    rhi::Texture* depth_for(std::uint32_t width, std::uint32_t height) {
        if (depth != nullptr) {
            const rhi::Extent2D extent = device->texture_extent(*depth);
            if (extent.width == width && extent.height == height) {
                return depth;
            }
            device->destroy_texture(depth);
            depth = nullptr;
        }
        depth = device->create_texture({.format = device->preferred_depth_format(), .width = width, .height = height});
        if (depth == nullptr) {
            std::fprintf(stderr, "gpu     depth texture failed: %s\n", platform::last_error());
        }
        return depth;
    }

    // GPU objects go before their device, and the device before the window.
    void destroy() noexcept {
        if (device != nullptr) {
            render::destroy_model(*device, model);
            device->destroy_sampler(sampler);
            render::destroy_fallback_textures(*device, fallbacks);
            device->destroy_graphics_pipeline(mesh_pipeline);
            device->destroy_graphics_pipeline(triangle_pipeline);
            device->destroy_texture(depth);
            mesh_pipeline = triangle_pipeline = nullptr;
            depth = nullptr;
            sampler = nullptr;
            device.reset();
        }
    }
};

rhi::GraphicsPipeline* make_pipeline(rhi::Device& device, const char* msl, const rhi::GraphicsPipelineDesc& base,
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

    rhi::Shader* vs = device.create_shader(vs_desc);
    rhi::Shader* fs = device.create_shader(fs_desc);
    rhi::GraphicsPipeline* pipeline = nullptr;
    if (vs != nullptr && fs != nullptr) {
        rhi::GraphicsPipelineDesc desc = base;
        desc.vertex_shader = vs;
        desc.fragment_shader = fs;
        pipeline = device.create_graphics_pipeline(desc);
    }
    if (pipeline == nullptr) {
        std::fprintf(stderr, "gpu     pipeline failed: %s\n", platform::last_error());
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
        std::fprintf(stderr, "gpu     unavailable: %s\n", platform::last_error());
        return r;
    }
    std::printf("gpu     %s, wants %s shaders, depth %s\n", r.device->backend_name(),
                rhi::shader_format_name(r.device->shader_format()),
                rhi::texture_format_name(r.device->preferred_depth_format()));
    if (!r.device->attach_window(window)) {
        std::fprintf(stderr, "gpu     cannot present to this window: %s\n", platform::last_error());
        r.device.reset();
        return r;
    }
    std::printf("swap    %s\n", r.device->swapchain_is_linear() ? "sRGB-encoded by the display hardware"
                                                                : "plain SDR; the shader encodes sRGB itself");
    if (r.device->shader_format() != rhi::ShaderFormat::Msl) {
        std::fprintf(stderr, "gpu     the sandbox only carries MSL until SDL_shadercross lands; drawing nothing\n");
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
    if (!render::create_fallback_textures(*r.device, r.fallbacks) || r.sampler == nullptr) {
        std::fprintf(stderr, "gpu     fallback textures or sampler failed: %s\n", platform::last_error());
        return r;
    }
    if (model_data != nullptr) {
        const double t0 = platform::now_seconds();
        if (!render::upload_model(*r.device, *model_data, r.fallbacks, r.model)) {
            std::fprintf(stderr, "gpu     model upload failed: %s\n", platform::last_error());
        } else {
            std::size_t uploaded = 0;
            for (const rhi::Texture* t : r.model.textures) {
                uploaded += t != nullptr ? 1 : 0;
            }
            std::printf("gpu     %zu of %zu textures uploaded with mipmaps in %.2f s\n", uploaded,
                        r.model.textures.size(), platform::now_seconds() - t0);
        }
    }
    return r;
}

void report_edges(const platform::Input& input) {
    for (std::size_t i = 0; i < platform::kKeyCount; ++i) {
        const auto key = static_cast<platform::Key>(i);
        if (input.key_pressed(key)) {
            std::printf("key     %s\n", platform::key_name(key));
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    TY_PROFILE_THREAD("main");
    const Options options = parse_options(argc, argv);

    if (!platform::init({.headless = options.headless})) {
        std::fprintf(stderr, "platform init failed: %s\n", platform::last_error());
        return 1;
    }

    auto window = platform::Window::create({.title = "tynima sandbox", .width = 1280, .height = 720});
    if (window == nullptr) {
        std::fprintf(stderr, "window creation failed: %s\n", platform::last_error());
        platform::shutdown();
        return 1;
    }

    std::printf("tynima sandbox - engine %s\n", tynima::core::version_string());
    std::printf("window  %dx%d points, %dx%d pixels, density %.2f%s\n", window->width(), window->height(),
                window->pixel_width(), window->pixel_height(), static_cast<double>(window->pixel_density()),
                options.headless ? " (headless)" : "");
    std::printf("profile %s\n", tynima::core::profiling_compiled()
                                     ? "tracy instrumentation compiled in; connect the Tracy GUI to 127.0.0.1"
                                     : "off (TYNIMA_PROFILE=OFF)");

    // The model loads on the CPU in every mode, so the headless run covers the importer too.
    render::ModelData model_data;
    std::string import_error;
    const double import_start = platform::now_seconds();
    const bool has_model = assets::import_gltf_file(options.model.c_str(), model_data, import_error);
    const render::MeshData& mesh_data = model_data.mesh;
    if (has_model) {
        const Vec3 size = mesh_data.bounds_max - mesh_data.bounds_min;
        std::printf("model   %s: %zu vertices, %zu triangles, %zu submeshes, %.3f x %.3f x %.3f m\n",
                    options.model.c_str(), mesh_data.vertices.size(), mesh_data.indices.size() / 3,
                    mesh_data.submeshes.size(), static_cast<double>(size.x), static_cast<double>(size.y),
                    static_cast<double>(size.z));
        std::printf("model   %zu materials, %zu images decoded in %.2f s", model_data.materials.size(),
                    model_data.images.size(), platform::now_seconds() - import_start);
        for (const render::ImageData& image : model_data.images) {
            std::printf(" [%ux%u%s]", image.width, image.height, image.srgb ? " sRGB" : "");
        }
        std::printf("\n");
    } else {
        std::fprintf(stderr, "model   %s — showing the triangle instead\n", import_error.c_str());
    }
    std::printf("controls right-drag looks, WASD/QE move, Shift runs, Escape quits\n"
                "        1/2/3 unlit / Blinn-Phong / Cook-Torrance, N/M/O/V/B debug views, T tonemap, arrows move the light\n");

    Renderer renderer = options.headless ? Renderer{} : create_renderer(*window, has_model ? &model_data : nullptr);
    bool reported_swapchain = false;

    Shading shading;
    FlyCamera fly;
    if (has_model) {
        const Vec3 center = (mesh_data.bounds_min + mesh_data.bounds_max) * 0.5f;
        fly.frame(center, length(mesh_data.bounds_max - mesh_data.bounds_min) * 0.5f);
    }

    platform::Input input;
    std::vector<platform::Event> events;
    long frame_count = 0;
    double last_time = platform::now_seconds();
    double last_report = last_time;
    long frames_since_report = 0;
    bool running = true;

    while (running) {
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
                std::printf("resize  %dx%d points, %dx%d pixels\n", event.width, event.height, event.pixel_width,
                            event.pixel_height);
                break;
            case platform::EventType::WindowFocusGained:
            case platform::EventType::WindowFocusLost:
                break;
            }
        }
        if (input.key_pressed(platform::Key::Escape)) {
            running = false;
        }
        report_edges(input);

        const double now = platform::now_seconds();
        const float dt = static_cast<float>(std::min(now - last_time, 0.1)); // clamp hitches
        last_time = now;
        fly.update(input, *window, dt);
        shading.update(input, dt);

        if (renderer.device != nullptr) {
            TY_PROFILE_SCOPE_NAMED("render");
            // begin_frame() blocks for vsync, which is what paces the loop.
            if (auto frame = renderer.device->begin_frame()) {
                if (frame->has_swapchain_image()) {
                    if (!reported_swapchain) {
                        std::printf("swap    %ux%u pixels\n", frame->width(), frame->height());
                        reported_swapchain = true;
                    }
                    rhi::Texture* depth = renderer.depth_for(frame->width(), frame->height());
                    if (auto pass = frame->begin_swapchain_pass({.r = 0.09f, .g = 0.10f, .b = 0.12f}, depth, 0.0f)) {
                        if (renderer.model.mesh.index_count > 0 && renderer.mesh_pipeline != nullptr) {
                            const float aspect = static_cast<float>(frame->width()) / static_cast<float>(frame->height());
                            const MeshUniforms uniforms{fly.camera.view_projection(aspect), Mat4::identity()};
                            pass->bind_pipeline(*renderer.mesh_pipeline);
                            pass->push_vertex_uniforms(0, &uniforms, sizeof(uniforms));
                            render::bind_mesh(*pass, renderer.model.mesh);
                            const Vec3 light = shading.light_direction();
                            const FrameUniforms frame_uniforms{
                                {fly.camera.position, 1.0f},
                                {light, shading.light_intensity},
                                {1.0f, 0.97f, 0.92f, shading.ambient},
                                {renderer.device->swapchain_is_linear() ? 0.0f : 1.0f, static_cast<float>(shading.model),
                                 static_cast<float>(shading.debug_view), shading.tonemap ? 1.0f : 0.0f}};
                            pass->push_fragment_uniforms(0, &frame_uniforms, sizeof(frame_uniforms));
                            for (const render::Submesh& sub : renderer.model.mesh.submeshes) {
                                const render::Material& material = renderer.model.materials[sub.material];
                                const MaterialUniforms material_uniforms{
                                    material.base_color_factor,
                                    {material.metallic_factor, material.roughness_factor, material.occlusion_strength,
                                     material.normal_scale},
                                    {material.emissive_factor, 0.0f}};
                                pass->bind_fragment_texture(0, *material.base_color, *renderer.sampler);
                                pass->bind_fragment_texture(1, *material.metallic_roughness, *renderer.sampler);
                                pass->bind_fragment_texture(2, *material.occlusion, *renderer.sampler);
                                pass->bind_fragment_texture(3, *material.emissive, *renderer.sampler);
                                pass->bind_fragment_texture(4, *material.normal, *renderer.sampler);
                                pass->push_fragment_uniforms(1, &material_uniforms, sizeof(material_uniforms));
                                pass->draw_indexed(sub.index_count, sub.first_index);
                            }
                        } else if (renderer.triangle_pipeline != nullptr) {
                            pass->bind_pipeline(*renderer.triangle_pipeline);
                            pass->draw(3);
                        }
                        pass->end();
                    }
                }
                frame->submit();
            } else {
                std::fprintf(stderr, "gpu     frame failed: %s\n", platform::last_error());
                running = false;
            }
        } else {
            // Nothing to draw and nothing to wait on: pace the loop by hand.
            platform::sleep_ns(4'000'000);
        }

        TY_PROFILE_FRAME();
        ++frame_count;
        ++frames_since_report;
        if (now - last_report >= 5.0) {
            std::printf("%.0f frames/s\n", static_cast<double>(frames_since_report) / (now - last_report));
            last_report = now;
            frames_since_report = 0;
        }
        if (options.max_frames >= 0 && frame_count >= options.max_frames) {
            running = false;
        }
    }

    std::printf("ran %ld frames\n", frame_count);
    renderer.destroy(); // GPU objects go before the window they present to
    window.reset();
    platform::shutdown();
    return 0;
}
