#pragma once

#include <tynima/core/math.h>
#include <tynima/render/camera.h>
#include <tynima/render/clusters.h>
#include <tynima/render/frame_graph.h>
#include <tynima/render/model.h>
#include <tynima/render/shadows.h>
#include <tynima/rhi/device.h>

#include <cstdint>

// The scene renderer: a list of models at world matrices, a camera, a sun
// and some point lights, drawn as the passes of a frame graph into an HDR
// image with a depth buffer beside it — for the post stack to make a
// picture of. It knows nothing about entities: scene/ collects the draw
// list from the World and hands it over.
//
// The passes, per frame: the sun's shadow maps, one depth-only pass per
// cascade; a compute pass sorting the point lights into a cluster grid; then
// the scene itself, one of three ways. Forward lights every mesh fragment as
// it is drawn. Fused writes the material into a G-buffer and lights it in
// the same render pass, reading the G-buffer through the tile — on a
// tile-based GPU it never leaves the chip, and on the native Metal backend
// the graph makes it memoryless. Split stores the G-buffer and samples it
// from the next pass, as a GPU without tile memory must: the fused path's
// yardstick. All three run the same lighting code and draw the same picture.
namespace tynima::render {

enum class ShadingModel : std::uint8_t { Unlit, BlinnPhong, CookTorrance };
enum class ShadingPath : std::uint8_t { Forward, Fused, Split };
// Lit is the picture; the rest show one quantity as it is, untonemapped.
// Cascades is lit with the shadow cascades tinted, so it is a picture too.
enum class DebugView : std::uint8_t {
    Lit,
    Normals,           // the shading normal, mapped
    MetallicRoughness, // metallic in red, roughness in green
    Occlusion,
    VertexNormals,
    Tangents,
    Cascades,
    Clusters, // lights per cell
    Count
};
[[nodiscard]] const char* shading_model_name(ShadingModel model) noexcept;
[[nodiscard]] const char* shading_path_name(ShadingPath path) noexcept;
[[nodiscard]] const char* debug_view_name(DebugView view) noexcept;
[[nodiscard]] constexpr bool debug_view_is_lit(DebugView view) noexcept {
    return view == DebugView::Lit || view == DebugView::Cascades;
}

struct SceneSettings {
    ShadingModel model = ShadingModel::CookTorrance;
    ShadingPath path = ShadingPath::Forward;
    DebugView debug_view = DebugView::Lit;
    bool shadows = true;
    bool point_lights = true;
};

// The one directional light, and the ambient term beside it.
struct Sun {
    math::Vec3 direction{0.0f, 1.0f, 0.0f}; // towards the light, unit length
    float intensity = 3.0f;                 // linear radiance; above 1 is what tonemapping is for
    math::Vec3 color{1.0f, 0.97f, 0.92f};
    float ambient = 0.10f;
};

// One thing to draw: a GPU model at a world matrix. Consecutive items with
// the same model bind its mesh once.
struct DrawItem {
    const Model* model = nullptr;
    math::Mat4 world = math::Mat4::identity();
};

// What a frame gives the renderer. The arrays are read while the graph
// executes, so they must outlive that (the frame arena is the place).
struct SceneFrame {
    std::uint32_t width = 0; // the HDR image, in pixels
    std::uint32_t height = 0;
    Camera camera;
    math::Vec2 jitter{0.0f, 0.0f};       // sub-pixel, in clip units: the post stack's TAA asks for it
    math::Vec3 sky{0.09f, 0.10f, 0.12f}; // what the image is cleared to, linear
    const DrawItem* draws = nullptr;
    std::uint32_t draw_count = 0;
    Sun sun;
    const PointLight* lights = nullptr; // at most kMaxSceneLights
    std::uint32_t light_count = 0;
    SceneSettings settings;
};

inline constexpr std::uint32_t kMaxSceneLights = 256;

class SceneRenderer {
public:
    SceneRenderer() = default;
    ~SceneRenderer();
    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    // Builds the pipelines for drawing into `hdr_format` with the device's
    // preferred depth format. False (with platform::last_error()) when the
    // essential pipeline fails; shadows and point lights are optional and
    // say so in the log — see shadows_available() and lights_available().
    [[nodiscard]] bool create(rhi::Device& device, rhi::TextureFormat hdr_format) noexcept;
    void destroy() noexcept;
    [[nodiscard]] bool ready() const noexcept {
        return device_ != nullptr && static_cast<bool>(mesh_pipeline_);
    }
    [[nodiscard]] bool shadows_available() const noexcept { return shadows_; }
    [[nodiscard]] bool lights_available() const noexcept { return clusters_; }
    [[nodiscard]] bool deferred_available() const noexcept { return deferred_; }

    // The sun's shadow: cascades out to this distance, maps of this size.
    CascadeSettings cascade_settings{.count = 4, .map_size = 2048, .max_distance = 60.0f};

