#pragma once

#include <tynima/core/handle.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace tynima::platform {
class Window;
}

namespace tynima::rhi {

// ------------------------------------------------------------------ handles

// GPU objects are addressed by generational handles (see core/handle.h) and
// owned by the Device that created them. A handle to a destroyed object
// resolves to nothing: creation functions return the null handle on failure,
// binds and uploads with a stale handle are refused (and assert in Debug).
struct ShaderTag {};
struct PipelineTag {};
struct BufferTag {};
struct TextureTag {};
struct SamplerTag {};
using ShaderHandle = core::Handle<ShaderTag>;
using PipelineHandle = core::Handle<PipelineTag>;
using BufferHandle = core::Handle<BufferTag>;
using TextureHandle = core::Handle<TextureTag>;
using SamplerHandle = core::Handle<SamplerTag>;

// ------------------------------------------------------------------ shaders

enum class ShaderStage : std::uint8_t { Vertex, Fragment };

// What a backend consumes. The device tells you which one it wants. Metal
// accepts MSL *source* and compiles it at runtime, which is what lets Phase 0
// run without the Xcode shader toolchain.
enum class ShaderFormat : std::uint8_t { Msl, SpirV, Dxil };
const char* shader_format_name(ShaderFormat format) noexcept;

struct ShaderDesc {
    ShaderStage stage = ShaderStage::Vertex;
    ShaderFormat format = ShaderFormat::Msl;
    const void* code = nullptr; // MSL: source text (no terminator needed); otherwise bytecode
    std::size_t code_size = 0;
    const char* entry_point = "main";
    std::uint32_t num_uniform_buffers = 0; // pushed with RenderPass::push_*_uniforms, slot n = [[buffer(n)]] in MSL
    std::uint32_t num_samplers = 0;
};

// ---------------------------------------------------------------- resources

enum class BufferUsage : std::uint8_t { Vertex, Index };

struct BufferDesc {
    BufferUsage usage = BufferUsage::Vertex;
    std::uint32_t size = 0; // bytes
};

// Rgba8Srgb is the same bytes as Rgba8Unorm, but the GPU decodes sRGB to
// linear when sampling — the right format for color textures authored for
// the eye (base color, emissive); data textures (normals, roughness) stay
// Unorm. The Bgra8 pair is what swapchains come in; Rgba16Float is the
// working format for light before tonemapping.
enum class TextureFormat : std::uint8_t {
    Rgba8Unorm,
    Rgba8Srgb,
    Bgra8Unorm,
    Bgra8Srgb,
    Rgba16Float,
    Depth32Float,
    Depth24Stencil8,
    Depth16
};
const char* texture_format_name(TextureFormat format) noexcept;
[[nodiscard]] constexpr bool is_depth_format(TextureFormat format) noexcept {
    return format == TextureFormat::Depth32Float || format == TextureFormat::Depth24Stencil8 ||
           format == TextureFormat::Depth16;
}
// Bytes per pixel as stored, for every format (depth included: a depth
// texture is never uploaded from the CPU, but it does take memory).
[[nodiscard]] constexpr std::uint32_t bytes_per_pixel(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::Rgba8Unorm:
    case TextureFormat::Rgba8Srgb:
    case TextureFormat::Bgra8Unorm:
    case TextureFormat::Bgra8Srgb:
    case TextureFormat::Depth32Float:
    case TextureFormat::Depth24Stencil8:
        return 4;
    case TextureFormat::Rgba16Float:
        return 8;
    case TextureFormat::Depth16:
        return 2;
    }
    return 0;
}
// Levels in a full mip chain down to 1x1.
[[nodiscard]] std::uint32_t mip_level_count(std::uint32_t width, std::uint32_t height) noexcept;

