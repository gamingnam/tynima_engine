// The native Metal backend: the engine's own path onto Apple GPUs, with
// nothing between the RHI calls and the Metal API. Same handles, same
// shader binding order as the SDL GPU backend (uniforms first at
// [[buffer(n)]], storage buffers after, the vertex buffer at [[buffer(14)]]),
// so every shader the engine has runs here unchanged. What this backend
// can do that SDL's cannot is what Phase 4 is about: keep attachments in
// tile memory and fuse the passes that share them.
//
// Objective-C++ under ARC. The pools hold Metal objects as retained void*
// (bridged in and out), so they stay plain C++ and nothing is released by
// surprise; frames get their own autorelease pool, pushed at begin_frame()
// and popped at submit(), for everything Metal autoreleases in between.
#include <tynima/core/assert.h>
#include <tynima/core/log.h>
#include <tynima/core/memory.h>
#include <tynima/core/profile.h>
#include <tynima/platform/window.h>

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL3/SDL.h>
#include <cstring>
#include <dispatch/dispatch.h>
#include <initializer_list>

#include "backend.h"

// The autorelease pool as a pair of calls rather than a block: a frame's
// pool is pushed in begin_frame() and popped in submit(). These are what
// @autoreleasepool compiles to; the runtime exports them, the headers do not.
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void* pool);

namespace tynima::rhi {

namespace {

constexpr std::uint32_t kFramesInFlight = 3;
constexpr std::uint32_t kVertexBufferIndex = 14; // SDL's convention; the shaders are written for it
constexpr std::uint32_t kStreamBytesPerFrame = 4 * 1024 * 1024;
constexpr std::uint32_t kMaxPushBytes = 4096; // setVertexBytes' limit

// ARC bridging: the pools keep Metal objects as retained void*.
template <typename T> void* retain(T object) noexcept {
    return (__bridge_retained void*)object;
}
template <typename T> T bridge(void* pointer) noexcept {
    return (__bridge T)pointer;
}
void release(void* pointer) noexcept {
    if (pointer != nullptr) {
        id object = (__bridge_transfer id)pointer;
        (void)object;
    }
}

struct Shader {
    void* function; // id<MTLFunction>
    std::uint32_t uniform_buffers;
    std::uint32_t storage_buffers;
};

struct GraphicsPipeline {
    void* state;       // id<MTLRenderPipelineState>
    void* depth_state; // id<MTLDepthStencilState>
    MTLPrimitiveType primitive;
    MTLCullMode cull;
    std::uint32_t vertex_uniforms;
    std::uint32_t fragment_uniforms;
};

struct ComputePipeline {
    void* state; // id<MTLComputePipelineState>
    MTLSize threads;
    std::uint32_t uniform_buffers;
    std::uint32_t readonly_buffers;
};

struct Buffer {
    void* buffer; // id<MTLBuffer>
    std::uint32_t size;
};

struct Texture {
    void* texture; // id<MTLTexture>
    Extent2D extent;
    TextureFormat format;
    std::uint32_t mip_levels;
    TextureUsage usage;
    bool borrowed;  // the drawable's: not ours to release
    void* drawable; // id<CAMetalDrawable>, borrowed textures only
};

struct Sampler {
    void* state; // id<MTLSamplerState>
};

// One per frame that can be in flight: its command buffer, its autorelease
// pool and its share of the streaming memory.
struct FrameState {
    void* command_buffer = nullptr; // id<MTLCommandBuffer>
    void* pool = nullptr;           // objc autorelease pool token
    void* stream = nullptr;         // id<MTLBuffer>, shared memory for write_buffer
    std::uint32_t stream_capacity = 0;
    std::uint32_t stream_used = 0;
    bool in_use = false;
};

MTLPixelFormat to_metal(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::Rgba8Unorm:
        return MTLPixelFormatRGBA8Unorm;
    case TextureFormat::Rgba8Srgb:
        return MTLPixelFormatRGBA8Unorm_sRGB;
    case TextureFormat::Bgra8Unorm:
        return MTLPixelFormatBGRA8Unorm;
    case TextureFormat::Bgra8Srgb:
        return MTLPixelFormatBGRA8Unorm_sRGB;
    case TextureFormat::Rgba16Float:
        return MTLPixelFormatRGBA16Float;
    case TextureFormat::Depth32Float:
        return MTLPixelFormatDepth32Float;
    case TextureFormat::Depth24Stencil8:
        return MTLPixelFormatDepth24Unorm_Stencil8;
    case TextureFormat::Depth16:
        return MTLPixelFormatDepth16Unorm;
    }
    return MTLPixelFormatInvalid;
}

MTLVertexFormat to_metal(VertexFormat format) noexcept {
    switch (format) {
    case VertexFormat::Float2:
        return MTLVertexFormatFloat2;
    case VertexFormat::Float3:
        return MTLVertexFormatFloat3;
    case VertexFormat::Float4:
        return MTLVertexFormatFloat4;
    }
    return MTLVertexFormatFloat3;
}

MTLPrimitiveType to_metal(PrimitiveTopology topology) noexcept {
    switch (topology) {
    case PrimitiveTopology::TriangleList:
        return MTLPrimitiveTypeTriangle;
    case PrimitiveTopology::TriangleStrip:
        return MTLPrimitiveTypeTriangleStrip;
    case PrimitiveTopology::LineList:
        return MTLPrimitiveTypeLine;
    }
    return MTLPrimitiveTypeTriangle;
}

MTLCullMode to_metal(CullMode mode) noexcept {
    switch (mode) {
    case CullMode::None:
        return MTLCullModeNone;
    case CullMode::Back:
        return MTLCullModeBack;
    case CullMode::Front:
        return MTLCullModeFront;
    }
    return MTLCullModeNone;
}

MTLCompareFunction to_metal(CompareOp op) noexcept {
    switch (op) {
    case CompareOp::Never:
        return MTLCompareFunctionNever;
    case CompareOp::Less:
        return MTLCompareFunctionLess;
    case CompareOp::Equal:
        return MTLCompareFunctionEqual;
    case CompareOp::LessEqual:
        return MTLCompareFunctionLessEqual;
    case CompareOp::Greater:
        return MTLCompareFunctionGreater;
    case CompareOp::NotEqual:
        return MTLCompareFunctionNotEqual;
    case CompareOp::GreaterEqual:
        return MTLCompareFunctionGreaterEqual;
    case CompareOp::Always:
        return MTLCompareFunctionAlways;
    }
    return MTLCompareFunctionAlways;
}

MTLSamplerMinMagFilter to_metal(Filter filter) noexcept {
    return filter == Filter::Nearest ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
}

MTLSamplerAddressMode to_metal(AddressMode mode) noexcept {
    switch (mode) {
    case AddressMode::Repeat:
        return MTLSamplerAddressModeRepeat;
    case AddressMode::MirroredRepeat:
        return MTLSamplerAddressModeMirrorRepeat;
    case AddressMode::ClampToEdge:
        return MTLSamplerAddressModeClampToEdge;
    }
    return MTLSamplerAddressModeRepeat;
}

MTLLoadAction to_metal(LoadOp op) noexcept {
    switch (op) {
    case LoadOp::Load:
        return MTLLoadActionLoad;
    case LoadOp::Clear:
        return MTLLoadActionClear;
    case LoadOp::DontCare:
        return MTLLoadActionDontCare;
    }
    return MTLLoadActionDontCare;
}

MTLStoreAction to_metal(StoreOp op) noexcept {
    return op == StoreOp::Store ? MTLStoreActionStore : MTLStoreActionDontCare;
}

void set_error(const char* what, NSError* error) noexcept {
    if (error != nil) {
        SDL_SetError("%s: %s", what, error.localizedDescription.UTF8String);
    } else {
        SDL_SetError("%s", what);
    }
}

} // namespace

