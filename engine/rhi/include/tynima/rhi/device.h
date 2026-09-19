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
struct ComputePipelineTag {};
struct BufferTag {};
struct TextureTag {};
struct SamplerTag {};
using ShaderHandle = core::Handle<ShaderTag>;
using PipelineHandle = core::Handle<PipelineTag>;
using ComputePipelineHandle = core::Handle<ComputePipelineTag>;
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
    std::uint32_t num_uniform_buffers = 0; // pushed per draw; slot n is [[buffer(n)]] in MSL
    std::uint32_t num_samplers = 0;
    std::uint32_t num_storage_buffers = 0; // Storage buffers bound for reading, after the uniforms in MSL
};

// A compute shader and the counts of what it binds. In MSL the [[buffer(n)]]
// slots run uniform buffers first, then read-only storage buffers, then the
// read-write ones (which a compute pass fixes when it begins); sampled
// textures take [[texture(n)]] with matching [[sampler(n)]].
struct ComputePipelineDesc {
    ShaderFormat format = ShaderFormat::Msl;
    const void* code = nullptr;
    std::size_t code_size = 0;
    const char* entry_point = "main";
    std::uint32_t num_uniform_buffers = 0;
    std::uint32_t num_readonly_storage_buffers = 0;
    std::uint32_t num_readwrite_storage_buffers = 0;
    std::uint32_t num_samplers = 0;
    std::uint32_t threads_x = 64; // the threadgroup size the shader declares
    std::uint32_t threads_y = 1;
    std::uint32_t threads_z = 1;
};

// ---------------------------------------------------------------- resources

// Storage: read by any stage as a storage buffer and written by compute.
enum class BufferUsage : std::uint8_t { Vertex, Index, Storage };

struct BufferDesc {
    BufferUsage usage = BufferUsage::Vertex;
    std::uint32_t size = 0; // bytes
};

// Rgba8Srgb is the same bytes as Rgba8Unorm, but the GPU decodes sRGB to
// linear when sampling — the right format for color textures authored for
// the eye (base color, emissive); data textures (normals, roughness) stay
// Unorm. The Bgra8 pair is what swapchains come in; Rgba16Float is the
// working format for light before tonemapping; Rg32Float holds two exact
// floats (a G-buffer's depth, say).
enum class TextureFormat : std::uint8_t {
    Rgba8Unorm,
    Rgba8Srgb,
    Bgra8Unorm,
    Bgra8Srgb,
    Rgba16Float,
    Rg32Float,
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
    case TextureFormat::Rg32Float:
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
    // A texture that exists only in tile memory, for the length of a pass:
    // an attachment that is cleared or don't-cared in and don't-cared out and
    // never sampled — it takes no memory at all. Only a render target, only
    // one level. Where the backend cannot (SDL GPU), an ordinary texture.
    bool memoryless = false;
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
// Ubyte4Norm: four bytes read as floats in [0, 1] — a packed RGBA colour.
enum class VertexFormat : std::uint8_t { Float2, Float3, Float4, Ubyte4Norm };
enum class CullMode : std::uint8_t { None, Back, Front };
enum class IndexType : std::uint8_t { Uint16, Uint32 };

// How a draw's colour combines with what the target already holds. Off
// writes over it. Alpha is ordinary transparency: source alpha, one minus
// source alpha. Premultiplied is the same for colour already multiplied by
// its alpha: one, one minus source alpha. Additive adds: one, one. Alpha
// itself always accumulates as one, one minus source alpha.
enum class BlendMode : std::uint8_t { Off, Alpha, Premultiplied, Additive };

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
    // false leaves a color target as it is: a draw that reads an attachment
    // in place (MSL's [[color(n)]] inputs, on tile-based GPUs) and writes
    // another declares the ones it only reads this way.
    bool color_write[kMaxColorTargets] = {true, true, true, true};
    BlendMode blend[kMaxColorTargets] = {BlendMode::Off, BlendMode::Off, BlendMode::Off, BlendMode::Off};
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

inline constexpr std::uint32_t kMaxComputeWrites = 4;

// A compute pass: the storage buffers it writes are fixed when it begins,
// bound in order after the pipeline's read-only ones.
struct ComputePassDesc {
    const char* name = nullptr;
    BufferHandle writes[kMaxComputeWrites]{};
    std::uint32_t write_count = 0;
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
    // A Storage buffer for the fragment shader to read: slot n is
    // [[buffer(num_uniform_buffers + n)]] in MSL.
    void bind_fragment_storage_buffer(std::uint32_t slot, BufferHandle buffer) noexcept;

