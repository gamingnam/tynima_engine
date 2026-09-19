#include <tynima/render/post.h>

#include <tynima/core/log.h>
#include <tynima/platform/platform.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace tynima::render {

using math::Mat4;
using math::Vec2;
using math::Vec4;

namespace {

// ------------------------------------------------------------------ shaders

// Three vertices that cover the screen, shared by every pass below.
constexpr const char* kFullscreenVertex = R"(
#include <metal_stdlib>
using namespace metal;

struct VSOut {
    float4 position [[position]];
    float2 uv;
};

vertex VSOut vs_main(uint vid [[vertex_id]]) {
    float2 corner = float2((vid << 1) & 2, vid & 2);
    VSOut out;
    out.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    out.uv = float2(corner.x, 1.0 - corner.y); // texture rows run top to bottom
    return out;
}

struct BloomUniforms {
    float4 texel;  // x, y: 1 / source size; z: threshold; w: knee
    float4 params; // x: prefilter here; z: upsample radius in texels
};

struct TaaUniforms {
    float4x4 reproject;
    float4 params; // x: blend; y, z: 1 / size; w: history is valid
};

struct TonemapUniforms {
    float4 params; // x: operator; y: encode sRGB; z: bloom intensity; w: bloom present
};

struct FxaaUniforms {
    float4 params; // x, y: 1 / size; z: the image is sRGB-encoded; w: encode sRGB on the way out
};
)";

// Downsample by two with the 13-tap filter from Jimenez's "Next Generation
// Post Processing in Call of Duty: Advanced Warfare" (SIGGRAPH 2014): a
// wide, stable kernel, so the blur stays smooth as things move. The first
// level also keeps only what is bright, with a soft knee.
constexpr const char* kBloomDownFragment = R"(
fragment float4 fs_main(VSOut in [[stage_in]], texture2d<float> src [[texture(0)]], sampler s [[sampler(0)]],
                        constant BloomUniforms& u [[buffer(0)]]) {
    float2 t = u.texel.xy;
    float2 uv = in.uv;
    float3 a = src.sample(s, uv + float2(-2.0, -2.0) * t).rgb;
    float3 b = src.sample(s, uv + float2(0.0, -2.0) * t).rgb;
    float3 c = src.sample(s, uv + float2(2.0, -2.0) * t).rgb;
    float3 d = src.sample(s, uv + float2(-2.0, 0.0) * t).rgb;
    float3 e = src.sample(s, uv).rgb;
    float3 f = src.sample(s, uv + float2(2.0, 0.0) * t).rgb;
    float3 g = src.sample(s, uv + float2(-2.0, 2.0) * t).rgb;
    float3 h = src.sample(s, uv + float2(0.0, 2.0) * t).rgb;
    float3 i = src.sample(s, uv + float2(2.0, 2.0) * t).rgb;
    float3 j = src.sample(s, uv + float2(-1.0, -1.0) * t).rgb;
    float3 k = src.sample(s, uv + float2(1.0, -1.0) * t).rgb;
    float3 l = src.sample(s, uv + float2(-1.0, 1.0) * t).rgb;
    float3 m = src.sample(s, uv + float2(1.0, 1.0) * t).rgb;
    float3 color = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    if (u.params.x > 0.5) {
        float threshold = u.texel.z;
        float knee = u.texel.w;
        float brightness = max(color.r, max(color.g, color.b));
        float soft = clamp(brightness - threshold + knee, 0.0, 2.0 * knee);
        soft = soft * soft / (4.0 * knee + 1e-4);
        float contribution = max(soft, brightness - threshold) / max(brightness, 1e-4);
        color *= contribution;
    }
    return float4(color, 1.0);
}
)";

