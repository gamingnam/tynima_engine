#pragma once

#include <tynima/core/math.h>
#include <tynima/render/frame_graph.h>
#include <tynima/rhi/device.h>

#include <cstdint>

// The post stack: what happens to the lit HDR image on its way to the
// screen. Bloom (the bright parts blurred wide and added back), temporal
// anti-aliasing (the frame blended with the last, reprojected), tonemapping
// (ACES or AgX, HDR down to the display's range), and FXAA (edges smoothed
// in the final image) — each a pass in the frame graph, each optional.
namespace tynima::render {

enum class Tonemap : std::uint8_t { None, Aces, Agx };
enum class AntiAliasing : std::uint8_t { None, Fxaa, Taa };
inline constexpr std::uint32_t kMaxBloomLevels = 6;

// The Halton (2, 3) sequence: sub-pixel offsets in [-0.5, 0.5] that never
// repeat a pattern within `count` frames and cover the pixel evenly.
[[nodiscard]] math::Vec2 halton_jitter(std::uint32_t index, std::uint32_t count = 8) noexcept;

// How many times an image halves before its smaller side goes under
// `smallest` pixels, at most kMaxBloomLevels.
[[nodiscard]] std::uint32_t bloom_level_count(std::uint32_t width, std::uint32_t height,
                                              std::uint32_t smallest = 16) noexcept;

struct PostSettings {
    Tonemap tonemap = Tonemap::Aces;
    bool bloom = true;
    float bloom_intensity = 0.12f; // how much of the blurred image is added back
    float bloom_threshold = 1.0f;  // linear radiance below which nothing blooms...
    float bloom_knee = 0.5f;       // ...softened over this much either side
    AntiAliasing anti_aliasing = AntiAliasing::Taa;
    float taa_blend = 0.1f; // the new frame's share; the rest is history
};

// What a frame tells the stack about itself.
struct PostFrame {
    std::uint32_t width = 0;  // pixels
    std::uint32_t height = 0;
    bool encode_srgb = false; // the swapchain is not sRGB-encoded by the hardware: do it in the shader
    math::Mat4 view_projection;          // the camera's, without the jitter
    math::Mat4 view_projection_jittered; // what the scene was drawn with
};

class PostStack {
public:
    PostStack() = default;
    ~PostStack();
    PostStack(const PostStack&) = delete;
    PostStack& operator=(const PostStack&) = delete;

    // Builds the pipelines for an HDR image of `hdr_format` presented to a
    // swapchain of `swapchain_format`. false (with platform::last_error())
    // when a shader or pipeline fails; the stack then adds nothing.
    [[nodiscard]] bool create(rhi::Device& device, rhi::TextureFormat hdr_format,
                              rhi::TextureFormat swapchain_format) noexcept;
    void destroy() noexcept;

    PostSettings settings;

    // The sub-pixel jitter the scene should be drawn with this frame, in
    // clip-space units, ready for Camera::projection(). Zero without TAA.
    [[nodiscard]] math::Vec2 jitter(std::uint32_t width, std::uint32_t height) const noexcept;

    // Adds this frame's passes to `graph`: from `hdr` (with `depth` beside
    // it, for TAA's reprojection) to `swapchain`. Call once per frame, after
    // the scene pass is declared; then compile and execute as usual.
    void add_passes(FrameGraph& graph, GraphTexture hdr, GraphTexture depth, GraphTexture swapchain,
                    const PostFrame& frame) noexcept;

    [[nodiscard]] bool ready() const noexcept { return device_ != nullptr; }

private:
    struct Level {
        GraphTexture down, up;
        std::uint32_t width = 0, height = 0;
    };
    // Every pass's uniforms live here for the frame, so the execute callbacks
    // capture pointers into something that outlives them.
    struct BloomUniforms {
        math::Vec4 texel;  // x, y: 1 / source size; z: threshold; w: knee
        math::Vec4 params; // x: prefilter here; y: unused; z: upsample radius in texels
    };
    struct TaaUniforms {
        math::Mat4 reproject; // this frame's NDC + depth -> last frame's clip
        math::Vec4 params;    // x: blend; y, z: 1 / size; w: history is valid
    };
    struct TonemapUniforms {
        math::Vec4 params; // x: operator; y: encode sRGB; z: bloom intensity; w: bloom present
    };
    struct FxaaUniforms {
        math::Vec4 params; // x, y: 1 / size; z: the image is sRGB-encoded
    };

    [[nodiscard]] bool ensure_history(std::uint32_t width, std::uint32_t height) noexcept;

    rhi::Device* device_ = nullptr;
    rhi::TextureFormat hdr_format_ = rhi::TextureFormat::Rgba16Float;
    rhi::TextureFormat swapchain_format_ = rhi::TextureFormat::Bgra8Unorm;
    rhi::PipelineHandle bloom_down_, bloom_up_, taa_, tonemap_to_swapchain_, tonemap_to_ldr_, fxaa_;
    rhi::SamplerHandle point_, linear_;
    rhi::TextureHandle history_[2];
    std::uint32_t history_width_ = 0, history_height_ = 0;
    std::uint32_t history_index_ = 0; // the one holding last frame's result
    bool history_valid_ = false;
    std::uint32_t frame_index_ = 0;
    math::Mat4 previous_view_projection_ = math::Mat4::identity();
    bool have_previous_ = false;

    BloomUniforms bloom_uniforms_[kMaxBloomLevels * 2]{};
    TaaUniforms taa_uniforms_{};
    TonemapUniforms tonemap_uniforms_{};
    FxaaUniforms fxaa_uniforms_{};
    Level levels_[kMaxBloomLevels]{};
    GraphTexture taa_output_, ldr_;
};

} // namespace tynima::render