class MetalDevice final : public Device {
public:
    MetalDevice(id<MTLDevice> device, const DeviceDesc& desc) noexcept;
    ~MetalDevice() override;

    bool attach_window(platform::Window& window) noexcept override;
    Backend backend() const noexcept override { return Backend::Metal; }
    const char* backend_name() const noexcept override { return "metal"; }
    ShaderFormat shader_format() const noexcept override { return ShaderFormat::Msl; }
    TextureFormat preferred_depth_format() const noexcept override { return TextureFormat::Depth32Float; }
    bool supports_texture(TextureFormat format, TextureUsage usage) const noexcept override;
    bool swapchain_is_linear() const noexcept override { return swapchain_linear_; }
    TextureFormat swapchain_format() const noexcept override { return swapchain_format_; }

    bool valid(ShaderHandle handle) const noexcept override;
    bool valid(PipelineHandle handle) const noexcept override;
    bool valid(ComputePipelineHandle handle) const noexcept override;
    bool valid(BufferHandle handle) const noexcept override;
    bool valid(TextureHandle handle) const noexcept override;
    bool valid(SamplerHandle handle) const noexcept override;

    ShaderHandle create_shader(const ShaderDesc& desc) noexcept override;
    void destroy_shader(ShaderHandle shader) noexcept override;
    PipelineHandle create_graphics_pipeline(const GraphicsPipelineDesc& desc) noexcept override;
    void destroy_graphics_pipeline(PipelineHandle pipeline) noexcept override;
    ComputePipelineHandle create_compute_pipeline(const ComputePipelineDesc& desc) noexcept override;
    void destroy_compute_pipeline(ComputePipelineHandle pipeline) noexcept override;
    BufferHandle create_buffer(const BufferDesc& desc) noexcept override;
    bool upload_buffer(BufferHandle buffer, const void* data, std::uint32_t size,
                       std::uint32_t offset) noexcept override;
    bool download_buffer(BufferHandle buffer, void* out, std::uint32_t size,
                         std::uint32_t offset) noexcept override;
    void destroy_buffer(BufferHandle buffer) noexcept override;
    TextureHandle create_texture(const TextureDesc& desc) noexcept override;
    Extent2D texture_extent(TextureHandle texture) const noexcept override;
    bool upload_texture(TextureHandle texture, const void* pixels, std::uint32_t size,
                        std::uint32_t mip_level) noexcept override;
    bool generate_mipmaps(TextureHandle texture) noexcept override;
    void destroy_texture(TextureHandle texture) noexcept override;
    SamplerHandle create_sampler(const SamplerDesc& desc) noexcept override;
    void destroy_sampler(SamplerHandle sampler) noexcept override;
    ResourceCounts resource_counts() const noexcept override;
    std::optional<Frame> begin_frame() noexcept override;

protected:
    std::optional<RenderPass> frame_begin_pass(void* frame, const RenderPassDesc& desc) noexcept override;
    std::optional<ComputePass> frame_begin_compute_pass(void* frame,
                                                        const ComputePassDesc& desc) noexcept override;
    bool frame_write_buffer(void* frame, BufferHandle buffer, const void* data, std::uint32_t size,
                            std::uint32_t offset) noexcept override;
    void frame_submit(void* frame, TextureHandle swapchain) noexcept override;
    void pass_bind_pipeline(void* pass, PipelineHandle pipeline) noexcept override;
    void pass_bind_vertex_buffer(void* pass, BufferHandle buffer, std::uint32_t offset) noexcept override;
    void pass_bind_index_buffer(void* pass, BufferHandle buffer, IndexType type,
                                std::uint32_t offset) noexcept override;
    void pass_bind_fragment_texture(void* pass, std::uint32_t slot, TextureHandle texture,
                                    SamplerHandle sampler) noexcept override;
    void pass_bind_fragment_storage_buffer(void* pass, std::uint32_t slot,
                                           BufferHandle buffer) noexcept override;
    void pass_push_vertex_uniforms(void* frame, void* pass, std::uint32_t slot, const void* data,
                                   std::uint32_t size) noexcept override;
    void pass_push_fragment_uniforms(void* frame, void* pass, std::uint32_t slot, const void* data,
                                     std::uint32_t size) noexcept override;
    void pass_draw(void* pass, std::uint32_t vertex_count, std::uint32_t instance_count) noexcept override;
    void pass_draw_indexed(void* pass, std::uint32_t index_count, std::uint32_t first_index,
                           std::int32_t vertex_offset, std::uint32_t instance_count) noexcept override;
    void pass_end(void* frame, void* pass, bool labelled) noexcept override;
    void compute_bind_pipeline(void* pass, ComputePipelineHandle pipeline) noexcept override;
    void compute_bind_storage_buffer(void* pass, std::uint32_t slot, BufferHandle buffer) noexcept override;
    void compute_push_uniforms(void* frame, void* pass, std::uint32_t slot, const void* data,
                               std::uint32_t size) noexcept override;
    void compute_dispatch(void* pass, std::uint32_t groups_x, std::uint32_t groups_y,
                          std::uint32_t groups_z) noexcept override;
    void compute_end(void* frame, void* pass, bool labelled) noexcept override;

private:
    struct Pools;
    [[nodiscard]] id<MTLBuffer> staging(const void* data, std::uint32_t size) noexcept;
    [[nodiscard]] bool run_blit_and_wait(void (^encode)(id<MTLBlitCommandEncoder>)) noexcept;
    [[nodiscard]] FrameState* frame_state(void* frame) const noexcept {
        return static_cast<FrameState*>(frame);
    }

    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    dispatch_semaphore_t in_flight_;
    Pools* pools_;
    FrameState frames_[kFramesInFlight + 1]{};
    std::uint32_t next_frame_ = 0;
    // The window, once attached.
    void* window_ = nullptr;     // SDL_Window*
    void* metal_view_ = nullptr; // SDL_MetalView
    CAMetalLayer* layer_ = nil;
    bool vsync_;
    bool want_linear_swapchain_;
    bool swapchain_linear_ = false;
    TextureFormat swapchain_format_ = TextureFormat::Bgra8Unorm;
    // What the current pass needs between calls: the pipeline's shape, and
    // the index buffer for the next indexed draw. One pass at a time.
    MTLPrimitiveType primitive_ = MTLPrimitiveTypeTriangle;
    std::uint32_t fragment_uniforms_ = 0;
    id<MTLBuffer> index_buffer_ = nil;
    MTLIndexType index_type_ = MTLIndexTypeUInt32;
    std::uint32_t index_offset_ = 0;
    // The compute pass's pipeline shape, and the buffers it writes, which
    // bind once the pipeline says where.
    std::uint32_t compute_uniforms_ = 0;
    std::uint32_t compute_readonly_ = 0;
    MTLSize compute_threads_{1, 1, 1};
    id<MTLBuffer> compute_writes_[kMaxComputeWrites] = {};
    std::uint32_t compute_write_count_ = 0;
};

struct MetalDevice::Pools {
    core::HandlePool<Shader, ShaderTag> shaders;
    core::HandlePool<GraphicsPipeline, PipelineTag> pipelines;
    core::HandlePool<ComputePipeline, ComputePipelineTag> compute_pipelines;
    core::HandlePool<Buffer, BufferTag> buffers;
    core::HandlePool<Texture, TextureTag> textures;
    core::HandlePool<Sampler, SamplerTag> samplers;

    explicit Pools(const DeviceDesc& desc) noexcept
        : shaders(desc.max_shaders), pipelines(desc.max_pipelines),
          compute_pipelines(desc.max_compute_pipelines), buffers(desc.max_buffers),
          textures(desc.max_textures), samplers(desc.max_samplers) {}
};

