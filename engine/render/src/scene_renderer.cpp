#include <tynima/core/log.h>
#include <tynima/core/profile.h>
#include <tynima/platform/platform.h>
#include <tynima/render/mesh.h>
#include <tynima/render/scene_renderer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "scene_shaders.h"

namespace tynima::render {

using namespace tynima::math;

namespace {

// The G-buffer: albedo and metallic; the shading normal and roughness; the
// view depth and occlusion. Twenty bytes a pixel beside HDR and depth — on a
// tile-based GPU, twenty bytes that need never exist in memory.
constexpr rhi::TextureFormat kGBufferAlbedoFormat = rhi::TextureFormat::Rgba8Unorm;
constexpr rhi::TextureFormat kGBufferNormalFormat = rhi::TextureFormat::Rgba16Float;
constexpr rhi::TextureFormat kGBufferDepthFormat = rhi::TextureFormat::Rg32Float;

// The shadow lookup is pushed two texels off the surface and one texel
// towards the light against acne.
constexpr float kShadowNormalOffsetTexels = 2.0f;
constexpr float kShadowBiasTexels = 1.0f;
constexpr std::uint32_t kShadowTextureSlot = 5; // after the five material maps
constexpr const char* kShadowPassNames[kMaxCascades] = {"shadow 0", "shadow 1", "shadow 2", "shadow 3"};

// The cluster grid: 16 x 9 tiles, 24 depth slices out to 80 m.
constexpr ClusterGridSettings kClusterSettings{
    .tiles_x = 16, .tiles_y = 9, .slices = 24, .max_distance = 80.0f};

static_assert(sizeof(PointLight) == 32 && sizeof(ClusterLights) == 128);

} // namespace

const char* shading_model_name(ShadingModel model) noexcept {
    switch (model) {
    case ShadingModel::Unlit:
        return "unlit";
    case ShadingModel::BlinnPhong:
        return "blinn-phong";
    case ShadingModel::CookTorrance:
        return "cook-torrance";
    }
    return "?";
}

const char* shading_path_name(ShadingPath path) noexcept {
    switch (path) {
    case ShadingPath::Forward:
        return "forward";
    case ShadingPath::Fused:
        return "fused deferred";
    case ShadingPath::Split:
        return "split deferred";
    }
    return "?";
}

const char* debug_view_name(DebugView view) noexcept {
    switch (view) {
    case DebugView::Lit:
        return "lit";
    case DebugView::Normals:
        return "shading normals (mapped)";
    case DebugView::MetallicRoughness:
        return "metallic (r) / roughness (g)";
    case DebugView::Occlusion:
        return "occlusion";
    case DebugView::VertexNormals:
        return "vertex normals";
    case DebugView::Tangents:
        return "tangents";
    case DebugView::Cascades:
        return "shadow cascades";
    case DebugView::Clusters:
        return "lights per cluster";
    case DebugView::Count:
        break;
    }
    return "?";
}

SceneRenderer::~SceneRenderer() {
    destroy();
}

rhi::PipelineHandle SceneRenderer::make_pipeline(const char* msl, const rhi::GraphicsPipelineDesc& base,
                                                 std::uint32_t vertex_uniforms,
                                                 std::uint32_t fragment_uniforms,
                                                 std::uint32_t fragment_samplers,
                                                 std::uint32_t fragment_storage_buffers) noexcept {
    const rhi::ShaderDesc common{
        .format = rhi::ShaderFormat::Msl, .code = msl, .code_size = std::strlen(msl)};
    rhi::ShaderDesc vs_desc = common;
    vs_desc.stage = rhi::ShaderStage::Vertex;
    vs_desc.entry_point = "vs_main";
    vs_desc.num_uniform_buffers = vertex_uniforms;
    rhi::ShaderDesc fs_desc = common;
    fs_desc.stage = rhi::ShaderStage::Fragment;
    fs_desc.entry_point = "fs_main";
    fs_desc.num_uniform_buffers = fragment_uniforms;
    fs_desc.num_samplers = fragment_samplers;
    fs_desc.num_storage_buffers = fragment_storage_buffers;

    const rhi::ShaderHandle vs = device_->create_shader(vs_desc);
    const rhi::ShaderHandle fs = device_->create_shader(fs_desc);
    rhi::PipelineHandle pipeline;
    if (vs && fs) {
        rhi::GraphicsPipelineDesc desc = base;
        desc.vertex_shader = vs;
        desc.fragment_shader = fs;
        pipeline = device_->create_graphics_pipeline(desc);
    }
    if (!pipeline) {
        TY_LOG_ERROR("render", "pipeline failed: %s", platform::last_error());
    }
    device_->destroy_shader(vs); // the pipeline holds what it needs
    device_->destroy_shader(fs);
    return pipeline;
}

bool SceneRenderer::create(rhi::Device& device, rhi::TextureFormat hdr_format) noexcept {
    static_assert(sizeof(MeshUniforms) == 128, "matches the MSL Uniforms struct");
    static_assert(sizeof(FrameUniforms) == 80, "matches the MSL FrameUniforms struct");
    static_assert(sizeof(MaterialUniforms) == 48, "matches the MSL MaterialUniforms struct");
    static_assert(sizeof(ShadowUniforms) == 336, "matches the MSL ShadowUniforms struct");
    static_assert(sizeof(DeferredUniforms) == 80, "matches the MSL DeferredUniforms struct");
    destroy();
    device_ = &device;
    hdr_format_ = hdr_format;
    depth_format_ = device.preferred_depth_format();
    if (device.shader_format() != rhi::ShaderFormat::Msl) {
        TY_LOG_WARN("render", "the scene carries MSL only until SDL_shadercross lands; drawing nothing");
        return false;
    }
    using namespace shaders;
    const std::string forward_msl =
        std::string(kMslCommon) + kMslMeshVertex + kMslSurface + kMslLighting + kMslForwardFragment;
    const std::string gbuffer_msl =
        std::string(kMslCommon) + kMslMeshVertex + kMslSurface + kMslGBufferFragment;
    const std::string fused_msl =
        std::string(kMslCommon) + kMslLighting + kMslDeferredLighting + kMslFusedLightingFragment;
    const std::string split_msl =
        std::string(kMslCommon) + kMslLighting + kMslDeferredLighting + kMslSplitLightingFragment;

    // Forward: the scene pass draws into HDR with depth.
    rhi::GraphicsPipelineDesc mesh_desc;
    mesh_desc.vertex_layout = vertex_layout();
    mesh_desc.cull = rhi::CullMode::Back;
    mesh_desc.depth = {.test = true, .write = true, .compare = rhi::CompareOp::Greater}; // reverse-Z
    mesh_desc.color_formats[0] = hdr_format;
    mesh_desc.color_target_count = 1;
    mesh_desc.depth_format = depth_format_;
    mesh_pipeline_ =
        make_pipeline(forward_msl.c_str(), mesh_desc, 1, 4, kShadowTextureSlot + kMaxCascades, 2);
    sampler_ = device.create_sampler({.max_anisotropy = 8.0f});
    if (!mesh_pipeline_ || !sampler_) {
        return false;
    }

    // Deferred: the G-buffer pass writes HDR (emissive), albedo, normal and
    // depth; the fused lighting draw reads the last three from the tile and
    // writes only HDR; the split one samples them from memory instead.
    rhi::GraphicsPipelineDesc gbuffer_desc = mesh_desc;
    gbuffer_desc.color_formats[1] = kGBufferAlbedoFormat;
    gbuffer_desc.color_formats[2] = kGBufferNormalFormat;
    gbuffer_desc.color_formats[3] = kGBufferDepthFormat;
    gbuffer_desc.color_target_count = 4;
    gbuffer_pipeline_ = make_pipeline(gbuffer_msl.c_str(), gbuffer_desc, 1, 2, 5, 0);
    rhi::GraphicsPipelineDesc fused_desc;
    for (std::uint32_t i = 0; i < 4; ++i) {
        fused_desc.color_formats[i] = gbuffer_desc.color_formats[i];
        fused_desc.color_write[i] = i == 0;
    }
    fused_desc.color_target_count = 4;
    fused_desc.depth_format = depth_format_;
    lighting_fused_pipeline_ = make_pipeline(fused_msl.c_str(), fused_desc, 0, 4, kMaxCascades, 2);
    rhi::GraphicsPipelineDesc split_desc;
    split_desc.color_formats[0] = hdr_format;
    split_desc.color_target_count = 1;
    lighting_split_pipeline_ = make_pipeline(split_msl.c_str(), split_desc, 0, 4, kMaxCascades + 4, 2);
    gbuffer_sampler_ = device.create_sampler({.min_filter = rhi::Filter::Nearest,
                                              .mag_filter = rhi::Filter::Nearest,
                                              .address_u = rhi::AddressMode::ClampToEdge,
                                              .address_v = rhi::AddressMode::ClampToEdge});
    deferred_ = gbuffer_pipeline_ && lighting_fused_pipeline_ && lighting_split_pipeline_ && gbuffer_sampler_;
    if (!deferred_) {
        TY_LOG_WARN("render", "no deferred shading: a G-buffer pipeline failed; forward only");
    }

    // Shadows need the depth format to be sampled as well as drawn into.
    if (device.supports_texture(depth_format_,
                                rhi::TextureUsage::DepthStencilTarget | rhi::TextureUsage::Sampled)) {
        rhi::GraphicsPipelineDesc shadow_desc;
        shadow_desc.vertex_layout = vertex_layout();
        shadow_desc.cull = rhi::CullMode::None; // thin things cast from both sides
        shadow_desc.depth = {.test = true, .write = true, .compare = rhi::CompareOp::Greater};
        shadow_desc.depth_format = depth_format_;
        shadow_pipeline_ = make_pipeline(kShadowMsl, shadow_desc, 1, 0, 0, 0);
        shadow_sampler_ = device.create_sampler({.address_u = rhi::AddressMode::ClampToEdge,
                                                 .address_v = rhi::AddressMode::ClampToEdge,
                                                 .compare = rhi::CompareOp::GreaterEqual});
        // The scene shader declares its shadow maps whether or not there are
        // any this frame, so the slots always need a depth texture in them.
        shadow_fallback_ = device.create_texture(
            {.format = depth_format_,
             .width = 1,
             .height = 1,
             .usage = rhi::TextureUsage::DepthStencilTarget | rhi::TextureUsage::Sampled});
        shadows_ = shadow_pipeline_ && shadow_sampler_ && shadow_fallback_;
    }
    if (!shadows_) {
        TY_LOG_WARN("render", "no shadows: %s cannot be sampled here, or the shadow pipeline failed",
                    rhi::texture_format_name(depth_format_));
    }

    // The point lights: the kernel that sorts them into cells, the buffer
    // they arrive in and the table they land in.
    cluster_kernel_ = device.create_compute_pipeline(cluster_kernel_pipeline_desc());
    light_buffer_ =
        device.create_buffer({.usage = rhi::BufferUsage::Storage,
                              .size = static_cast<std::uint32_t>(kMaxSceneLights * sizeof(PointLight))});
    const ClusterGrid grid = ClusterGrid::make(kClusterSettings, 0.05f);
    cluster_buffer_ = device.create_buffer(
        {.usage = rhi::BufferUsage::Storage,
         .size = static_cast<std::uint32_t>(grid.cluster_count() * sizeof(ClusterLights))});
    clusters_ = cluster_kernel_ && light_buffer_ && cluster_buffer_;
    if (!clusters_) {
        // The scene shader reads the cluster table whatever the settings
        // say, so without it there is no scene: that is an error, not a warning.
        TY_LOG_ERROR("render", "no clustered lights: %s", platform::last_error());
        return false;
    }
    return true;
}

void SceneRenderer::destroy() noexcept {
    if (device_ != nullptr) {
        device_->destroy_graphics_pipeline(mesh_pipeline_);
        device_->destroy_graphics_pipeline(shadow_pipeline_);
        device_->destroy_graphics_pipeline(gbuffer_pipeline_);
        device_->destroy_graphics_pipeline(lighting_fused_pipeline_);
        device_->destroy_graphics_pipeline(lighting_split_pipeline_);
        device_->destroy_sampler(sampler_);
        device_->destroy_sampler(shadow_sampler_);
        device_->destroy_sampler(gbuffer_sampler_);
        device_->destroy_texture(shadow_fallback_);
        device_->destroy_compute_pipeline(cluster_kernel_);
        device_->destroy_buffer(light_buffer_);
        device_->destroy_buffer(cluster_buffer_);
    }
    mesh_pipeline_ = shadow_pipeline_ = gbuffer_pipeline_ = {};
    lighting_fused_pipeline_ = lighting_split_pipeline_ = {};
    sampler_ = shadow_sampler_ = gbuffer_sampler_ = {};
    shadow_fallback_ = {};
    cluster_kernel_ = {};
    light_buffer_ = cluster_buffer_ = {};
    shadows_ = clusters_ = deferred_ = false;
    device_ = nullptr;
}

void SceneRenderer::draw_shadows(rhi::RenderPass& pass, std::uint32_t cascade) noexcept {
    pass.bind_pipeline(shadow_pipeline_);
    const Mat4& light_view_projection = cascades_.cascades[cascade].view_projection;
    const Model* bound = nullptr;
    for (std::uint32_t i = 0; i < frame_.draw_count; ++i) {
        const DrawItem& item = frame_.draws[i];
        if (item.model == nullptr || item.model->mesh.index_count == 0) {
            continue;
        }
        if (item.model != bound) {
            bind_mesh(pass, item.model->mesh);
            bound = item.model;
        }
        const MeshUniforms uniforms{light_view_projection * item.world, item.world};
        pass.push_vertex_uniforms(0, &uniforms, sizeof uniforms);
        pass.draw_indexed(item.model->mesh.index_count); // every submesh at once: no materials here
    }
}

// Every draw through a material pipeline: the forward one, or the G-buffer one.
void SceneRenderer::draw_materials(rhi::RenderPass& pass) noexcept {
    const Model* bound = nullptr;
    for (std::uint32_t i = 0; i < frame_.draw_count; ++i) {
        const DrawItem& item = frame_.draws[i];
        if (item.model == nullptr || item.model->mesh.index_count == 0) {
            continue;
        }
        const Model& model = *item.model;
        if (item.model != bound) {
            bind_mesh(pass, model.mesh);
            bound = item.model;
        }
        const MeshUniforms uniforms{view_projection_ * item.world, item.world};
        pass.push_vertex_uniforms(0, &uniforms, sizeof uniforms);
        for (const Submesh& sub : model.mesh.submeshes) {
            const Material& material = model.materials[sub.material];
            const MaterialUniforms material_uniforms{material.base_color_factor,
                                                     {material.metallic_factor, material.roughness_factor,
                                                      material.occlusion_strength, material.normal_scale},
                                                     {material.emissive_factor, 0.0f}};
            pass.bind_fragment_texture(0, material.base_color, sampler_);
            pass.bind_fragment_texture(1, material.metallic_roughness, sampler_);
            pass.bind_fragment_texture(2, material.occlusion, sampler_);
            pass.bind_fragment_texture(3, material.emissive, sampler_);
            pass.bind_fragment_texture(4, material.normal, sampler_);
            pass.push_fragment_uniforms(1, &material_uniforms, sizeof material_uniforms);
            pass.draw_indexed(sub.index_count, sub.first_index);
        }
    }
}

// What every lighting variant needs bound: the shadow maps (every slot gets
// a depth texture, since the shaders declare four: the cascades, then the
// last one again, or the 1x1 stand-in when shadows are off), the light and
// cell buffers, and the uniforms — at the slots the forward shader uses
// (materials first) or the deferred ones.
void SceneRenderer::bind_lighting(rhi::RenderPass& pass, const PassResources& resources,
                                  std::uint32_t shadow_slot, std::uint32_t shadow_uniform_slot) noexcept {
    pass.push_fragment_uniforms(0, &frame_uniforms_, sizeof frame_uniforms_);
    pass.push_fragment_uniforms(shadow_uniform_slot, &shadow_uniforms_, sizeof shadow_uniforms_);
    pass.push_fragment_uniforms(shadow_uniform_slot + 1, &cluster_uniforms_, sizeof cluster_uniforms_);
    if (clusters_) {
        pass.bind_fragment_storage_buffer(0, resources.buffer(light_list_));
        pass.bind_fragment_storage_buffer(1, resources.buffer(cluster_table_));
    }
    if (shadow_sampler_ && shadow_fallback_) {
        for (std::uint32_t i = 0; i < kMaxCascades; ++i) {
            rhi::TextureHandle map = shadow_fallback_;
            if (shadows_this_frame_ && cascades_.count > 0) {
                map = resources.texture(shadow_maps_[std::min(i, cascades_.count - 1)]);
            }
            pass.bind_fragment_texture(shadow_slot + i, map, shadow_sampler_);
        }
    }
}

void SceneRenderer::add_passes(FrameGraph& graph, GraphTexture& hdr, GraphTexture& depth,
                               const SceneFrame& frame) noexcept {
    TY_PROFILE_SCOPE_NAMED("SceneRenderer::add_passes");
    frame_ = frame;
    const SceneSettings& settings = frame.settings;
    // Without a device the passes are declared for their shape alone, as
    // the post stack does; with one, only what its pipelines can draw.
    const bool shape_only = device_ == nullptr;
    const bool drawable = ready() && clusters_;
    const float aspect =
        static_cast<float>(std::max(frame.width, 1u)) / static_cast<float>(std::max(frame.height, 1u));
    view_projection_ = frame.camera.view_projection(aspect, frame.jitter);
    const Vec3 light = frame.sun.direction;
    shadows_this_frame_ = (shape_only || (drawable && shadows_)) && settings.shadows && frame.draw_count > 0;
    const bool point_lights = settings.point_lights;
    const std::uint32_t light_count = point_lights ? std::min(frame.light_count, kMaxSceneLights) : 0;
    frame_.light_count = light_count;

    // The point lights: the kernel sorts them into the cells before the
    // scene reads them. With the lights off the table still exists, empty,
    // so the shader reads nothing from it.
    grid_ = ClusterGrid::make(kClusterSettings, frame.camera.near);
    const float tan_half_y = std::tan(0.5f * frame.camera.fov_y);
    cluster_uniforms_ = cluster_uniforms(grid_, light_count, static_cast<float>(frame.width),
                                         static_cast<float>(frame.height), tan_half_y * aspect, tan_half_y,
                                         frame.camera.view());
    light_list_ = cluster_table_ = GraphBuffer{};
    if (clusters_) {
        light_list_ = graph.import_buffer("lights", light_buffer_);
        cluster_table_ = graph.import_buffer("clusters", cluster_buffer_);
        graph.add_compute_pass(
            "cluster lights",
            [this](PassBuilder& b) {
                b.read_buffer(light_list_);
                cluster_table_ = b.write_buffer(cluster_table_);
            },
            [this](rhi::ComputePass& pass, const PassResources& resources) {
                pass.bind_pipeline(cluster_kernel_);
                pass.bind_storage_buffer(0, resources.buffer(light_list_));
                pass.push_uniforms(0, &cluster_uniforms_, sizeof cluster_uniforms_);
                pass.dispatch((grid_.cluster_count() + 63) / 64);
            });
    }

    // The cascades: fitted to this frame's view, each drawn from the light.
    cascades_ = CascadeSet{};
    if (shadows_this_frame_) {
        cascades_ = fit_cascades(frame.camera, aspect, light, cascade_settings);
        for (std::uint32_t i = 0; i < cascades_.count; ++i) {
            shadow_maps_[i] = graph.create(kShadowPassNames[i], {.format = depth_format_,
                                                                 .width = cascade_settings.map_size,
                                                                 .height = cascade_settings.map_size});
            graph.add_pass(
                kShadowPassNames[i],
                [this, i](PassBuilder& b) { shadow_maps_[i] = b.write_depth(shadow_maps_[i]); },
                [this, i](rhi::RenderPass& pass, const PassResources&) { draw_shadows(pass, i); });
        }
    }
    shadow_uniforms_ = ShadowUniforms{};
    for (std::uint32_t i = 0; i < cascades_.count; ++i) {
        const Cascade& cascade = cascades_.cascades[i];
        shadow_uniforms_.cascade_matrix[i] = cascade.view_projection;
        shadow_uniforms_.cascade_param[i] = {cascade.texel_size, 1.0f / cascade.depth_range,
                                             1.0f / static_cast<float>(cascade_settings.map_size), 0.0f};
    }
    shadow_uniforms_.settings = {static_cast<float>(cascades_.count), shadows_this_frame_ ? 1.0f : 0.0f,
                                 kShadowNormalOffsetTexels, kShadowBiasTexels};

    frame_uniforms_ = FrameUniforms{
        {frame.camera.position, 1.0f},
        {frame.camera.forward(), 0.0f},
        {light, frame.sun.intensity},
        {frame.sun.color, frame.sun.ambient},
        {static_cast<float>(settings.model), static_cast<float>(settings.debug_view), 0.0f, 0.0f}};
    deferred_uniforms_ = DeferredUniforms{
        inverse(frame.camera.view()),
        {tan_half_y * aspect, tan_half_y, static_cast<float>(frame.width), static_cast<float>(frame.height)}};

    const auto read_lighting_inputs = [this](PassBuilder& b) {
        for (std::uint32_t i = 0; i < cascades_.count; ++i) {
            b.read(shadow_maps_[i]);
        }
        if (clusters_) {
            b.read_buffer(light_list_);
            b.read_buffer(cluster_table_);
        }
    };
    const rhi::ClearColor sky{frame.sky.x, frame.sky.y, frame.sky.z, 1.0f};
    path_ = (deferred_ || shape_only) ? settings.path : ShadingPath::Forward;
    if (path_ == ShadingPath::Forward) {
        graph.add_pass(
            "scene",
            [&](PassBuilder& b) {
                read_lighting_inputs(b);
                hdr = b.write_color(hdr, rhi::LoadOp::Clear, sky);
                depth = b.write_depth(depth, rhi::LoadOp::Clear, 0.0f);
            },
            [this](rhi::RenderPass& pass, const PassResources& resources) {
                if (!ready() || !clusters_) {
                    return;
                }
                pass.bind_pipeline(mesh_pipeline_);
                bind_lighting(pass, resources, kShadowTextureSlot, 2);
                draw_materials(pass);
            });
        return;
    }

    // The G-buffer: three attachments beside HDR and depth. In the fused
    // pass nothing reads them afterwards, so the graph stores none of them
    // and, on the native backend, makes them memoryless — the lighting draw
    // reads them from the tile. In the split path the next pass samples
    // them, so they are stored and read back.
    const std::uint32_t w = frame.width, h = frame.height;
    GraphTexture g_albedo = graph.create("g albedo", {kGBufferAlbedoFormat, w, h});
    GraphTexture g_normal = graph.create("g normal", {kGBufferNormalFormat, w, h});
    GraphTexture g_depth = graph.create("g depth", {kGBufferDepthFormat, w, h});
    const auto write_gbuffer = [&](PassBuilder& b, GraphTexture& target) {
        target = b.write_color(target, rhi::LoadOp::Clear, sky);
        g_albedo = b.write_color(g_albedo, rhi::LoadOp::DontCare);
        g_normal = b.write_color(g_normal, rhi::LoadOp::DontCare);
        // A view depth of 0 marks the sky, so the lighting leaves it alone.
        g_depth = b.write_color(g_depth, rhi::LoadOp::Clear, {0.0f, 0.0f, 0.0f, 0.0f});
        depth = b.write_depth(depth, rhi::LoadOp::Clear, 0.0f);
    };
    if (path_ == ShadingPath::Fused) {
        graph.add_pass(
            "gbuffer+lighting",
            [&](PassBuilder& b) {
                read_lighting_inputs(b);
                write_gbuffer(b, hdr);
            },
            [this](rhi::RenderPass& pass, const PassResources& resources) {
                if (!deferred_) {
                    return;
                }
                pass.bind_pipeline(gbuffer_pipeline_);
                pass.push_fragment_uniforms(0, &frame_uniforms_, sizeof frame_uniforms_);
                draw_materials(pass);
                // Then, in the same pass, the lighting over the whole screen,
                // reading what the draws above left in the tile.
                pass.bind_pipeline(lighting_fused_pipeline_);
                bind_lighting(pass, resources, 0, 1);
                pass.push_fragment_uniforms(3, &deferred_uniforms_, sizeof deferred_uniforms_);
                pass.draw(3);
            });
        return;
    }
    GraphTexture emissive = graph.create("g emissive", {hdr_format_, w, h});
    graph.add_pass(
        "gbuffer", [&](PassBuilder& b) { write_gbuffer(b, emissive); },
        [this](rhi::RenderPass& pass, const PassResources&) {
            if (!deferred_) {
                return;
            }
            pass.bind_pipeline(gbuffer_pipeline_);
            pass.push_fragment_uniforms(0, &frame_uniforms_, sizeof frame_uniforms_);
            draw_materials(pass);
        });
    graph.add_pass(
        "lighting",
        [&](PassBuilder& b) {
            read_lighting_inputs(b);
            b.read(emissive);
            b.read(g_albedo);
            b.read(g_normal);
            b.read(g_depth);
            hdr = b.write_color(hdr, rhi::LoadOp::DontCare);
        },
        [this, emissive, g_albedo, g_normal, g_depth](rhi::RenderPass& pass, const PassResources& resources) {
            if (!deferred_) {
                return;
            }
            pass.bind_pipeline(lighting_split_pipeline_);
            bind_lighting(pass, resources, 0, 1);
            pass.push_fragment_uniforms(3, &deferred_uniforms_, sizeof deferred_uniforms_);
            const GraphTexture inputs[4] = {emissive, g_albedo, g_normal, g_depth};
            for (std::uint32_t i = 0; i < 4; ++i) {
                pass.bind_fragment_texture(kMaxCascades + i, resources.texture(inputs[i]), gbuffer_sampler_);
            }
            pass.draw(3);
        });
}

bool SceneRenderer::stream(rhi::Frame& frame) noexcept {
    if (!clusters_ || frame_.light_count == 0 || frame_.lights == nullptr) {
        return true;
    }
    const auto bytes = static_cast<std::uint32_t>(frame_.light_count * sizeof(PointLight));
    if (!frame.write_buffer(light_buffer_, frame_.lights, bytes)) {
        TY_LOG_ERROR("render", "light upload failed: %s", platform::last_error());
        return false;
    }
    return true;
}

} // namespace tynima::render