// Upsample by two with a 3x3 tent, added to the level's own downsample: the
// blur widens as it climbs back up, and every level's light reaches the top.
constexpr const char* kBloomUpFragment = R"(
fragment float4 fs_main(VSOut in [[stage_in]], texture2d<float> smaller [[texture(0)]],
                        texture2d<float> same [[texture(1)]], sampler s0 [[sampler(0)]],
                        sampler s1 [[sampler(1)]], constant BloomUniforms& u [[buffer(0)]]) {
    float2 t = u.texel.xy * u.params.z;
    float2 uv = in.uv;
    float3 sum = smaller.sample(s0, uv + float2(-1.0, -1.0) * t).rgb +
                 smaller.sample(s0, uv + float2(0.0, -1.0) * t).rgb * 2.0 +
                 smaller.sample(s0, uv + float2(1.0, -1.0) * t).rgb +
                 smaller.sample(s0, uv + float2(-1.0, 0.0) * t).rgb * 2.0 +
                 smaller.sample(s0, uv).rgb * 4.0 +
                 smaller.sample(s0, uv + float2(1.0, 0.0) * t).rgb * 2.0 +
                 smaller.sample(s0, uv + float2(-1.0, 1.0) * t).rgb +
                 smaller.sample(s0, uv + float2(0.0, 1.0) * t).rgb * 2.0 +
                 smaller.sample(s0, uv + float2(1.0, 1.0) * t).rgb;
    return float4(same.sample(s1, uv).rgb + sum / 16.0, 1.0);
}
)";

// Temporal anti-aliasing: this frame (drawn with a sub-pixel jitter) blended
// with the last, fetched from where this pixel's surface was then, and held
// inside the neighbourhood's colour range (variance clipping, in YCoCg) so a
// moved object leaves no trail. Weighted by inverse luminance, so a bright
// speck does not flicker in.
constexpr const char* kTaaFragment = R"(
float3 rgb_to_ycocg(float3 c) {
    float y = 0.25 * c.r + 0.5 * c.g + 0.25 * c.b;
    float co = 0.5 * c.r - 0.5 * c.b;
    float cg = -0.25 * c.r + 0.5 * c.g - 0.25 * c.b;
    return float3(y, co, cg);
}

float3 ycocg_to_rgb(float3 c) {
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

fragment float4 fs_main(VSOut in [[stage_in]], texture2d<float> current [[texture(0)]],
                        texture2d<float> history [[texture(1)]], depth2d<float> depth [[texture(2)]],
                        sampler point [[sampler(0)]], sampler linear [[sampler(1)]],
                        sampler point2 [[sampler(2)]], constant TaaUniforms& u [[buffer(0)]]) {
    float2 uv = in.uv;
    float2 t = u.params.yz;
    float3 c = current.sample(point, uv).rgb;

    // The neighbourhood's mean and spread, one standard deviation each way.
    float3 m1 = float3(0.0);
    float3 m2 = float3(0.0);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float3 n = rgb_to_ycocg(current.sample(point, uv + float2(x, y) * t).rgb);
            m1 += n;
            m2 += n * n;
        }
    }
    float3 mean = m1 / 9.0;
    float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
    float3 lo = mean - sigma;
    float3 hi = mean + sigma;

    // Where this pixel's surface was last frame.
    float d = depth.sample(point2, uv);
    float2 prev_uv = uv;
    if (d > 0.0) {
        float4 clip = u.reproject * float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
        float3 ndc = clip.xyz / clip.w;
        prev_uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    }
    float3 h = history.sample(linear, prev_uv).rgb;
    h = ycocg_to_rgb(clamp(rgb_to_ycocg(h), lo, hi));

    float blend = u.params.x;
    bool off_screen = any(prev_uv < float2(0.0)) || any(prev_uv > float2(1.0));
    if (u.params.w < 0.5 || off_screen) {
        blend = 1.0;
    }
    float wc = blend / (1.0 + dot(c, float3(0.299, 0.587, 0.114)));
    float wh = (1.0 - blend) / (1.0 + dot(h, float3(0.299, 0.587, 0.114)));
    return float4((c * wc + h * wh) / max(wc + wh, 1e-5), 1.0);
}
)";