std::unique_ptr<Device> create_metal_device(const DeviceDesc& desc) {
    TY_EXTERNAL_ALLOCATIONS();
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
        SDL_SetError("rhi: no Metal device");
        return nullptr;
    }
    return std::unique_ptr<Device>(new MetalDevice(device, desc));
}

MetalDevice::MetalDevice(id<MTLDevice> device, const DeviceDesc& desc) noexcept
    : device_(device), queue_([device newCommandQueue]),
      in_flight_(dispatch_semaphore_create(kFramesInFlight)), pools_(new Pools(desc)), vsync_(desc.vsync),
      want_linear_swapchain_(desc.linear_swapchain) {
    queue_.label = @"tynima";
}

MetalDevice::~MetalDevice() {
    @autoreleasepool {
        TY_EXTERNAL_ALLOCATIONS();
        // Everything in flight lands before anything is released — and the
        // semaphore goes back to its starting count, which libdispatch insists on.
        for (std::uint32_t i = 0; i < kFramesInFlight; ++i) {
            dispatch_semaphore_wait(in_flight_, DISPATCH_TIME_FOREVER);
        }
        for (std::uint32_t i = 0; i < kFramesInFlight; ++i) {
            dispatch_semaphore_signal(in_flight_);
        }
        const ResourceCounts leaked = resource_counts();
        if (leaked.shaders + leaked.pipelines + leaked.compute_pipelines + leaked.buffers + leaked.textures +
                leaked.samplers >
            0) {
            TY_LOG_WARN("rhi",
                        "%u shader(s), %u pipeline(s), %u compute pipeline(s), %u buffer(s), %u texture(s), "
                        "%u sampler(s) were never destroyed",
                        leaked.shaders, leaked.pipelines, leaked.compute_pipelines, leaked.buffers,
                        leaked.textures, leaked.samplers);
        }
        pools_->shaders.for_each([&](ShaderHandle, Shader& s) { release(s.function); });
        pools_->pipelines.for_each([&](PipelineHandle, GraphicsPipeline& p) {
            release(p.state);
            release(p.depth_state);
        });
        pools_->compute_pipelines.for_each(
            [&](ComputePipelineHandle, ComputePipeline& p) { release(p.state); });
        pools_->buffers.for_each([&](BufferHandle, Buffer& b) { release(b.buffer); });
        pools_->textures.for_each([&](TextureHandle, Texture& t) {
            if (!t.borrowed) {
                release(t.texture);
            }
        });
        pools_->samplers.for_each([&](SamplerHandle, Sampler& s) { release(s.state); });
        delete pools_;
        for (FrameState& frame : frames_) {
            release(frame.stream);
        }
        if (metal_view_ != nullptr) {
            SDL_Metal_DestroyView(metal_view_);
        }
    }
}

bool MetalDevice::attach_window(platform::Window& window) noexcept {
    @autoreleasepool {
        TY_EXTERNAL_ALLOCATIONS();
        SDL_Window* handle = static_cast<SDL_Window*>(window.native_handle());
        SDL_MetalView view = SDL_Metal_CreateView(handle);
        if (view == nullptr) {
            return false;
        }
        layer_ = bridge<CAMetalLayer*>(SDL_Metal_GetLayer(view));
        if (layer_ == nil) {
            SDL_Metal_DestroyView(view);
            SDL_SetError("rhi: the window has no Metal layer");
            return false;
        }
        window_ = handle;
        metal_view_ = view;
        layer_.device = device_;
        swapchain_linear_ = want_linear_swapchain_;
        swapchain_format_ = swapchain_linear_ ? TextureFormat::Bgra8Srgb : TextureFormat::Bgra8Unorm;
        layer_.pixelFormat = to_metal(swapchain_format_);
        layer_.framebufferOnly = YES;
        layer_.displaySyncEnabled = vsync_ ? YES : NO;
        layer_.maximumDrawableCount = 3;
        return true;
    }
}

bool MetalDevice::supports_texture(TextureFormat format, TextureUsage) const noexcept {
    if (format == TextureFormat::Depth24Stencil8) {
        return device_.depth24Stencil8PixelFormatSupported == YES; // not on Apple silicon
    }
    return to_metal(format) != MTLPixelFormatInvalid;
}

bool MetalDevice::valid(ShaderHandle handle) const noexcept {
    return pools_->shaders.get(handle) != nullptr;
}
bool MetalDevice::valid(PipelineHandle handle) const noexcept {
    return pools_->pipelines.get(handle) != nullptr;
}
bool MetalDevice::valid(ComputePipelineHandle handle) const noexcept {
    return pools_->compute_pipelines.get(handle) != nullptr;
}
bool MetalDevice::valid(BufferHandle handle) const noexcept {
    return pools_->buffers.get(handle) != nullptr;
}
bool MetalDevice::valid(TextureHandle handle) const noexcept {
    return pools_->textures.get(handle) != nullptr;
}
bool MetalDevice::valid(SamplerHandle handle) const noexcept {
    return pools_->samplers.get(handle) != nullptr;
}

Device::ResourceCounts MetalDevice::resource_counts() const noexcept {
    return {pools_->shaders.size(), pools_->pipelines.size(), pools_->compute_pipelines.size(),
            pools_->buffers.size(), pools_->textures.size(),  pools_->samplers.size()};
}

// ------------------------------------------------------------------ shaders

ShaderHandle MetalDevice::create_shader(const ShaderDesc& desc) noexcept {
    @autoreleasepool {
        TY_EXTERNAL_ALLOCATIONS();
        if (desc.format != ShaderFormat::Msl) {
            SDL_SetError("rhi::Device::create_shader: the Metal backend takes MSL source");
            return {};
        }
        if (desc.code == nullptr || desc.code_size == 0) {
            SDL_SetError("rhi::Device::create_shader: no shader code");
            return {};
        }
        if (pools_->shaders.full()) {
            SDL_SetError("rhi: shader budget of %u exhausted", pools_->shaders.capacity());
            return {};
        }
        NSString* source = [[NSString alloc] initWithBytes:desc.code
                                                    length:desc.code_size
                                                  encoding:NSUTF8StringEncoding];
        if (source == nil) {
            SDL_SetError("rhi::Device::create_shader: the source is not UTF-8");
            return {};
        }
        NSError* error = nil;
        MTLCompileOptions* options = [MTLCompileOptions new];
        id<MTLLibrary> library = [device_ newLibraryWithSource:source options:options error:&error];
        if (library == nil) {
            set_error("rhi::Device::create_shader", error);
            return {};
        }
        id<MTLFunction> function =
            [library newFunctionWithName:[NSString stringWithUTF8String:desc.entry_point]];
        if (function == nil) {
            SDL_SetError("rhi::Device::create_shader: no function '%s' in the source", desc.entry_point);
            return {};
        }
        return pools_->shaders.create(
            Shader{retain(function), desc.num_uniform_buffers, desc.num_storage_buffers});
    }
}

void MetalDevice::destroy_shader(ShaderHandle shader) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    if (const Shader* object = pools_->shaders.get(shader)) {
        release(object->function);
        pools_->shaders.destroy(shader);
    }
}

// ---------------------------------------------------------------- pipelines