    // Uniform data for the next draws; `size` bytes are copied immediately.
    // Slot n is [[buffer(n)]] in MSL. Keep structs 16-byte aligned like the shader expects.
    void push_vertex_uniforms(std::uint32_t slot, const void* data, std::uint32_t size) noexcept;
    void push_fragment_uniforms(std::uint32_t slot, const void* data, std::uint32_t size) noexcept;

    // Limits the next draws to a rectangle of the attachments, in pixels
    // from the top left; clipped to the attachments' size. A pass starts
    // with the whole attachment.
    void set_scissor(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height) noexcept;

    void draw(std::uint32_t vertex_count, std::uint32_t instance_count = 1) noexcept;
    void draw_indexed(std::uint32_t index_count, std::uint32_t first_index = 0,
                      std::int32_t vertex_offset = 0, std::uint32_t instance_count = 1) noexcept;
    void end() noexcept;

private:
    friend class Device;
    RenderPass(Device* device, void* command_buffer, void* pass, bool labelled) noexcept
        : device_(device), command_buffer_(command_buffer), pass_(pass), labelled_(labelled) {}
    Device* device_;       // not owned
    void* command_buffer_; // the backend's command buffer, not owned
    void* pass_;           // the backend's pass or encoder
    bool labelled_;        // a debug group was pushed for the pass's name; end() pops it
};

// Compute work on the frame's command buffer. end() closes it; the
// destructor closes it if you forget. Never inside a render pass.
class ComputePass {
public:
    ComputePass(ComputePass&& other) noexcept;
    ComputePass& operator=(ComputePass&& other) noexcept;
    ComputePass(const ComputePass&) = delete;
    ComputePass& operator=(const ComputePass&) = delete;
    ~ComputePass();