// Tonemapping, with the bloom added first. ACES is Narkowicz's fit; AgX is
// Sobotka's, in the compact form Wrensch derived (an inset matrix, a log
// encoding, a sigmoid, the outset), both leaving linear light for the sRGB
// encode — done here only when the swapchain does not do it in hardware.
constexpr const char* kTonemapFragment = R"(
float3 tonemap_aces(float3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 tonemap_agx(float3 val) {
    const float3x3 inset = float3x3(float3(0.842479062253094, 0.0423282422610123, 0.0423756549057051),
                                    float3(0.0784335999999992, 0.878468636469772, 0.0784336),
                                    float3(0.0792237451477643, 0.0791661274605434, 0.879142973793104));
    const float3x3 outset = float3x3(float3(1.19687900512017, -0.0528968517574562, -0.0529716355144438),
                                     float3(-0.0980208811401368, 1.15190312990417, -0.0980434501171241),
                                     float3(-0.0990297440797205, -0.0989611768448433, 1.15107367264116));
    const float min_ev = -12.47393;
    const float max_ev = 4.026069;
    val = inset * max(val, float3(1e-10));
    val = clamp(log2(val), min_ev, max_ev);
    val = (val - min_ev) / (max_ev - min_ev);
    float3 x = val;
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    val = 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
    val = outset * val;
    return pow(max(val, float3(0.0)), float3(2.2)); // the sigmoid's output is display-encoded; back to linear
}

fragment float4 fs_main(VSOut in [[stage_in]], texture2d<float> image [[texture(0)]],
                        texture2d<float> bloom [[texture(1)]], sampler point [[sampler(0)]],
                        sampler linear [[sampler(1)]], constant TonemapUniforms& u [[buffer(0)]]) {
    float3 color = image.sample(point, in.uv).rgb;
    if (u.params.w > 0.5) {
        color += bloom.sample(linear, in.uv).rgb * u.params.z;
    }
    int op = int(u.params.x);
    if (op == 1) {
        color = tonemap_aces(color);
    } else if (op == 2) {
        color = tonemap_agx(color);
    }
    if (u.params.y > 0.5) {
        color = pow(max(color, float3(0.0)), float3(1.0 / 2.2));
    }
    return float4(color, 1.0);
}
)";

// FXAA, the compact form of Lottes's 3.11: the luma gradient across the
// pixel picks an edge direction, the image is blurred along it, and the
// blur is kept only where it stays within the neighbourhood's luma range.
constexpr const char* kFxaaFragment = R"(
float fxaa_luma(float3 c, float encoded) {
    float l = dot(c, float3(0.299, 0.587, 0.114));
    return encoded > 0.5 ? l : sqrt(l); // perceptual, as the filter expects
}