// Bit flags. Default picks Sampled for color formats and DepthStencilTarget
// for depth formats. GPU mipmap generation needs ColorTarget on every backend.
enum class TextureUsage : std::uint8_t { Default = 0, Sampled = 1, ColorTarget = 2, DepthStencilTarget = 4 };
[[nodiscard]] constexpr TextureUsage operator|(TextureUsage a, TextureUsage b) noexcept {
    return static_cast<TextureUsage>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr bool has_usage(TextureUsage flags, TextureUsage bit) noexcept {
    return (static_cast<std::uint8_t>(flags) & static_cast<std::uint8_t>(bit)) != 0;
}

struct TextureDesc {
    TextureFormat format = TextureFormat::Rgba8Unorm;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mip_levels = 1; // 0 = full chain
    TextureUsage usage = TextureUsage::Default;
};

struct Extent2D {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

enum class Filter : std::uint8_t { Nearest, Linear };
enum class AddressMode : std::uint8_t { Repeat, MirroredRepeat, ClampToEdge };

enum class CompareOp : std::uint8_t {
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always
};

struct SamplerDesc {
    Filter min_filter = Filter::Linear;
    Filter mag_filter = Filter::Linear;
    Filter mip_filter = Filter::Linear;
    AddressMode address_u = AddressMode::Repeat;
    AddressMode address_v = AddressMode::Repeat;
    float max_anisotropy = 1.0f; // 1 = off; 8-16 is what a material sampler wants
    // A comparison sampler: sampling a depth texture through it compares a
    // reference depth against the texels (with the filter blending the 0/1
    // results, which is hardware PCF) — shadow maps' sampler.
    std::optional<CompareOp> compare;
};

// ---------------------------------------------------------------- pipelines

enum class PrimitiveTopology : std::uint8_t { TriangleList, TriangleStrip, LineList };
enum class VertexFormat : std::uint8_t { Float2, Float3, Float4 };
enum class CullMode : std::uint8_t { None, Back, Front };
enum class IndexType : std::uint8_t { Uint16, Uint32 };

// One vertex attribute; `location` is [[attribute(n)]] in MSL.
struct VertexAttribute {
    std::uint32_t location = 0;
    VertexFormat format = VertexFormat::Float3;
    std::uint32_t offset = 0; // bytes from the start of the vertex
};

// One interleaved vertex buffer at slot 0. A view: the attribute array must
// outlive the pipeline creation call only.
struct VertexLayout {
    std::uint32_t stride = 0; // bytes per vertex; 0 means no vertex buffer
    const VertexAttribute* attributes = nullptr;
    std::uint32_t attribute_count = 0;
};

// Reverse-Z by default: GREATER passes what is nearer, and the depth target
// is cleared to 0. See math/transform.h for the matching projection.
struct DepthState {
    bool test = false;
    bool write = false;
    CompareOp compare = CompareOp::Greater;
};

inline constexpr std::uint32_t kMaxColorTargets = 4;

// A pipeline is built for the formats of the targets it will draw into —
// every GPU API bakes them into the pipeline state — and a pass binding
// other formats is refused. No color targets means a depth-only pipeline
// (a shadow map); `Device::swapchain_format()` is what the swapchain wants.
struct GraphicsPipelineDesc {
    ShaderHandle vertex_shader;
    ShaderHandle fragment_shader;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    VertexLayout vertex_layout{};
    CullMode cull = CullMode::None; // front faces are counter-clockwise, as in glTF
    DepthState depth{};
    TextureFormat color_formats[kMaxColorTargets]{};
    std::uint32_t color_target_count = 0;
    std::optional<TextureFormat> depth_format; // nullopt: no depth target
};

struct ClearColor {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

// What a pass does with an attachment's contents on the way in and out.
// Load keeps what is there; Clear fills it; DontCare promises the pass
// writes every pixel it will ever read, and Store/DontCare say whether the
// result must reach memory at all. A tile-based GPU keeps an attachment that
// is cleared or don't-cared in and don't-cared out entirely on-chip.
enum class LoadOp : std::uint8_t { Load, Clear, DontCare };
enum class StoreOp : std::uint8_t { Store, DontCare };

struct ColorAttachment {
    TextureHandle texture; // needs ColorTarget usage; the swapchain handle is one
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    ClearColor clear{};
};

struct DepthAttachment {
    TextureHandle texture; // needs DepthStencilTarget usage
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::DontCare; // depth is rarely read back
    float clear = 0.0f;                // reverse-Z: far
};

// Every attachment must have the same size. `name` labels the pass for GPU
// debuggers and profilers.
struct RenderPassDesc {
    const char* name = nullptr;
    ColorAttachment colors[kMaxColorTargets]{};
    std::uint32_t color_count = 0;
    std::optional<DepthAttachment> depth;
};

// ---------------------------------------------------------------- recording

class Device;

// A render pass on its attachments. end() closes it; the destructor closes
// it if you forget.
class RenderPass {
public:
    RenderPass(RenderPass&& other) noexcept;
    RenderPass& operator=(RenderPass&& other) noexcept;
    RenderPass(const RenderPass&) = delete;
    RenderPass& operator=(const RenderPass&) = delete;
    ~RenderPass();

    void bind_pipeline(PipelineHandle pipeline) noexcept;
    void bind_vertex_buffer(BufferHandle buffer, std::uint32_t offset = 0) noexcept;
    void bind_index_buffer(BufferHandle buffer, IndexType type, std::uint32_t offset = 0) noexcept;
    // Slot n is [[texture(n)]] and [[sampler(n)]] in the fragment shader; the
    // shader's ShaderDesc::num_samplers must cover it.
    void bind_fragment_texture(std::uint32_t slot, TextureHandle texture, SamplerHandle sampler) noexcept;

    // Uniform data for the next draws; `size` bytes are copied immediately.
    // Slot n is [[buffer(n)]] in MSL. Keep structs 16-byte aligned like the shader expects.
    void push_vertex_uniforms(std::uint32_t slot, const void* data, std::uint32_t size) noexcept;
    void push_fragment_uniforms(std::uint32_t slot, const void* data, std::uint32_t size) noexcept;

    void draw(std::uint32_t vertex_count, std::uint32_t instance_count = 1) noexcept;
    void draw_indexed(std::uint32_t index_count, std::uint32_t first_index = 0, std::int32_t vertex_offset = 0,
                      std::uint32_t instance_count = 1) noexcept;
    void end() noexcept;

private:
    friend class Frame;
    RenderPass(Device* device, void* command_buffer, void* pass, bool labelled) noexcept
        : device_(device), command_buffer_(command_buffer), pass_(pass), labelled_(labelled) {}
    Device* device_;       // resolves handles; not owned
    void* command_buffer_; // SDL_GPUCommandBuffer*, not owned
    void* pass_;           // SDL_GPURenderPass*
    bool labelled_;        // a debug group was pushed for the pass's name; end() pops it
};

// One frame's command buffer and, when the window is visible, its swapchain
// image. Must end with submit(); the destructor submits if you forget.
class Frame {
public:
    Frame(Frame&& other) noexcept;
    Frame& operator=(Frame&& other) noexcept;
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    ~Frame();

    // False while the window is minimized: nothing to draw to this frame, but
    // submit() is still required.
    [[nodiscard]] bool has_swapchain_image() const noexcept { return static_cast<bool>(swapchain_); }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }   // pixels
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; } // pixels
    // This frame's swapchain image as a texture handle: a color attachment
    // for passes in this frame only — never sampled, never kept. Null when
    // there is no image.
    [[nodiscard]] TextureHandle swapchain_texture() const noexcept { return swapchain_; }

    // Begins a pass on the given attachments. nullopt (with the reason in
    // platform::last_error()) for no attachments, a destroyed or misused
    // handle, or sizes that differ.
    [[nodiscard]] std::optional<RenderPass> begin_pass(const RenderPassDesc& desc) noexcept;
    // The common case: clear the swapchain image and, if given, the depth
    // texture (which must match the swapchain size), and discard depth after
    // the pass. nullopt when there is no image.
    [[nodiscard]] std::optional<RenderPass> begin_swapchain_pass(const ClearColor& clear, TextureHandle depth = {},
                                                                 float depth_clear = 0.0f) noexcept;
    void submit() noexcept;

private:
    friend class Device;
    Frame(Device* device, void* command_buffer, TextureHandle swapchain, std::uint32_t width,
          std::uint32_t height) noexcept;
    Device* device_;         // resolves handles; not owned
    void* command_buffer_;   // SDL_GPUCommandBuffer*
    TextureHandle swapchain_; // a borrowed entry in the texture pool, gone after submit()
    std::uint32_t width_;
    std::uint32_t height_;
};

// ------------------------------------------------------------------- device

struct DeviceDesc {
    bool debug = false; // validation layers; slow, loud, and worth it in Debug builds
    bool vsync = true;
    // An sRGB-encoded swapchain: shaders write linear light and the display
    // encoding happens in hardware. Falls back to a plain SDR swapchain where
    // unsupported — check swapchain_is_linear() and encode in the shader then.
    bool linear_swapchain = true;
    // Resource budgets: pools of this many. Creation past a budget fails with
    // the null handle rather than growing.
    std::uint32_t max_shaders = 256;
    std::uint32_t max_pipelines = 256;
    std::uint32_t max_buffers = 4096;
    std::uint32_t max_textures = 4096;
    std::uint32_t max_samplers = 64;
};

class Device {
public:
    // Creates a device with no window attached — enough to compile shaders
    // and upload buffers, which is how tests exercise the GPU path headless.
    // nullptr on failure; platform::last_error() says why. Fails where no
    // backend consumes the shader formats we can supply: until SDL_shadercross
    // joins the build, that means Windows.
    [[nodiscard]] static std::unique_ptr<Device> create(const DeviceDesc& desc = {});
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    // Claims the window for presentation. Required before begin_frame() and
    // before creating pipelines (they target the swapchain format). Fails on
    // a headless window: there is no surface to present to.
    [[nodiscard]] bool attach_window(platform::Window& window) noexcept;

    [[nodiscard]] const char* backend_name() const noexcept; // "metal", "vulkan", "direct3d12"
    [[nodiscard]] ShaderFormat shader_format() const noexcept;
    // The best depth format this GPU supports as a render target.
    [[nodiscard]] TextureFormat preferred_depth_format() const noexcept;
    // Whether a texture of this format can be created for these uses at all
    // (sampling a depth format, say).
    [[nodiscard]] bool supports_texture(TextureFormat format, TextureUsage usage) const noexcept;
    // True once a window is attached with an sRGB-encoded swapchain.
    [[nodiscard]] bool swapchain_is_linear() const noexcept { return swapchain_linear_; }
    // The swapchain's format once a window is attached: what a pipeline that
    // draws to it must be built for.
    [[nodiscard]] TextureFormat swapchain_format() const noexcept { return swapchain_format_; }

    // Handle validity: false for null handles and for anything destroyed.
    [[nodiscard]] bool valid(ShaderHandle handle) const noexcept;
    [[nodiscard]] bool valid(PipelineHandle handle) const noexcept;
    [[nodiscard]] bool valid(BufferHandle handle) const noexcept;
    [[nodiscard]] bool valid(TextureHandle handle) const noexcept;
    [[nodiscard]] bool valid(SamplerHandle handle) const noexcept;

    [[nodiscard]] ShaderHandle create_shader(const ShaderDesc& desc) noexcept;
    void destroy_shader(ShaderHandle shader) noexcept;

    // Shaders may be destroyed as soon as the pipeline exists.
    [[nodiscard]] PipelineHandle create_graphics_pipeline(const GraphicsPipelineDesc& desc) noexcept;
    void destroy_graphics_pipeline(PipelineHandle pipeline) noexcept;

    [[nodiscard]] BufferHandle create_buffer(const BufferDesc& desc) noexcept;
    // Copies `size` bytes into `buffer` at `offset` and waits for the copy:
    // for loading, not for per-frame streaming.
    [[nodiscard]] bool upload_buffer(BufferHandle buffer, const void* data, std::uint32_t size,
                                     std::uint32_t offset = 0) noexcept;
    [[nodiscard]] BufferHandle create_buffer_with_data(BufferUsage usage, const void* data,
                                                       std::uint32_t size) noexcept;
    void destroy_buffer(BufferHandle buffer) noexcept;

    [[nodiscard]] TextureHandle create_texture(const TextureDesc& desc) noexcept;
    // {0, 0} for an invalid handle.
    [[nodiscard]] Extent2D texture_extent(TextureHandle texture) const noexcept;
    // Uploads tightly packed pixels for one mip level and waits for the copy.
    // `size` must equal width * height * bytes_per_pixel at that level.
    [[nodiscard]] bool upload_texture(TextureHandle texture, const void* pixels, std::uint32_t size,
                                      std::uint32_t mip_level = 0) noexcept;
    // Fills levels 1..n from level 0 on the GPU and waits. The texture needs
    // more than one level and ColorTarget usage.
    [[nodiscard]] bool generate_mipmaps(TextureHandle texture) noexcept;
    // A sampled color texture with a full mip chain: created, uploaded, mipmapped.
    [[nodiscard]] TextureHandle create_texture_with_data(TextureFormat format, std::uint32_t width,
                                                         std::uint32_t height, const void* pixels,
                                                         std::uint32_t size, bool mipmaps = true) noexcept;
    void destroy_texture(TextureHandle texture) noexcept;

    [[nodiscard]] SamplerHandle create_sampler(const SamplerDesc& desc) noexcept;
    void destroy_sampler(SamplerHandle sampler) noexcept;

    // Live objects per pool: a leak check, and a view of the budgets.
    struct ResourceCounts {
        std::uint32_t shaders = 0, pipelines = 0, buffers = 0, textures = 0, samplers = 0;
    };
    [[nodiscard]] ResourceCounts resource_counts() const noexcept;

    // Acquires this frame's command buffer and swapchain image; with vsync on
    // this is where the loop waits for the display. nullopt on error.
    [[nodiscard]] std::optional<Frame> begin_frame() noexcept;

private:
    friend class Frame;
    friend class RenderPass;
    struct Pools; // the resource pools; complete in device.cpp
    Device(void* device, const DeviceDesc& desc) noexcept;
    [[nodiscard]] bool run_copy_and_wait(void* transfer, void* target, std::uint32_t size, std::uint32_t level_or_offset,
                                         Extent2D extent, bool is_texture) noexcept;
    void* device_;           // SDL_GPUDevice*
    void* window_ = nullptr; // SDL_Window*, once attached
    Pools* pools_;
    bool vsync_;
    bool want_linear_swapchain_;
    bool swapchain_linear_ = false;
    TextureFormat swapchain_format_ = TextureFormat::Bgra8Unorm;
};

} // namespace tynima::rhi