    void bind_pipeline(ComputePipelineHandle pipeline) noexcept;
    // A Storage buffer the shader reads: slot n is [[buffer(num_uniform_buffers + n)]].
    void bind_storage_buffer(std::uint32_t slot, BufferHandle buffer) noexcept;
    // Uniform data for the next dispatches; `size` bytes are copied at once.
    void push_uniforms(std::uint32_t slot, const void* data, std::uint32_t size) noexcept;
    // Threadgroups, not threads: the pipeline's threads_x/y/z go in each.
    void dispatch(std::uint32_t groups_x, std::uint32_t groups_y = 1, std::uint32_t groups_z = 1) noexcept;
    void end() noexcept;

private:
    friend class Device;
    ComputePass(Device* device, void* command_buffer, void* pass, bool labelled) noexcept
        : device_(device), command_buffer_(command_buffer), pass_(pass), labelled_(labelled) {}
    Device* device_;
    void* command_buffer_;
    void* pass_;
    bool labelled_;
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
    // Begins a compute pass writing the given Storage buffers.
    [[nodiscard]] std::optional<ComputePass> begin_compute_pass(const ComputePassDesc& desc) noexcept;
    // Streams `size` bytes into `buffer` at `offset` on this frame's timeline:
    // every pass begun after this sees them, and nothing waits — per-frame
    // data (lights, instances) goes this way. Only between passes.
    [[nodiscard]] bool write_buffer(BufferHandle buffer, const void* data, std::uint32_t size,
                                    std::uint32_t offset = 0) noexcept;
    // The common case: clear the swapchain image and, if given, the depth
    // texture (which must match the swapchain size), and discard depth after
    // the pass. nullopt when there is no image.
    [[nodiscard]] std::optional<RenderPass> begin_swapchain_pass(const ClearColor& clear,
                                                                 TextureHandle depth = {},
                                                                 float depth_clear = 0.0f) noexcept;
    void submit() noexcept;

private:
    friend class Device;
    Frame(Device* device, void* command_buffer, TextureHandle swapchain, std::uint32_t width,
          std::uint32_t height) noexcept;
    Device* device_;          // not owned
    void* command_buffer_;    // the backend's command buffer
    TextureHandle swapchain_; // a borrowed entry in the texture pool, gone after submit()
    std::uint32_t width_;
    std::uint32_t height_;
};

// ------------------------------------------------------------------- device

// Which implementation stands behind the Device. SDL GPU runs everywhere
// SDL does (Metal, Vulkan, D3D12 underneath); the native Metal backend is
// the engine's own, on Apple GPUs, and the one that can fuse tile passes.
enum class Backend : std::uint8_t { Auto, SdlGpu, Metal };
const char* backend_name(Backend backend) noexcept;

struct DeviceDesc {
    Backend backend = Backend::Auto; // Auto: native Metal on Apple platforms, SDL GPU elsewhere
    bool debug = false;              // validation layers; slow, loud, and worth it in Debug builds
    bool vsync = true;
    // An sRGB-encoded swapchain: shaders write linear light and the display
    // encoding happens in hardware. Falls back to a plain SDR swapchain where
    // unsupported — check swapchain_is_linear() and encode in the shader then.
    bool linear_swapchain = true;
    // Resource budgets: pools of this many. Creation past a budget fails with
    // the null handle rather than growing.
    std::uint32_t max_shaders = 256;
    std::uint32_t max_pipelines = 256;
    std::uint32_t max_compute_pipelines = 64;
    std::uint32_t max_buffers = 4096;
    std::uint32_t max_textures = 4096;
    std::uint32_t max_samplers = 64;
};

// The GPU, in engine vocabulary. One implementation per backend; every
// call here is the same on all of them, and the shaders bind the same way
// (see ShaderDesc and ComputePipelineDesc for the MSL slot order).
class Device {
public:
    // Creates a device with no window attached — enough to compile shaders,
    // upload buffers and run compute, which is how tests exercise the GPU
    // path headless. nullptr on failure; platform::last_error() says why.
    // Fails where no backend consumes the shader formats we can supply:
    // until SDL_shadercross joins the build, that means Windows.
    [[nodiscard]] static std::unique_ptr<Device> create(const DeviceDesc& desc = {});
    virtual ~Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    // Claims the window for presentation. Required before begin_frame() can
    // present. Fails on a headless window: there is no surface to present to.
    [[nodiscard]] virtual bool attach_window(platform::Window& window) noexcept = 0;

    [[nodiscard]] virtual Backend backend() const noexcept = 0;
    // What runs underneath: "metal", "vulkan", "direct3d12".
    [[nodiscard]] virtual const char* backend_name() const noexcept = 0;
    [[nodiscard]] virtual ShaderFormat shader_format() const noexcept = 0;
    // The best depth format this GPU supports as a render target.
    [[nodiscard]] virtual TextureFormat preferred_depth_format() const noexcept = 0;
    // Whether a texture of this format can be created for these uses at all
    // (sampling a depth format, say).
    [[nodiscard]] virtual bool supports_texture(TextureFormat format, TextureUsage usage) const noexcept = 0;
    // Whether TextureDesc::memoryless does anything here.
    [[nodiscard]] virtual bool supports_memoryless() const noexcept = 0;
    // True once a window is attached with an sRGB-encoded swapchain.
    [[nodiscard]] virtual bool swapchain_is_linear() const noexcept = 0;
    // The swapchain's format once a window is attached: what a pipeline that
    // draws to it must be built for.
    [[nodiscard]] virtual TextureFormat swapchain_format() const noexcept = 0;

    // Handle validity: false for null handles and for anything destroyed.
    [[nodiscard]] virtual bool valid(ShaderHandle handle) const noexcept = 0;
    [[nodiscard]] virtual bool valid(PipelineHandle handle) const noexcept = 0;
    [[nodiscard]] virtual bool valid(ComputePipelineHandle handle) const noexcept = 0;
    [[nodiscard]] virtual bool valid(BufferHandle handle) const noexcept = 0;
    [[nodiscard]] virtual bool valid(TextureHandle handle) const noexcept = 0;
    [[nodiscard]] virtual bool valid(SamplerHandle handle) const noexcept = 0;