fragment float4 fs_main(VSOut in [[stage_in]], texture2d<float> image [[texture(0)]],
                        sampler s [[sampler(0)]], constant FxaaUniforms& u [[buffer(0)]]) {
    float2 t = u.params.xy;
    float2 uv = in.uv;
    float3 rgb_m = image.sample(s, uv).rgb;
    float luma_nw = fxaa_luma(image.sample(s, uv + float2(-1.0, -1.0) * t).rgb, u.params.z);
    float luma_ne = fxaa_luma(image.sample(s, uv + float2(1.0, -1.0) * t).rgb, u.params.z);
    float luma_sw = fxaa_luma(image.sample(s, uv + float2(-1.0, 1.0) * t).rgb, u.params.z);
    float luma_se = fxaa_luma(image.sample(s, uv + float2(1.0, 1.0) * t).rgb, u.params.z);
    float luma_m = fxaa_luma(rgb_m, u.params.z);
    float luma_min = min(luma_m, min(min(luma_nw, luma_ne), min(luma_sw, luma_se)));
    float luma_max = max(luma_m, max(max(luma_nw, luma_ne), max(luma_sw, luma_se)));

    float2 dir = float2(-((luma_nw + luma_ne) - (luma_sw + luma_se)),
                        (luma_nw + luma_sw) - (luma_ne + luma_se));
    float dir_reduce = max((luma_nw + luma_ne + luma_sw + luma_se) * 0.25 * (1.0 / 8.0), 1.0 / 128.0);
    float rcp_dir_min = 1.0 / (min(abs(dir.x), abs(dir.y)) + dir_reduce);
    dir = clamp(dir * rcp_dir_min, float2(-8.0), float2(8.0)) * t;

    float3 rgb_a = 0.5 * (image.sample(s, uv + dir * (1.0 / 3.0 - 0.5)).rgb +
                          image.sample(s, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    float3 rgb_b = rgb_a * 0.5 + 0.25 * (image.sample(s, uv + dir * -0.5).rgb +
                                         image.sample(s, uv + dir * 0.5).rgb);
    float luma_b = fxaa_luma(rgb_b, u.params.z);
    float3 color = (luma_b < luma_min || luma_b > luma_max) ? rgb_a : rgb_b;
    if (u.params.w > 0.5) {
        color = pow(max(color, float3(0.0)), float3(1.0 / 2.2));
    }
    return float4(color, 1.0);
}
)";

constexpr const char* kDownNames[kMaxBloomLevels] = {"bloom down 0", "bloom down 1", "bloom down 2",
                                                     "bloom down 3", "bloom down 4", "bloom down 5"};
constexpr const char* kUpNames[kMaxBloomLevels] = {"bloom up 0", "bloom up 1", "bloom up 2",
                                                   "bloom up 3", "bloom up 4", "bloom up 5"};
constexpr rhi::TextureFormat kLdrFormat = rhi::TextureFormat::Rgba8Srgb; // encoded on write, decoded on read

// A pipeline from the shared vertex shader and one fragment source.
rhi::PipelineHandle make_pipeline(rhi::Device& device, const char* fragment, rhi::TextureFormat target,
                                  std::uint32_t samplers, std::string& source) {
    source.assign(kFullscreenVertex);
    source += fragment;
    rhi::ShaderDesc vs{.stage = rhi::ShaderStage::Vertex,
                       .format = rhi::ShaderFormat::Msl,
                       .code = source.data(),
                       .code_size = source.size(),
                       .entry_point = "vs_main"};
    rhi::ShaderDesc fs{.stage = rhi::ShaderStage::Fragment,
                       .format = rhi::ShaderFormat::Msl,
                       .code = source.data(),
                       .code_size = source.size(),
                       .entry_point = "fs_main",
                       .num_uniform_buffers = 1,
                       .num_samplers = samplers};
    const rhi::ShaderHandle vertex = device.create_shader(vs);
    const rhi::ShaderHandle fragment_shader = device.create_shader(fs);
    rhi::PipelineHandle pipeline;
    if (vertex && fragment_shader) {
        rhi::GraphicsPipelineDesc desc{.vertex_shader = vertex, .fragment_shader = fragment_shader};
        desc.color_formats[0] = target;
        desc.color_target_count = 1;
        pipeline = device.create_graphics_pipeline(desc);
    }
    device.destroy_shader(vertex);
    device.destroy_shader(fragment_shader);
    return pipeline;
}

float halton(std::uint32_t index, std::uint32_t base) noexcept {
    float result = 0.0f;
    float f = 1.0f / static_cast<float>(base);
    for (std::uint32_t i = index; i > 0; i /= base) {
        result += f * static_cast<float>(i % base);
        f /= static_cast<float>(base);
    }
    return result;
}

} // namespace

// ------------------------------------------------------------------ helpers

Vec2 halton_jitter(std::uint32_t index, std::uint32_t count) noexcept {
    const std::uint32_t i = (index % std::max<std::uint32_t>(count, 1)) + 1; // the sequence starts at 1
    return {halton(i, 2) - 0.5f, halton(i, 3) - 0.5f};
}

std::uint32_t bloom_level_count(std::uint32_t width, std::uint32_t height, std::uint32_t smallest) noexcept {
    std::uint32_t levels = 0;
    std::uint32_t w = width, h = height;
    while (levels < kMaxBloomLevels && w / 2 >= smallest && h / 2 >= smallest) {
        w /= 2;
        h /= 2;
        ++levels;
    }
    return levels;
}

// ---------------------------------------------------------------- PostStack

PostStack::~PostStack() {
    destroy();
}