PipelineHandle MetalDevice::create_graphics_pipeline(const GraphicsPipelineDesc& desc) noexcept {
    @autoreleasepool {
        TY_EXTERNAL_ALLOCATIONS();
        if (desc.color_target_count > kMaxColorTargets) {
            SDL_SetError("rhi::Device::create_graphics_pipeline: %u color targets (at most %u)",
                         desc.color_target_count, kMaxColorTargets);
            return {};
        }
        if (desc.color_target_count == 0 && !desc.depth_format.has_value()) {
            SDL_SetError("rhi::Device::create_graphics_pipeline: a pipeline needs a color or depth target");
            return {};
        }
        for (std::uint32_t i = 0; i < desc.color_target_count; ++i) {
            if (is_depth_format(desc.color_formats[i])) {
                SDL_SetError("rhi::Device::create_graphics_pipeline: color target %u has a depth format", i);
                return {};
            }
        }
        if (desc.depth_format.has_value() && !is_depth_format(*desc.depth_format)) {
            SDL_SetError("rhi::Device::create_graphics_pipeline: the depth target has a color format");
            return {};
        }
        const Shader* vertex = pools_->shaders.get(desc.vertex_shader);
        const Shader* fragment = pools_->shaders.get(desc.fragment_shader);
        if (vertex == nullptr || fragment == nullptr) {
            SDL_SetError("rhi::Device::create_graphics_pipeline: both shaders are required and must be live");
            return {};
        }
        if (desc.vertex_layout.attribute_count > 16) {
            SDL_SetError("rhi::Device::create_graphics_pipeline: too many vertex attributes");
            return {};
        }
        if (pools_->pipelines.full()) {
            SDL_SetError("rhi: pipeline budget of %u exhausted", pools_->pipelines.capacity());
            return {};
        }

        MTLRenderPipelineDescriptor* pipeline = [MTLRenderPipelineDescriptor new];
        pipeline.vertexFunction = bridge<id<MTLFunction>>(vertex->function);
        pipeline.fragmentFunction = bridge<id<MTLFunction>>(fragment->function);
        if (desc.vertex_layout.stride > 0) {
            MTLVertexDescriptor* vertices = [MTLVertexDescriptor new];
            for (std::uint32_t i = 0; i < desc.vertex_layout.attribute_count; ++i) {
                const VertexAttribute& a = desc.vertex_layout.attributes[i];
                vertices.attributes[a.location].format = to_metal(a.format);
                vertices.attributes[a.location].offset = a.offset;
                vertices.attributes[a.location].bufferIndex = kVertexBufferIndex;
            }
            vertices.layouts[kVertexBufferIndex].stride = desc.vertex_layout.stride;
            vertices.layouts[kVertexBufferIndex].stepFunction = MTLVertexStepFunctionPerVertex;
            pipeline.vertexDescriptor = vertices;
        }
        for (std::uint32_t i = 0; i < desc.color_target_count; ++i) {
            pipeline.colorAttachments[i].pixelFormat = to_metal(desc.color_formats[i]);
        }
        if (desc.depth_format.has_value()) {
            pipeline.depthAttachmentPixelFormat = to_metal(*desc.depth_format);
            if (*desc.depth_format == TextureFormat::Depth24Stencil8) {
                pipeline.stencilAttachmentPixelFormat = pipeline.depthAttachmentPixelFormat;
            }
        }
        NSError* error = nil;
        id<MTLRenderPipelineState> state = [device_ newRenderPipelineStateWithDescriptor:pipeline
                                                                                   error:&error];
        if (state == nil) {
            set_error("rhi::Device::create_graphics_pipeline", error);
            return {};
        }
        MTLDepthStencilDescriptor* depth = [MTLDepthStencilDescriptor new];
        depth.depthCompareFunction =
            desc.depth.test ? to_metal(desc.depth.compare) : MTLCompareFunctionAlways;
        depth.depthWriteEnabled = desc.depth.write ? YES : NO;
        id<MTLDepthStencilState> depth_state = [device_ newDepthStencilStateWithDescriptor:depth];
        if (depth_state == nil) {
            SDL_SetError("rhi::Device::create_graphics_pipeline: depth state failed");
            return {};
        }
        return pools_->pipelines.create(GraphicsPipeline{retain(state), retain(depth_state),
                                                         to_metal(desc.topology), to_metal(desc.cull),
                                                         vertex->uniform_buffers, fragment->uniform_buffers});
    }
}

void MetalDevice::destroy_graphics_pipeline(PipelineHandle pipeline) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    if (const GraphicsPipeline* object = pools_->pipelines.get(pipeline)) {
        release(object->state);
        release(object->depth_state);
        pools_->pipelines.destroy(pipeline);
    }
}

ComputePipelineHandle MetalDevice::create_compute_pipeline(const ComputePipelineDesc& desc) noexcept {
    @autoreleasepool {
        TY_EXTERNAL_ALLOCATIONS();
        if (desc.format != ShaderFormat::Msl || desc.code == nullptr || desc.code_size == 0) {
            SDL_SetError("rhi::Device::create_compute_pipeline: the Metal backend takes MSL source");
            return {};
        }
        if (desc.threads_x == 0 || desc.threads_y == 0 || desc.threads_z == 0) {
            SDL_SetError("rhi::Device::create_compute_pipeline: a threadgroup dimension is zero");
            return {};
        }
        if (pools_->compute_pipelines.full()) {
            SDL_SetError("rhi: compute pipeline budget of %u exhausted",
                         pools_->compute_pipelines.capacity());
            return {};
        }
        NSString* source = [[NSString alloc] initWithBytes:desc.code
                                                    length:desc.code_size
                                                  encoding:NSUTF8StringEncoding];
        NSError* error = nil;
        id<MTLLibrary> library = [device_ newLibraryWithSource:source
                                                       options:[MTLCompileOptions new]
                                                         error:&error];
        if (library == nil) {
            set_error("rhi::Device::create_compute_pipeline", error);
            return {};
        }
        id<MTLFunction> function =
            [library newFunctionWithName:[NSString stringWithUTF8String:desc.entry_point]];
        if (function == nil) {
            SDL_SetError("rhi::Device::create_compute_pipeline: no function '%s' in the source",
                         desc.entry_point);
            return {};
        }
        id<MTLComputePipelineState> state = [device_ newComputePipelineStateWithFunction:function
                                                                                   error:&error];
        if (state == nil) {
            set_error("rhi::Device::create_compute_pipeline", error);
            return {};
        }
        const std::uint64_t threads = std::uint64_t{desc.threads_x} * desc.threads_y * desc.threads_z;
        if (threads > state.maxTotalThreadsPerThreadgroup) {
            SDL_SetError("rhi::Device::create_compute_pipeline: %llu threads a group, the GPU allows %lu",
                         static_cast<unsigned long long>(threads),
                         static_cast<unsigned long>(state.maxTotalThreadsPerThreadgroup));
            return {};
        }
        return pools_->compute_pipelines.create(
            ComputePipeline{retain(state), MTLSizeMake(desc.threads_x, desc.threads_y, desc.threads_z),
                            desc.num_uniform_buffers, desc.num_readonly_storage_buffers});
    }
}

void MetalDevice::destroy_compute_pipeline(ComputePipelineHandle pipeline) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    if (const ComputePipeline* object = pools_->compute_pipelines.get(pipeline)) {
        release(object->state);
        pools_->compute_pipelines.destroy(pipeline);
    }
}

// ------------------------------------------------------------------ buffers

// Unified memory: a shared buffer with the bytes in it, the source of a copy.
id<MTLBuffer> MetalDevice::staging(const void* data, std::uint32_t size) noexcept {
    id<MTLBuffer> buffer = [device_ newBufferWithLength:size options:MTLResourceStorageModeShared];
    if (buffer != nil && data != nullptr) {
        std::memcpy(buffer.contents, data, size);
    }
    return buffer;
}

// A command buffer of just the copies `encode` records, run to completion.
bool MetalDevice::run_blit_and_wait(void (^encode)(id<MTLBlitCommandEncoder>)) noexcept {
    @autoreleasepool {
        id<MTLCommandBuffer> command_buffer = [queue_ commandBuffer];
        if (command_buffer == nil) {
            SDL_SetError("rhi: no command buffer");
            return false;
        }
        id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
        encode(blit);
        [blit endEncoding];
        [command_buffer commit];
        [command_buffer waitUntilCompleted];
        if (command_buffer.status != MTLCommandBufferStatusCompleted) {
            set_error("rhi: the copy failed", command_buffer.error);
            return false;
        }
        return true;
    }
}