    [[nodiscard]] virtual ShaderHandle create_shader(const ShaderDesc& desc) noexcept = 0;
    virtual void destroy_shader(ShaderHandle shader) noexcept = 0;

    // Shaders may be destroyed as soon as the pipeline exists.
    [[nodiscard]] virtual PipelineHandle
    create_graphics_pipeline(const GraphicsPipelineDesc& desc) noexcept = 0;
    virtual void destroy_graphics_pipeline(PipelineHandle pipeline) noexcept = 0;

    [[nodiscard]] virtual ComputePipelineHandle
    create_compute_pipeline(const ComputePipelineDesc& desc) noexcept = 0;
    virtual void destroy_compute_pipeline(ComputePipelineHandle pipeline) noexcept = 0;

    [[nodiscard]] virtual BufferHandle create_buffer(const BufferDesc& desc) noexcept = 0;
    // Copies `size` bytes into `buffer` at `offset` and waits for the copy:
    // for loading, not for per-frame streaming.
    [[nodiscard]] virtual bool upload_buffer(BufferHandle buffer, const void* data, std::uint32_t size,
                                             std::uint32_t offset = 0) noexcept = 0;
    [[nodiscard]] BufferHandle create_buffer_with_data(BufferUsage usage, const void* data,
                                                       std::uint32_t size) noexcept;
    // Copies `size` bytes out of `buffer` at `offset` and waits for the GPU
    // to finish everything submitted so far: a readback for tests and tools,
    // never for a frame.
    [[nodiscard]] virtual bool download_buffer(BufferHandle buffer, void* out, std::uint32_t size,
                                               std::uint32_t offset = 0) noexcept = 0;
    virtual void destroy_buffer(BufferHandle buffer) noexcept = 0;

    [[nodiscard]] virtual TextureHandle create_texture(const TextureDesc& desc) noexcept = 0;
    // {0, 0} for an invalid handle.
    [[nodiscard]] virtual Extent2D texture_extent(TextureHandle texture) const noexcept = 0;
    // Uploads tightly packed pixels for one mip level and waits for the copy.
    // `size` must equal width * height * bytes_per_pixel at that level.
    [[nodiscard]] virtual bool upload_texture(TextureHandle texture, const void* pixels, std::uint32_t size,
                                              std::uint32_t mip_level = 0) noexcept = 0;
    // Fills levels 1..n from level 0 on the GPU and waits. The texture needs
    // more than one level and ColorTarget usage.
    [[nodiscard]] virtual bool generate_mipmaps(TextureHandle texture) noexcept = 0;
    // Copies level 0 of a color texture out, tightly packed, and waits for
    // the GPU to finish everything submitted so far: a readback for tests,
    // never for a frame. `size` must equal width * height * bytes_per_pixel.
    [[nodiscard]] virtual bool download_texture(TextureHandle texture, void* out,
                                                std::uint32_t size) noexcept = 0;
    // A sampled color texture with a full mip chain: created, uploaded, mipmapped.
    [[nodiscard]] TextureHandle create_texture_with_data(TextureFormat format, std::uint32_t width,
                                                         std::uint32_t height, const void* pixels,
                                                         std::uint32_t size, bool mipmaps = true) noexcept;
    virtual void destroy_texture(TextureHandle texture) noexcept = 0;

    [[nodiscard]] virtual SamplerHandle create_sampler(const SamplerDesc& desc) noexcept = 0;
    virtual void destroy_sampler(SamplerHandle sampler) noexcept = 0;

    // Live objects per pool: a leak check, and a view of the budgets.
    struct ResourceCounts {
        std::uint32_t shaders = 0, pipelines = 0, compute_pipelines = 0, buffers = 0, textures = 0,
                      samplers = 0;
    };
    [[nodiscard]] virtual ResourceCounts resource_counts() const noexcept = 0;
    // What the GPU is doing: the time the last completed frame's commands
    // took on it, and the memory the device holds for our resources. Zero
    // where a backend cannot say (SDL GPU).
    struct GpuStats {
        double frame_ms = 0.0;
        std::uint64_t allocated_bytes = 0;
    };
    [[nodiscard]] virtual GpuStats gpu_stats() const noexcept = 0;