    // Declares this frame's passes into `graph`: `hdr` and `depth` are the
    // caller's transients (created in hdr_format and the device's preferred
    // depth format, `frame.width` by `frame.height`), and come back as the
    // versions the scene wrote. Declares the same passes without a device,
    // so the graph's shape can be examined; they draw nothing then.
    void add_passes(FrameGraph& graph, GraphTexture& hdr, GraphTexture& depth,
                    const SceneFrame& frame) noexcept;
    // Streams the frame's point lights up. Call between graph.compile() and
    // graph.execute(): a copy on the frame's timeline, before any pass runs.
    [[nodiscard]] bool stream(rhi::Frame& frame) noexcept;

    // What the last add_passes() decided: the cascades fitted, and which
    // path was actually taken (Forward when the deferred pipelines are missing).
    [[nodiscard]] const CascadeSet& cascades() const noexcept { return cascades_; }
    [[nodiscard]] ShadingPath path_taken() const noexcept { return path_; }

private:
    // Mirrors of the MSL uniform structs: float4 members only, so C++ and
    // Metal agree on layout. All of a frame's uniforms live here, so the
    // execute callbacks capture pointers into something that outlives them.
    struct MeshUniforms {
        math::Mat4 mvp;
        math::Mat4 model;
    };
    struct FrameUniforms {
        math::Vec4 camera_position;
        math::Vec4 camera_forward;
        math::Vec4 light_direction;
        math::Vec4 light_color;
        math::Vec4 params;
    };
    struct MaterialUniforms {
        math::Vec4 base_color_factor;
        math::Vec4 factors;
        math::Vec4 emissive_factor;
    };
    struct ShadowUniforms {
        math::Mat4 cascade_matrix[kMaxCascades];
        math::Vec4 cascade_param[kMaxCascades];
        math::Vec4 settings;
    };
    struct DeferredUniforms {
        math::Mat4 inverse_view;
        math::Vec4 params;
    };

    [[nodiscard]] rhi::PipelineHandle make_pipeline(const char* msl, const rhi::GraphicsPipelineDesc& base,
                                                    std::uint32_t vertex_uniforms,
                                                    std::uint32_t fragment_uniforms,
                                                    std::uint32_t fragment_samplers,
                                                    std::uint32_t fragment_storage_buffers) noexcept;
    void draw_shadows(rhi::RenderPass& pass, std::uint32_t cascade) noexcept;
    void draw_materials(rhi::RenderPass& pass) noexcept;
    void bind_lighting(rhi::RenderPass& pass, const PassResources& resources, std::uint32_t shadow_slot,
                       std::uint32_t shadow_uniform_slot) noexcept;

    rhi::Device* device_ = nullptr;
    rhi::TextureFormat hdr_format_ = rhi::TextureFormat::Rgba16Float;
    rhi::TextureFormat depth_format_ = rhi::TextureFormat::Depth32Float;
    rhi::PipelineHandle mesh_pipeline_;           // forward
    rhi::PipelineHandle shadow_pipeline_;         // depth only
    rhi::PipelineHandle gbuffer_pipeline_;        // the material into four attachments
    rhi::PipelineHandle lighting_fused_pipeline_; // reads them from the tile, in the same pass
    rhi::PipelineHandle lighting_split_pipeline_; // samples them, in the next pass
    rhi::SamplerHandle sampler_;                  // materials: anisotropic, repeating
    rhi::SamplerHandle shadow_sampler_;           // the shadow maps: compared, bilinear, clamped
    rhi::SamplerHandle gbuffer_sampler_;          // the split path: one texel per pixel
    rhi::TextureHandle shadow_fallback_;          // a 1x1 depth texture for the shadow slots with no maps
    rhi::ComputePipelineHandle cluster_kernel_;   // lists the lights per cell
    rhi::BufferHandle light_buffer_;              // kMaxSceneLights PointLights, streamed in each frame
    rhi::BufferHandle cluster_buffer_;            // one ClusterLights per cell, written by the kernel
    bool shadows_ = false;
    bool clusters_ = false;
    bool deferred_ = false;

    // This frame, as add_passes() left it for the execute callbacks.
    SceneFrame frame_{};
    ShadingPath path_ = ShadingPath::Forward;
    math::Mat4 view_projection_ = math::Mat4::identity();
    CascadeSet cascades_{};
    ClusterGrid grid_{};
    FrameUniforms frame_uniforms_{};
    ShadowUniforms shadow_uniforms_{};
    ClusterUniforms cluster_uniforms_{};
    DeferredUniforms deferred_uniforms_{};
    GraphTexture shadow_maps_[kMaxCascades];
    GraphBuffer light_list_, cluster_table_;
    bool shadows_this_frame_ = false;
};

} // namespace tynima::render