BufferHandle MetalDevice::create_buffer(const BufferDesc& desc) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    if (desc.size == 0) {
        SDL_SetError("rhi::Device::create_buffer: size must be non-zero");
        return {};
    }
    if (pools_->buffers.full()) {
        SDL_SetError("rhi: buffer budget of %u exhausted", pools_->buffers.capacity());
        return {};
    }
    // Private: the GPU's own memory, filled by copies. Every usage is the
    // same buffer here; Metal has no per-usage kinds.
    id<MTLBuffer> buffer = [device_ newBufferWithLength:desc.size options:MTLResourceStorageModePrivate];
    if (buffer == nil) {
        SDL_SetError("rhi::Device::create_buffer: %u bytes refused", desc.size);
        return {};
    }
    return pools_->buffers.create(Buffer{retain(buffer), desc.size});
}

bool MetalDevice::upload_buffer(BufferHandle buffer, const void* data, std::uint32_t size,
                                std::uint32_t offset) noexcept {
    TY_PROFILE_SCOPE_NAMED("rhi::upload_buffer");
    TY_EXTERNAL_ALLOCATIONS();
    const Buffer* object = pools_->buffers.get(buffer);
    if (object == nullptr) {
        SDL_SetError("rhi::Device::upload_buffer: null or destroyed buffer handle");
        return false;
    }
    if (size == 0 || offset + size > object->size) {
        SDL_SetError("rhi::Device::upload_buffer: %u bytes at %u do not fit a %u byte buffer", size, offset,
                     object->size);
        return false;
    }
    id<MTLBuffer> source = staging(data, size);
    if (source == nil) {
        SDL_SetError("rhi::Device::upload_buffer: staging memory refused");
        return false;
    }
    id<MTLBuffer> destination = bridge<id<MTLBuffer>>(object->buffer);
    return run_blit_and_wait(^(id<MTLBlitCommandEncoder> blit) {
      [blit copyFromBuffer:source sourceOffset:0 toBuffer:destination destinationOffset:offset size:size];
    });
}

bool MetalDevice::download_buffer(BufferHandle buffer, void* out, std::uint32_t size,
                                  std::uint32_t offset) noexcept {
    TY_PROFILE_SCOPE_NAMED("rhi::download_buffer");
    TY_EXTERNAL_ALLOCATIONS();
    const Buffer* object = pools_->buffers.get(buffer);
    if (object == nullptr) {
        SDL_SetError("rhi::Device::download_buffer: null or destroyed buffer handle");
        return false;
    }
    if (size == 0 || offset + size > object->size) {
        SDL_SetError("rhi::Device::download_buffer: %u bytes at %u do not fit a %u byte buffer", size, offset,
                     object->size);
        return false;
    }
    id<MTLBuffer> destination = staging(nullptr, size);
    if (destination == nil) {
        SDL_SetError("rhi::Device::download_buffer: staging memory refused");
        return false;
    }
    id<MTLBuffer> source = bridge<id<MTLBuffer>>(object->buffer);
    // Everything submitted so far runs before this copy: one queue, in order.
    if (!run_blit_and_wait(^(id<MTLBlitCommandEncoder> blit) {
          [blit copyFromBuffer:source sourceOffset:offset toBuffer:destination destinationOffset:0 size:size];
        })) {
        return false;
    }
    std::memcpy(out, destination.contents, size);
    return true;
}

void MetalDevice::destroy_buffer(BufferHandle buffer) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    if (const Buffer* object = pools_->buffers.get(buffer)) {
        release(object->buffer);
        pools_->buffers.destroy(buffer);
    }
}

// ----------------------------------------------------------------- textures

TextureHandle MetalDevice::create_texture(const TextureDesc& desc) noexcept {
    @autoreleasepool {
        TY_EXTERNAL_ALLOCATIONS();
        if (desc.width == 0 || desc.height == 0) {
            SDL_SetError("rhi::Device::create_texture: size must be non-zero");
            return {};
        }
        if (pools_->textures.full()) {
            SDL_SetError("rhi: texture budget of %u exhausted", pools_->textures.capacity());
            return {};
        }
        TextureUsage usage = desc.usage;
        if (usage == TextureUsage::Default) {
            usage = is_depth_format(desc.format) ? TextureUsage::DepthStencilTarget : TextureUsage::Sampled;
        }
        const std::uint32_t levels =
            desc.mip_levels == 0 ? mip_level_count(desc.width, desc.height) : desc.mip_levels;
        MTLTextureDescriptor* texture =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:to_metal(desc.format)
                                                               width:desc.width
                                                              height:desc.height
                                                           mipmapped:levels > 1];
        texture.mipmapLevelCount = levels;
        texture.storageMode = MTLStorageModePrivate;
        MTLTextureUsage metal_usage = 0;
        if (has_usage(usage, TextureUsage::Sampled)) {
            metal_usage |= MTLTextureUsageShaderRead;
        }
        if (has_usage(usage, TextureUsage::ColorTarget) ||
            has_usage(usage, TextureUsage::DepthStencilTarget)) {
            metal_usage |= MTLTextureUsageRenderTarget;
        }
        texture.usage = metal_usage;
        id<MTLTexture> handle = [device_ newTextureWithDescriptor:texture];
        if (handle == nil) {
            SDL_SetError("rhi::Device::create_texture: a %ux%u %s texture was refused", desc.width,
                         desc.height, texture_format_name(desc.format));
            return {};
        }
        return pools_->textures.create(
            Texture{retain(handle), {desc.width, desc.height}, desc.format, levels, usage, false, nullptr});
    }
}

Extent2D MetalDevice::texture_extent(TextureHandle texture) const noexcept {
    const Texture* object = pools_->textures.get(texture);
    return object != nullptr ? object->extent : Extent2D{};
}

bool MetalDevice::upload_texture(TextureHandle texture, const void* pixels, std::uint32_t size,
                                 std::uint32_t mip_level) noexcept {
    TY_PROFILE_SCOPE_NAMED("rhi::upload_texture");
    TY_EXTERNAL_ALLOCATIONS();
    const Texture* object = pools_->textures.get(texture);
    if (object == nullptr || object->borrowed) {
        SDL_SetError("rhi::Device::upload_texture: null, destroyed or borrowed texture handle");
        return false;
    }
    if (mip_level >= object->mip_levels) {
        SDL_SetError("rhi::Device::upload_texture: mip level %u of %u", mip_level, object->mip_levels);
        return false;
    }
    if (is_depth_format(object->format)) {
        SDL_SetError("rhi::Device::upload_texture: depth textures are rendered to, not uploaded");
        return false;
    }
    const Extent2D level{object->extent.width >> mip_level > 0 ? object->extent.width >> mip_level : 1,
                         object->extent.height >> mip_level > 0 ? object->extent.height >> mip_level : 1};
    const std::uint32_t row = level.width * bytes_per_pixel(object->format);
    const std::uint32_t expected = row * level.height;
    if (expected == 0 || size != expected) {
        SDL_SetError("rhi::Device::upload_texture: level %u wants %u bytes, got %u", mip_level, expected,
                     size);
        return false;
    }
    id<MTLBuffer> source = staging(pixels, size);
    if (source == nil) {
        SDL_SetError("rhi::Device::upload_texture: staging memory refused");
        return false;
    }
    id<MTLTexture> destination = bridge<id<MTLTexture>>(object->texture);
    return run_blit_and_wait(^(id<MTLBlitCommandEncoder> blit) {
      [blit copyFromBuffer:source
                 sourceOffset:0
            sourceBytesPerRow:row
          sourceBytesPerImage:size
                   sourceSize:MTLSizeMake(level.width, level.height, 1)
                    toTexture:destination
             destinationSlice:0
             destinationLevel:mip_level
            destinationOrigin:MTLOriginMake(0, 0, 0)];
    });
}