    // Acquires this frame's command buffer and swapchain image; with vsync on
    // this is where the loop waits for the display. nullopt on error. With no
    // window attached the frame has no image: compute and copies only.
    [[nodiscard]] virtual std::optional<Frame> begin_frame() noexcept = 0;

protected:
    Device() = default;
    // What Frame, RenderPass and ComputePass call: the backend's side of each
    // of their methods, with the backend's own objects passed back in.
    friend class Frame;
    friend class RenderPass;
    friend class ComputePass;
    [[nodiscard]] virtual std::optional<RenderPass> frame_begin_pass(void* command_buffer,
                                                                     const RenderPassDesc& desc) noexcept = 0;
    [[nodiscard]] virtual std::optional<ComputePass>
    frame_begin_compute_pass(void* command_buffer, const ComputePassDesc& desc) noexcept = 0;
    [[nodiscard]] virtual bool frame_write_buffer(void* command_buffer, BufferHandle buffer, const void* data,
                                                  std::uint32_t size, std::uint32_t offset) noexcept = 0;
    virtual void frame_submit(void* command_buffer, TextureHandle swapchain) noexcept = 0;

    virtual void pass_bind_pipeline(void* pass, PipelineHandle pipeline) noexcept = 0;
    virtual void pass_bind_vertex_buffer(void* pass, BufferHandle buffer, std::uint32_t offset) noexcept = 0;
    virtual void pass_bind_index_buffer(void* pass, BufferHandle buffer, IndexType type,
                                        std::uint32_t offset) noexcept = 0;
    virtual void pass_bind_fragment_texture(void* pass, std::uint32_t slot, TextureHandle texture,
                                            SamplerHandle sampler) noexcept = 0;
    virtual void pass_bind_fragment_storage_buffer(void* pass, std::uint32_t slot,
                                                   BufferHandle buffer) noexcept = 0;
    virtual void pass_push_vertex_uniforms(void* command_buffer, void* pass, std::uint32_t slot,
                                           const void* data, std::uint32_t size) noexcept = 0;
    virtual void pass_push_fragment_uniforms(void* command_buffer, void* pass, std::uint32_t slot,
                                             const void* data, std::uint32_t size) noexcept = 0;
    virtual void pass_set_scissor(void* pass, std::uint32_t x, std::uint32_t y, std::uint32_t width,
                                  std::uint32_t height) noexcept = 0;
    virtual void pass_draw(void* pass, std::uint32_t vertex_count, std::uint32_t instance_count) noexcept = 0;
    virtual void pass_draw_indexed(void* pass, std::uint32_t index_count, std::uint32_t first_index,
                                   std::int32_t vertex_offset, std::uint32_t instance_count) noexcept = 0;
    virtual void pass_end(void* command_buffer, void* pass, bool labelled) noexcept = 0;

    virtual void compute_bind_pipeline(void* pass, ComputePipelineHandle pipeline) noexcept = 0;
    virtual void compute_bind_storage_buffer(void* pass, std::uint32_t slot,
                                             BufferHandle buffer) noexcept = 0;
    virtual void compute_push_uniforms(void* command_buffer, void* pass, std::uint32_t slot, const void* data,
                                       std::uint32_t size) noexcept = 0;
    virtual void compute_dispatch(void* pass, std::uint32_t groups_x, std::uint32_t groups_y,
                                  std::uint32_t groups_z) noexcept = 0;
    virtual void compute_end(void* command_buffer, void* pass, bool labelled) noexcept = 0;

    // For the backends, which cannot reach the private constructors themselves.
    static Frame make_frame(Device* device, void* command_buffer, TextureHandle swapchain,
                            std::uint32_t width, std::uint32_t height) noexcept {
        return Frame(device, command_buffer, swapchain, width, height);
    }
    static RenderPass make_render_pass(Device* device, void* command_buffer, void* pass,
                                       bool labelled) noexcept {
        return RenderPass(device, command_buffer, pass, labelled);
    }
    static ComputePass make_compute_pass(Device* device, void* command_buffer, void* pass,
                                         bool labelled) noexcept {
        return ComputePass(device, command_buffer, pass, labelled);
    }
};

} // namespace tynima::rhi