bool PostStack::create(rhi::Device& device, rhi::TextureFormat hdr_format,
                       rhi::TextureFormat swapchain_format) noexcept {
    destroy();
    if (device.shader_format() != rhi::ShaderFormat::Msl) {
        return false; // the shaders above are MSL only, for now
    }
    hdr_format_ = hdr_format;
    swapchain_format_ = swapchain_format;
    std::string source;
    bloom_down_ = make_pipeline(device, kBloomDownFragment, hdr_format, 1, source);
    bloom_up_ = make_pipeline(device, kBloomUpFragment, hdr_format, 2, source);
    taa_ = make_pipeline(device, kTaaFragment, hdr_format, 3, source);
    tonemap_to_swapchain_ = make_pipeline(device, kTonemapFragment, swapchain_format, 2, source);
    tonemap_to_ldr_ = make_pipeline(device, kTonemapFragment, kLdrFormat, 2, source);
    fxaa_ = make_pipeline(device, kFxaaFragment, swapchain_format, 1, source);
    point_ = device.create_sampler({.min_filter = rhi::Filter::Nearest,
                                    .mag_filter = rhi::Filter::Nearest,
                                    .address_u = rhi::AddressMode::ClampToEdge,
                                    .address_v = rhi::AddressMode::ClampToEdge});
    linear_ = device.create_sampler({.address_u = rhi::AddressMode::ClampToEdge,
                                     .address_v = rhi::AddressMode::ClampToEdge});
    device_ = &device;
    if (!bloom_down_ || !bloom_up_ || !taa_ || !tonemap_to_swapchain_ || !tonemap_to_ldr_ || !fxaa_ ||
        !point_ || !linear_) {
        destroy();
        return false;
    }
    return true;
}

void PostStack::destroy() noexcept {
    if (device_ == nullptr) {
        return;
    }
    for (const rhi::PipelineHandle pipeline :
         {bloom_down_, bloom_up_, taa_, tonemap_to_swapchain_, tonemap_to_ldr_, fxaa_}) {
        device_->destroy_graphics_pipeline(pipeline);
    }
    bloom_down_ = bloom_up_ = taa_ = tonemap_to_swapchain_ = tonemap_to_ldr_ = fxaa_ = {};
    device_->destroy_sampler(point_);
    device_->destroy_sampler(linear_);
    point_ = linear_ = {};
    for (rhi::TextureHandle& history : history_) {
        device_->destroy_texture(history);
        history = {};
    }
    history_width_ = history_height_ = 0;
    history_valid_ = false;
    have_previous_ = false;
    device_ = nullptr;
}

Vec2 PostStack::jitter(std::uint32_t width, std::uint32_t height) const noexcept {
    if (settings.anti_aliasing != AntiAliasing::Taa || !ready() || width == 0 || height == 0) {
        return {};
    }
    // A pixel is 2 / size of clip space; pixel y runs down, clip y up.
    const Vec2 pixels = halton_jitter(frame_index_);
    return {pixels.x * 2.0f / static_cast<float>(width), -pixels.y * 2.0f / static_cast<float>(height)};
}

bool PostStack::ensure_history(std::uint32_t width, std::uint32_t height) noexcept {
    if (history_[0] && history_[1] && history_width_ == width && history_height_ == height) {
        return true;
    }
    for (rhi::TextureHandle& history : history_) {
        device_->destroy_texture(history);
        history = device_->create_texture({.format = hdr_format_,
                                           .width = width,
                                           .height = height,
                                           .usage = rhi::TextureUsage::ColorTarget |
                                                    rhi::TextureUsage::Sampled});
    }
    history_width_ = width;
    history_height_ = height;
    history_valid_ = false;
    if (!history_[0] || !history_[1]) {
        TY_LOG_ERROR("post", "TAA history textures failed: %s", platform::last_error());
        return false;
    }
    return true;
}