bool MetalDevice::generate_mipmaps(TextureHandle texture) noexcept {
    TY_PROFILE_SCOPE_NAMED("rhi::generate_mipmaps");
    TY_EXTERNAL_ALLOCATIONS();
    const Texture* object = pools_->textures.get(texture);
    if (object == nullptr || object->borrowed) {
        SDL_SetError("rhi::Device::generate_mipmaps: null, destroyed or borrowed texture handle");
        return false;
    }
    if (object->mip_levels <= 1) {
        SDL_SetError("rhi::Device::generate_mipmaps: the texture has a single level");
        return false;
    }
    if (!has_usage(object->usage, TextureUsage::ColorTarget)) {
        SDL_SetError("rhi::Device::generate_mipmaps: the texture needs ColorTarget usage");
        return false;
    }
    id<MTLTexture> target = bridge<id<MTLTexture>>(object->texture);
    return run_blit_and_wait(^(id<MTLBlitCommandEncoder> blit) {
      [blit generateMipmapsForTexture:target];
    });
}

void MetalDevice::destroy_texture(TextureHandle texture) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    if (const Texture* object = pools_->textures.get(texture)) {
        if (!object->borrowed) {
            release(object->texture);
        }
        pools_->textures.destroy(texture);
    }
}

// ----------------------------------------------------------------- samplers

SamplerHandle MetalDevice::create_sampler(const SamplerDesc& desc) noexcept {
    @autoreleasepool {
        TY_EXTERNAL_ALLOCATIONS();
        if (pools_->samplers.full()) {
            SDL_SetError("rhi: sampler budget of %u exhausted", pools_->samplers.capacity());
            return {};
        }
        MTLSamplerDescriptor* sampler = [MTLSamplerDescriptor new];
        sampler.minFilter = to_metal(desc.min_filter);
        sampler.magFilter = to_metal(desc.mag_filter);
        sampler.mipFilter =
            desc.mip_filter == Filter::Nearest ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
        sampler.sAddressMode = to_metal(desc.address_u);
        sampler.tAddressMode = to_metal(desc.address_v);
        sampler.rAddressMode = MTLSamplerAddressModeRepeat;
        sampler.maxAnisotropy = desc.max_anisotropy > 1.0f ? static_cast<NSUInteger>(desc.max_anisotropy) : 1;
        sampler.normalizedCoordinates = YES;
        if (desc.compare.has_value()) {
            sampler.compareFunction = to_metal(*desc.compare);
        }
        id<MTLSamplerState> state = [device_ newSamplerStateWithDescriptor:sampler];
        if (state == nil) {
            SDL_SetError("rhi::Device::create_sampler: refused");
            return {};
        }
        return pools_->samplers.create(Sampler{retain(state)});
    }
}

void MetalDevice::destroy_sampler(SamplerHandle sampler) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    if (const Sampler* object = pools_->samplers.get(sampler)) {
        release(object->state);
        pools_->samplers.destroy(sampler);
    }
}

// ------------------------------------------------------------------- frames

std::optional<Frame> MetalDevice::begin_frame() noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    // At most kFramesInFlight frames on the GPU at once: the wait is here.
    {
        TY_PROFILE_SCOPE_NAMED("rhi::frames in flight (wait)");
        dispatch_semaphore_wait(in_flight_, DISPATCH_TIME_FOREVER);
    }
    FrameState* frame = nullptr;
    for (std::uint32_t i = 0; i < kFramesInFlight + 1 && frame == nullptr; ++i) {
        FrameState& candidate = frames_[(next_frame_ + i) % (kFramesInFlight + 1)];
        if (!candidate.in_use) {
            frame = &candidate;
            next_frame_ = (next_frame_ + i + 1) % (kFramesInFlight + 1);
        }
    }
    TY_ASSERT(frame != nullptr, "rhi: more frames open than can be in flight");
    if (frame == nullptr) {
        dispatch_semaphore_signal(in_flight_);
        SDL_SetError("rhi::Device::begin_frame: a frame is still open");
        return std::nullopt;
    }
    frame->pool = objc_autoreleasePoolPush();
    id<MTLCommandBuffer> command_buffer = [queue_ commandBuffer];
    if (command_buffer == nil) {
        objc_autoreleasePoolPop(frame->pool);
        frame->pool = nullptr;
        dispatch_semaphore_signal(in_flight_);
        SDL_SetError("rhi::Device::begin_frame: no command buffer");
        return std::nullopt;
    }
    command_buffer.label = @"frame";
    frame->command_buffer = retain(command_buffer);
    frame->stream_used = 0;
    frame->in_use = true;

    TextureHandle swapchain;
    std::uint32_t width = 0, height = 0;
    if (layer_ != nil) {
        // The layer follows the window's pixel size; the drawable is the
        // image this frame presents, and it is only ours until then.
        int pixel_width = 0, pixel_height = 0;
        SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(window_), &pixel_width, &pixel_height);
        if (pixel_width > 0 && pixel_height > 0) {
            const CGSize size =
                CGSizeMake(static_cast<double>(pixel_width), static_cast<double>(pixel_height));
            if (layer_.drawableSize.width != size.width || layer_.drawableSize.height != size.height) {
                layer_.drawableSize = size;
            }
        }
        TY_PROFILE_SCOPE_NAMED("rhi::next drawable (vsync wait)");
        id<CAMetalDrawable> drawable = [layer_ nextDrawable];
        if (drawable != nil) {
            id<MTLTexture> texture = drawable.texture;
            width = static_cast<std::uint32_t>(texture.width);
            height = static_cast<std::uint32_t>(texture.height);
            swapchain = pools_->textures.create(Texture{(__bridge void*)texture,
                                                        {width, height},
                                                        swapchain_format_,
                                                        1,
                                                        TextureUsage::ColorTarget,
                                                        true,
                                                        retain(drawable)});
            if (!swapchain) {
                SDL_SetError("rhi: texture budget of %u exhausted, no room for the swapchain image",
                             pools_->textures.capacity());
            }
        }
    }
    return make_frame(this, frame, swapchain, width, height);
}

void MetalDevice::frame_submit(void* frame_pointer, TextureHandle swapchain) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    TY_PROFILE_SCOPE_NAMED("rhi::Frame::submit");
    FrameState* frame = frame_state(frame_pointer);
    id<MTLCommandBuffer> command_buffer = bridge<id<MTLCommandBuffer>>(frame->command_buffer);
    void* drawable = nullptr;
    if (const Texture* object = pools_->textures.get(swapchain)) {
        drawable = object->drawable;
        pools_->textures.destroy(swapchain);
    }
    if (drawable != nullptr) {
        [command_buffer presentDrawable:bridge<id<CAMetalDrawable>>(drawable)];
    }
    dispatch_semaphore_t in_flight = in_flight_;
    [command_buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
      dispatch_semaphore_signal(in_flight);
    }];
    [command_buffer commit];
    release(drawable);
    release(frame->command_buffer);
    frame->command_buffer = nullptr;
    frame->in_use = false;
    objc_autoreleasePoolPop(frame->pool);
    frame->pool = nullptr;
}

bool MetalDevice::frame_write_buffer(void* frame_pointer, BufferHandle buffer, const void* data,
                                     std::uint32_t size, std::uint32_t offset) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    FrameState* frame = frame_state(frame_pointer);
    const Buffer* object = pools_->buffers.get(buffer);
    if (object == nullptr) {
        SDL_SetError("rhi::Frame::write_buffer: null or destroyed buffer handle");
        return false;
    }
    if (size == 0 || offset + size > object->size) {
        SDL_SetError("rhi::Frame::write_buffer: %u bytes at %u do not fit a %u byte buffer", size, offset,
                     object->size);
        return false;
    }
    // The frame's streaming memory: shared, written by the CPU here, read by
    // the copy below on the GPU; free again once the frame's command buffer
    // completes, which the in-flight limit guarantees before it is reused.
    // Grown when a frame needs more; the old buffer stays alive for the
    // copies already recorded from it.
    if (frame->stream == nullptr || frame->stream_used + size > frame->stream_capacity) {
        std::uint32_t capacity = frame->stream_capacity > 0 ? frame->stream_capacity : kStreamBytesPerFrame;
        while (frame->stream_used + size > capacity) {
            capacity *= 2;
        }
        id<MTLBuffer> stream = [device_ newBufferWithLength:capacity options:MTLResourceStorageModeShared];
        if (stream == nil) {
            SDL_SetError("rhi::Frame::write_buffer: %u bytes of streaming memory refused", capacity);
            return false;
        }
        release(frame->stream);
        frame->stream = retain(stream);
        frame->stream_capacity = capacity;
        frame->stream_used = 0;
    }
    id<MTLBuffer> stream = bridge<id<MTLBuffer>>(frame->stream);
    const std::uint32_t at = frame->stream_used;
    std::memcpy(static_cast<unsigned char*>(stream.contents) + at, data, size);
    frame->stream_used += (size + 255u) & ~255u; // keep each copy's source 256-byte aligned

    id<MTLCommandBuffer> command_buffer = bridge<id<MTLCommandBuffer>>(frame->command_buffer);
    id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
    [blit copyFromBuffer:stream
             sourceOffset:at
                 toBuffer:bridge<id<MTLBuffer>>(object->buffer)
        destinationOffset:offset
                     size:size];
    [blit endEncoding];
    return true;
}

// ------------------------------------------------------------- render passes

std::optional<RenderPass> MetalDevice::frame_begin_pass(void* frame_pointer,
                                                        const RenderPassDesc& desc) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    if (desc.color_count > kMaxColorTargets) {
        SDL_SetError("rhi::Frame::begin_pass: %u color attachments (at most %u)", desc.color_count,
                     kMaxColorTargets);
        return std::nullopt;
    }
    if (desc.color_count == 0 && !desc.depth.has_value()) {
        SDL_SetError("rhi::Frame::begin_pass: a pass needs at least one attachment");
        return std::nullopt;
    }
    Extent2D extent{};
    const auto resolve = [&](TextureHandle handle, TextureUsage needed, const char* what) -> const Texture* {
        const Texture* object = pools_->textures.get(handle);
        TY_ASSERT(object != nullptr, "rhi::Frame::begin_pass: null or destroyed attachment handle");
        if (object == nullptr) {
            SDL_SetError("rhi::Frame::begin_pass: %s attachment is null or destroyed", what);
            return nullptr;
        }
        if (!has_usage(object->usage, needed)) {
            SDL_SetError("rhi::Frame::begin_pass: %s attachment lacks the usage for it", what);
            return nullptr;
        }
        if (extent.width == 0) {
            extent = object->extent;
        } else if (extent.width != object->extent.width || extent.height != object->extent.height) {
            SDL_SetError("rhi::Frame::begin_pass: attachments differ in size (%ux%u vs %ux%u)", extent.width,
                         extent.height, object->extent.width, object->extent.height);
            return nullptr;
        }
        return object;
    };

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    for (std::uint32_t i = 0; i < desc.color_count; ++i) {
        const ColorAttachment& a = desc.colors[i];
        const Texture* object = resolve(a.texture, TextureUsage::ColorTarget, "color");
        if (object == nullptr) {
            return std::nullopt;
        }
        MTLRenderPassColorAttachmentDescriptor* attachment = pass.colorAttachments[i];
        attachment.texture = bridge<id<MTLTexture>>(object->texture);
        attachment.loadAction = to_metal(a.load);
        attachment.storeAction = to_metal(a.store);
        attachment.clearColor =
            MTLClearColorMake(static_cast<double>(a.clear.r), static_cast<double>(a.clear.g),
                              static_cast<double>(a.clear.b), static_cast<double>(a.clear.a));
    }
    if (desc.depth.has_value()) {
        const DepthAttachment& a = *desc.depth;
        const Texture* object = resolve(a.texture, TextureUsage::DepthStencilTarget, "depth");
        if (object == nullptr) {
            return std::nullopt;
        }
        pass.depthAttachment.texture = bridge<id<MTLTexture>>(object->texture);
        pass.depthAttachment.loadAction = to_metal(a.load);
        pass.depthAttachment.storeAction = to_metal(a.store);
        pass.depthAttachment.clearDepth = static_cast<double>(a.clear);
        if (object->format == TextureFormat::Depth24Stencil8) {
            pass.stencilAttachment.texture = pass.depthAttachment.texture;
            pass.stencilAttachment.loadAction = MTLLoadActionDontCare;
            pass.stencilAttachment.storeAction = MTLStoreActionDontCare;
        }
    }
    FrameState* frame = frame_state(frame_pointer);
    id<MTLCommandBuffer> command_buffer = bridge<id<MTLCommandBuffer>>(frame->command_buffer);
    id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
    if (encoder == nil) {
        SDL_SetError("rhi::Frame::begin_pass: no encoder");
        return std::nullopt;
    }
    const bool labelled = desc.name != nullptr;
    if (labelled) {
        NSString* name = [NSString stringWithUTF8String:desc.name];
        encoder.label = name;
        [encoder pushDebugGroup:name];
    }
    [encoder setFrontFacingWinding:MTLWindingCounterClockwise];
    index_buffer_ = nil;
    return make_render_pass(this, frame_pointer, retain(encoder), labelled);
}

void MetalDevice::pass_bind_pipeline(void* pass, PipelineHandle pipeline) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    const GraphicsPipeline* object = pools_->pipelines.get(pipeline);
    TY_ASSERT(object != nullptr, "bind_pipeline: null or destroyed pipeline handle");
    if (object == nullptr) {
        return;
    }
    id<MTLRenderCommandEncoder> encoder = bridge<id<MTLRenderCommandEncoder>>(pass);
    [encoder setRenderPipelineState:bridge<id<MTLRenderPipelineState>>(object->state)];
    [encoder setDepthStencilState:bridge<id<MTLDepthStencilState>>(object->depth_state)];
    [encoder setCullMode:object->cull];
    primitive_ = object->primitive;
    fragment_uniforms_ = object->fragment_uniforms;
}

void MetalDevice::pass_bind_vertex_buffer(void* pass, BufferHandle buffer, std::uint32_t offset) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    const Buffer* object = pools_->buffers.get(buffer);
    TY_ASSERT(object != nullptr, "bind_vertex_buffer: null or destroyed buffer handle");
    if (object != nullptr) {
        [bridge<id<MTLRenderCommandEncoder>>(pass) setVertexBuffer:bridge<id<MTLBuffer>>(object->buffer)
                                                            offset:offset
                                                           atIndex:kVertexBufferIndex];
    }
}

void MetalDevice::pass_bind_index_buffer(void*, BufferHandle buffer, IndexType type,
                                         std::uint32_t offset) noexcept {
    const Buffer* object = pools_->buffers.get(buffer);
    TY_ASSERT(object != nullptr, "bind_index_buffer: null or destroyed buffer handle");
    if (object != nullptr) {
        index_buffer_ = bridge<id<MTLBuffer>>(object->buffer);
        index_type_ = type == IndexType::Uint16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
        index_offset_ = offset;
    }
}