// The passes are declared whether or not the pipelines exist (a graph with
// no device compiles them all the same, which is how the tests see the
// shape); without them the callbacks draw nothing and the swapchain is
// cleared rather than left to chance.
GraphTexture PostStack::add_passes(FrameGraph& graph, GraphTexture hdr, GraphTexture depth,
                                   GraphTexture swapchain, const PostFrame& frame) noexcept {
    const TextureInfo screen{.format = hdr_format_, .width = frame.width, .height = frame.height};
    const rhi::ClearColor black{};
    const rhi::LoadOp present_load = ready() ? rhi::LoadOp::DontCare : rhi::LoadOp::Clear;

    // Bloom: down the chain with the wide filter, then back up adding each
    // level's own blur to the widening one from below.
    GraphTexture bloom;
    if (settings.bloom) {
        const std::uint32_t count = bloom_level_count(frame.width, frame.height);
        GraphTexture source = hdr;
        std::uint32_t source_width = frame.width, source_height = frame.height;
        for (std::uint32_t i = 0; i < count; ++i) {
            Level& level = levels_[i];
            level.width = std::max(source_width / 2, 1u);
            level.height = std::max(source_height / 2, 1u);
            level.down = graph.create(kDownNames[i], {hdr_format_, level.width, level.height});
            bloom_uniforms_[i] = BloomUniforms{
                .texel = Vec4{1.0f / static_cast<float>(source_width),
                              1.0f / static_cast<float>(source_height), settings.bloom_threshold,
                              settings.bloom_knee},
                .params = Vec4{i == 0 ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f}};
            const BloomUniforms* uniforms = &bloom_uniforms_[i];
            graph.add_pass(
                kDownNames[i],
                [&](PassBuilder& b) {
                    b.read(source);
                    level.down = b.write_color(level.down, rhi::LoadOp::DontCare, black);
                },
                [this, source, uniforms](rhi::RenderPass& pass, const PassResources& r) {
                    if (!bloom_down_) {
                        return;
                    }
                    pass.bind_pipeline(bloom_down_);
                    pass.bind_fragment_texture(0, r.texture(source), linear_);
                    pass.push_fragment_uniforms(0, uniforms, sizeof *uniforms);
                    pass.draw(3);
                });
            source = level.down;
            source_width = level.width;
            source_height = level.height;
        }
        if (count > 0) {
            GraphTexture smaller = levels_[count - 1].down;
            std::uint32_t smaller_width = levels_[count - 1].width;
            std::uint32_t smaller_height = levels_[count - 1].height;
            for (std::uint32_t i = count - 1; i-- > 0;) {
                Level& level = levels_[i];
                level.up = graph.create(kUpNames[i], {hdr_format_, level.width, level.height});
                bloom_uniforms_[kMaxBloomLevels + i] = BloomUniforms{
                    .texel = Vec4{1.0f / static_cast<float>(smaller_width),
                                  1.0f / static_cast<float>(smaller_height), 0.0f, 0.0f},
                    .params = Vec4{0.0f, 0.0f, 1.0f, 0.0f}};
                const BloomUniforms* uniforms = &bloom_uniforms_[kMaxBloomLevels + i];
                const GraphTexture same = level.down;
                graph.add_pass(
                    kUpNames[i],
                    [&](PassBuilder& b) {
                        b.read(smaller);
                        b.read(same);
                        level.up = b.write_color(level.up, rhi::LoadOp::DontCare, black);
                    },
                    [this, smaller, same, uniforms](rhi::RenderPass& pass, const PassResources& r) {
                        if (!bloom_up_) {
                            return;
                        }
                        pass.bind_pipeline(bloom_up_);
                        pass.bind_fragment_texture(0, r.texture(smaller), linear_);
                        pass.bind_fragment_texture(1, r.texture(same), linear_);
                        pass.push_fragment_uniforms(0, uniforms, sizeof *uniforms);
                        pass.draw(3);
                    });
                smaller = level.up;
                smaller_width = level.width;
                smaller_height = level.height;
            }
            bloom = smaller;
        }
    }

    // TAA: the resolved frame is written straight into the history texture
    // the next frame will read, so nothing is copied.
    GraphTexture image = hdr;
    if (settings.anti_aliasing == AntiAliasing::Taa && ready() && ensure_history(frame.width, frame.height)) {
        const std::uint32_t next = history_index_ ^ 1u;
        const GraphTexture previous = graph.import("taa history", history_[history_index_], screen);
        taa_output_ = graph.import("taa output", history_[next], screen);
        taa_uniforms_ = TaaUniforms{
            .reproject = previous_view_projection_ * math::inverse(frame.view_projection_jittered),
            .params = Vec4{settings.taa_blend, 1.0f / static_cast<float>(frame.width),
                           1.0f / static_cast<float>(frame.height),
                           history_valid_ && have_previous_ ? 1.0f : 0.0f}};
        graph.add_pass(
            "taa",
            [&](PassBuilder& b) {
                b.read(hdr);
                b.read(previous);
                b.read(depth);
                taa_output_ = b.write_color(taa_output_, rhi::LoadOp::DontCare, black);
            },
            [this, hdr, previous, depth](rhi::RenderPass& pass, const PassResources& r) {
                if (!taa_) {
                    return;
                }
                pass.bind_pipeline(taa_);
                pass.bind_fragment_texture(0, r.texture(hdr), point_);
                pass.bind_fragment_texture(1, r.texture(previous), linear_);
                pass.bind_fragment_texture(2, r.texture(depth), point_);
                pass.push_fragment_uniforms(0, &taa_uniforms_, sizeof taa_uniforms_);
                pass.draw(3);
            });
        image = taa_output_;
        history_index_ = next;
        history_valid_ = true;
    } else {
        history_valid_ = false; // whatever is there will be stale by the time TAA is back
    }
    previous_view_projection_ = frame.view_projection;
    have_previous_ = true;

    // Tonemap onto the swapchain, or onto an LDR image for FXAA to finish.
    const bool fxaa = settings.anti_aliasing == AntiAliasing::Fxaa;
    const bool last = !fxaa; // the pass that writes the swapchain encodes for it
    tonemap_uniforms_ = TonemapUniforms{.params = Vec4{static_cast<float>(settings.tonemap),
                                                       last && frame.encode_srgb ? 1.0f : 0.0f,
                                                       settings.bloom_intensity, bloom ? 1.0f : 0.0f}};
    if (fxaa) {
        ldr_ = graph.create("ldr", {kLdrFormat, frame.width, frame.height});
    }
    const GraphTexture bloom_or_image = bloom ? bloom : image; // something valid in the slot either way
    graph.add_pass(
        "tonemap",
        [&](PassBuilder& b) {
            b.read(image);
            if (bloom) {
                b.read(bloom);
            }
            if (fxaa) {
                ldr_ = b.write_color(ldr_, rhi::LoadOp::DontCare, black);
            } else {
                swapchain = b.write_color(swapchain, present_load, black);
            }
        },
        [this, image, bloom_or_image, fxaa](rhi::RenderPass& pass, const PassResources& r) {
            const rhi::PipelineHandle pipeline = fxaa ? tonemap_to_ldr_ : tonemap_to_swapchain_;
            if (!pipeline) {
                return;
            }
            pass.bind_pipeline(pipeline);
            pass.bind_fragment_texture(0, r.texture(image), point_);
            pass.bind_fragment_texture(1, r.texture(bloom_or_image), linear_);
            pass.push_fragment_uniforms(0, &tonemap_uniforms_, sizeof tonemap_uniforms_);
            pass.draw(3);
        });
    if (fxaa) {
        fxaa_uniforms_ = FxaaUniforms{.params = Vec4{1.0f / static_cast<float>(frame.width),
                                                     1.0f / static_cast<float>(frame.height), 0.0f,
                                                     frame.encode_srgb ? 1.0f : 0.0f}};
        graph.add_pass(
            "fxaa",
            [&](PassBuilder& b) {
                b.read(ldr_);
                swapchain = b.write_color(swapchain, present_load, black);
            },
            [this](rhi::RenderPass& pass, const PassResources& r) {
                if (!fxaa_) {
                    return;
                }
                pass.bind_pipeline(fxaa_);
                pass.bind_fragment_texture(0, r.texture(ldr_), linear_);
                pass.push_fragment_uniforms(0, &fxaa_uniforms_, sizeof fxaa_uniforms_);
                pass.draw(3);
            });
    }
    ++frame_index_;
    return swapchain;
}

} // namespace tynima::render