void MetalDevice::pass_bind_fragment_texture(void* pass, std::uint32_t slot, TextureHandle texture,
                                             SamplerHandle sampler) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    const Texture* texture_object = pools_->textures.get(texture);
    const Sampler* sampler_object = pools_->samplers.get(sampler);
    TY_ASSERT(texture_object != nullptr, "bind_fragment_texture: null or destroyed texture handle");
    TY_ASSERT(sampler_object != nullptr, "bind_fragment_texture: null or destroyed sampler handle");
    if (texture_object != nullptr && sampler_object != nullptr) {
        id<MTLRenderCommandEncoder> encoder = bridge<id<MTLRenderCommandEncoder>>(pass);
        [encoder setFragmentTexture:bridge<id<MTLTexture>>(texture_object->texture) atIndex:slot];
        [encoder setFragmentSamplerState:bridge<id<MTLSamplerState>>(sampler_object->state) atIndex:slot];
    }
}

void MetalDevice::pass_bind_fragment_storage_buffer(void* pass, std::uint32_t slot,
                                                    BufferHandle buffer) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    const Buffer* object = pools_->buffers.get(buffer);
    TY_ASSERT(object != nullptr, "bind_fragment_storage_buffer: null or destroyed buffer handle");
    if (object != nullptr) {
        [bridge<id<MTLRenderCommandEncoder>>(pass) setFragmentBuffer:bridge<id<MTLBuffer>>(object->buffer)
                                                              offset:0
                                                             atIndex:fragment_uniforms_ + slot];
    }
}

void MetalDevice::pass_push_vertex_uniforms(void*, void* pass, std::uint32_t slot, const void* data,
                                            std::uint32_t size) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    TY_ASSERT(size <= kMaxPushBytes, "push_vertex_uniforms: more than 4 KB; use a buffer");
    [bridge<id<MTLRenderCommandEncoder>>(pass) setVertexBytes:data length:size atIndex:slot];
}

void MetalDevice::pass_push_fragment_uniforms(void*, void* pass, std::uint32_t slot, const void* data,
                                              std::uint32_t size) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    TY_ASSERT(size <= kMaxPushBytes, "push_fragment_uniforms: more than 4 KB; use a buffer");
    [bridge<id<MTLRenderCommandEncoder>>(pass) setFragmentBytes:data length:size atIndex:slot];
}

void MetalDevice::pass_draw(void* pass, std::uint32_t vertex_count, std::uint32_t instance_count) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    [bridge<id<MTLRenderCommandEncoder>>(pass) drawPrimitives:primitive_
                                                  vertexStart:0
                                                  vertexCount:vertex_count
                                                instanceCount:instance_count];
}

void MetalDevice::pass_draw_indexed(void* pass, std::uint32_t index_count, std::uint32_t first_index,
                                    std::int32_t vertex_offset, std::uint32_t instance_count) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    TY_ASSERT(index_buffer_ != nil, "draw_indexed: no index buffer bound");
    if (index_buffer_ == nil) {
        return;
    }
    const std::uint32_t index_size = index_type_ == MTLIndexTypeUInt16 ? 2 : 4;
    [bridge<id<MTLRenderCommandEncoder>>(pass) drawIndexedPrimitives:primitive_
                                                          indexCount:index_count
                                                           indexType:index_type_
                                                         indexBuffer:index_buffer_
                                                   indexBufferOffset:index_offset_ + first_index * index_size
                                                       instanceCount:instance_count
                                                          baseVertex:vertex_offset
                                                        baseInstance:0];
}

void MetalDevice::pass_end(void*, void* pass, bool labelled) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    id<MTLRenderCommandEncoder> encoder = bridge<id<MTLRenderCommandEncoder>>(pass);
    if (labelled) {
        [encoder popDebugGroup];
    }
    [encoder endEncoding];
    index_buffer_ = nil;
    release(pass);
}

// ------------------------------------------------------------ compute passes

std::optional<ComputePass> MetalDevice::frame_begin_compute_pass(void* frame_pointer,
                                                                 const ComputePassDesc& desc) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    if (desc.write_count > kMaxComputeWrites) {
        SDL_SetError("rhi::Frame::begin_compute_pass: %u buffers written (at most %u)", desc.write_count,
                     kMaxComputeWrites);
        return std::nullopt;
    }
    for (std::uint32_t i = 0; i < desc.write_count; ++i) {
        const Buffer* object = pools_->buffers.get(desc.writes[i]);
        TY_ASSERT(object != nullptr, "rhi::Frame::begin_compute_pass: null or destroyed buffer handle");
        if (object == nullptr) {
            SDL_SetError("rhi::Frame::begin_compute_pass: written buffer %u is null or destroyed", i);
            return std::nullopt;
        }
        compute_writes_[i] = bridge<id<MTLBuffer>>(object->buffer);
    }
    compute_write_count_ = desc.write_count;
    FrameState* frame = frame_state(frame_pointer);
    id<MTLCommandBuffer> command_buffer = bridge<id<MTLCommandBuffer>>(frame->command_buffer);
    id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
        SDL_SetError("rhi::Frame::begin_compute_pass: no encoder");
        return std::nullopt;
    }
    const bool labelled = desc.name != nullptr;
    if (labelled) {
        NSString* name = [NSString stringWithUTF8String:desc.name];
        encoder.label = name;
        [encoder pushDebugGroup:name];
    }
    return make_compute_pass(this, frame_pointer, retain(encoder), labelled);
}

void MetalDevice::compute_bind_pipeline(void* pass, ComputePipelineHandle pipeline) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    const ComputePipeline* object = pools_->compute_pipelines.get(pipeline);
    TY_ASSERT(object != nullptr, "ComputePass::bind_pipeline: null or destroyed pipeline handle");
    if (object == nullptr) {
        return;
    }
    id<MTLComputeCommandEncoder> encoder = bridge<id<MTLComputeCommandEncoder>>(pass);
    [encoder setComputePipelineState:bridge<id<MTLComputePipelineState>>(object->state)];
    compute_uniforms_ = object->uniform_buffers;
    compute_readonly_ = object->readonly_buffers;
    compute_threads_ = object->threads;
    // The pass's written buffers go after the read-only ones — known only now.
    for (std::uint32_t i = 0; i < compute_write_count_; ++i) {
        [encoder setBuffer:compute_writes_[i] offset:0 atIndex:compute_uniforms_ + compute_readonly_ + i];
    }
}

void MetalDevice::compute_bind_storage_buffer(void* pass, std::uint32_t slot, BufferHandle buffer) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    const Buffer* object = pools_->buffers.get(buffer);
    TY_ASSERT(object != nullptr, "ComputePass::bind_storage_buffer: null or destroyed buffer handle");
    if (object != nullptr) {
        [bridge<id<MTLComputeCommandEncoder>>(pass) setBuffer:bridge<id<MTLBuffer>>(object->buffer)
                                                       offset:0
                                                      atIndex:compute_uniforms_ + slot];
    }
}

void MetalDevice::compute_push_uniforms(void*, void* pass, std::uint32_t slot, const void* data,
                                        std::uint32_t size) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    TY_ASSERT(size <= kMaxPushBytes, "ComputePass::push_uniforms: more than 4 KB; use a buffer");
    [bridge<id<MTLComputeCommandEncoder>>(pass) setBytes:data length:size atIndex:slot];
}

void MetalDevice::compute_dispatch(void* pass, std::uint32_t groups_x, std::uint32_t groups_y,
                                   std::uint32_t groups_z) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    [bridge<id<MTLComputeCommandEncoder>>(pass) dispatchThreadgroups:MTLSizeMake(groups_x, groups_y, groups_z)
                                               threadsPerThreadgroup:compute_threads_];
}

void MetalDevice::compute_end(void*, void* pass, bool labelled) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    id<MTLComputeCommandEncoder> encoder = bridge<id<MTLComputeCommandEncoder>>(pass);
    if (labelled) {
        [encoder popDebugGroup];
    }
    [encoder endEncoding];
    compute_write_count_ = 0;
    release(pass);
}

} // namespace tynima::rhi
