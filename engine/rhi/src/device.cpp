#include <tynima/rhi/device.h>

#include <tynima/core/assert.h>
#include <tynima/core/log.h>
#include <tynima/core/memory.h>
#include <tynima/core/profile.h>
#include <tynima/platform/window.h>

#include <SDL3/SDL.h>

#include <cstring>
#include <initializer_list>
#include <utility>

namespace tynima::rhi {

namespace {

struct Shader {
    SDL_GPUShader* handle;
};

struct GraphicsPipeline {
    SDL_GPUGraphicsPipeline* handle;
};

struct Buffer {
    SDL_GPUBuffer* handle;
    std::uint32_t size;
};

struct Texture {
    SDL_GPUTexture* handle;
    Extent2D extent;
    TextureFormat format;
    std::uint32_t mip_levels;
    TextureUsage usage;
    bool borrowed; // SDL's (the swapchain image): never released, uploaded to, or mipmapped by us
};

struct Sampler {
    SDL_GPUSampler* handle;
};

SDL_GPUDevice* dev(void* p) noexcept {
    return static_cast<SDL_GPUDevice*>(p);
}
SDL_Window* win(void* p) noexcept {
    return static_cast<SDL_Window*>(p);
}
SDL_GPUCommandBuffer* cmd(void* p) noexcept {
    return static_cast<SDL_GPUCommandBuffer*>(p);
}
SDL_GPURenderPass* rp(void* p) noexcept {
    return static_cast<SDL_GPURenderPass*>(p);
}

SDL_GPUShaderFormat to_sdl(ShaderFormat format) noexcept {
    switch (format) {
    case ShaderFormat::Msl: return SDL_GPU_SHADERFORMAT_MSL;
    case ShaderFormat::SpirV: return SDL_GPU_SHADERFORMAT_SPIRV;
    case ShaderFormat::Dxil: return SDL_GPU_SHADERFORMAT_DXIL;
    }
    return SDL_GPU_SHADERFORMAT_INVALID;
}

SDL_GPUPrimitiveType to_sdl(PrimitiveTopology topology) noexcept {
    switch (topology) {
    case PrimitiveTopology::TriangleList: return SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    case PrimitiveTopology::TriangleStrip: return SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
    case PrimitiveTopology::LineList: return SDL_GPU_PRIMITIVETYPE_LINELIST;
    }
    return SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
}

SDL_GPUVertexElementFormat to_sdl(VertexFormat format) noexcept {
    switch (format) {
    case VertexFormat::Float2: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    case VertexFormat::Float3: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    case VertexFormat::Float4: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    }
    return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
}

SDL_GPUCullMode to_sdl(CullMode mode) noexcept {
    switch (mode) {
    case CullMode::None: return SDL_GPU_CULLMODE_NONE;
    case CullMode::Back: return SDL_GPU_CULLMODE_BACK;
    case CullMode::Front: return SDL_GPU_CULLMODE_FRONT;
    }
    return SDL_GPU_CULLMODE_NONE;
}

SDL_GPUCompareOp to_sdl(CompareOp op) noexcept {
    switch (op) {
    case CompareOp::Never: return SDL_GPU_COMPAREOP_NEVER;
    case CompareOp::Less: return SDL_GPU_COMPAREOP_LESS;
    case CompareOp::Equal: return SDL_GPU_COMPAREOP_EQUAL;
    case CompareOp::LessEqual: return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    case CompareOp::Greater: return SDL_GPU_COMPAREOP_GREATER;
    case CompareOp::NotEqual: return SDL_GPU_COMPAREOP_NOT_EQUAL;
    case CompareOp::GreaterEqual: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    case CompareOp::Always: return SDL_GPU_COMPAREOP_ALWAYS;
    }
    return SDL_GPU_COMPAREOP_GREATER;
}

SDL_GPUTextureFormat to_sdl(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::Rgba8Unorm: return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    case TextureFormat::Rgba8Srgb: return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
    case TextureFormat::Bgra8Unorm: return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    case TextureFormat::Bgra8Srgb: return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
    case TextureFormat::Rgba16Float: return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    case TextureFormat::Depth32Float: return SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    case TextureFormat::Depth24Stencil8: return SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    case TextureFormat::Depth16: return SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    }
    return SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
}

// The swapchain's format, as one of ours; false for one we have no name for.
bool from_sdl(SDL_GPUTextureFormat format, TextureFormat& out) noexcept {
    for (const TextureFormat candidate : {TextureFormat::Rgba8Unorm, TextureFormat::Rgba8Srgb,
                                          TextureFormat::Bgra8Unorm, TextureFormat::Bgra8Srgb,
                                          TextureFormat::Rgba16Float}) {
        if (to_sdl(candidate) == format) {
            out = candidate;
            return true;
        }
    }
    return false;
}

SDL_GPULoadOp to_sdl(LoadOp op) noexcept {
    switch (op) {
    case LoadOp::Load: return SDL_GPU_LOADOP_LOAD;
    case LoadOp::Clear: return SDL_GPU_LOADOP_CLEAR;
    case LoadOp::DontCare: return SDL_GPU_LOADOP_DONT_CARE;
    }
    return SDL_GPU_LOADOP_DONT_CARE;
}

SDL_GPUStoreOp to_sdl(StoreOp op) noexcept {
    return op == StoreOp::Store ? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE;
}

SDL_GPUFilter to_sdl(Filter filter) noexcept {
    return filter == Filter::Nearest ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
}

SDL_GPUSamplerAddressMode to_sdl(AddressMode mode) noexcept {
    switch (mode) {
    case AddressMode::Repeat: return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    case AddressMode::MirroredRepeat: return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
    case AddressMode::ClampToEdge: return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    }
    return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
}

SDL_GPUTextureUsageFlags to_sdl(TextureUsage usage) noexcept {
    SDL_GPUTextureUsageFlags flags = 0;
    if (has_usage(usage, TextureUsage::Sampled)) {
        flags |= SDL_GPU_TEXTUREUSAGE_SAMPLER;
    }
    if (has_usage(usage, TextureUsage::ColorTarget)) {
        flags |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    }
    if (has_usage(usage, TextureUsage::DepthStencilTarget)) {
        flags |= SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    }
    return flags;
}

SDL_GPUBufferUsageFlags to_sdl(BufferUsage usage) noexcept {
    switch (usage) {
    case BufferUsage::Vertex: return SDL_GPU_BUFFERUSAGE_VERTEX;
    case BufferUsage::Index: return SDL_GPU_BUFFERUSAGE_INDEX;
    }
    return SDL_GPU_BUFFERUSAGE_VERTEX;
}

// A transfer buffer holding a copy of `data`, or nullptr.
SDL_GPUTransferBuffer* stage(SDL_GPUDevice* device, const void* data, std::uint32_t size) noexcept {
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = size;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &info);
    if (transfer == nullptr) {
        return nullptr;
    }
    void* mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    if (mapped == nullptr) {
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        return nullptr;
    }
    std::memcpy(mapped, data, size);
    SDL_UnmapGPUTransferBuffer(device, transfer);
    return transfer;
}

} // namespace

// One pool per resource type. Handles index into these; the Device's public
// functions resolve a handle to its SDL object or refuse the call.
struct Device::Pools {
    core::HandlePool<Shader, ShaderTag> shaders;
    core::HandlePool<GraphicsPipeline, PipelineTag> pipelines;
    core::HandlePool<Buffer, BufferTag> buffers;
    core::HandlePool<Texture, TextureTag> textures;
    core::HandlePool<Sampler, SamplerTag> samplers;

    explicit Pools(const DeviceDesc& desc) noexcept
        : shaders(desc.max_shaders), pipelines(desc.max_pipelines), buffers(desc.max_buffers),
          textures(desc.max_textures), samplers(desc.max_samplers) {}
};

std::uint32_t mip_level_count(std::uint32_t width, std::uint32_t height) noexcept {
    std::uint32_t levels = 1;
    for (std::uint32_t size = width > height ? width : height; size > 1; size >>= 1) {
        ++levels;
    }
    return levels;
}

const char* shader_format_name(ShaderFormat format) noexcept {
    switch (format) {
    case ShaderFormat::Msl: return "MSL";
    case ShaderFormat::SpirV: return "SPIR-V";
    case ShaderFormat::Dxil: return "DXIL";
    }
    return "unknown";
}

const char* texture_format_name(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::Rgba8Unorm: return "RGBA8_UNORM";
    case TextureFormat::Rgba8Srgb: return "RGBA8_SRGB";
    case TextureFormat::Bgra8Unorm: return "BGRA8_UNORM";
    case TextureFormat::Bgra8Srgb: return "BGRA8_SRGB";
    case TextureFormat::Rgba16Float: return "RGBA16_FLOAT";
    case TextureFormat::Depth32Float: return "D32_FLOAT";
    case TextureFormat::Depth24Stencil8: return "D24_UNORM_S8";
    case TextureFormat::Depth16: return "D16_UNORM";
    }
    return "unknown";
}

// ---------------------------------------------------------------- RenderPass

RenderPass::RenderPass(RenderPass&& other) noexcept
    : device_(other.device_), command_buffer_(other.command_buffer_),
      pass_(std::exchange(other.pass_, nullptr)), labelled_(other.labelled_) {}

RenderPass& RenderPass::operator=(RenderPass&& other) noexcept {
    if (this != &other) {
        end();
        device_ = other.device_;
        command_buffer_ = other.command_buffer_;
        pass_ = std::exchange(other.pass_, nullptr);
        labelled_ = other.labelled_;
    }
    return *this;
}

RenderPass::~RenderPass() {
    end();
}

void RenderPass::bind_pipeline(PipelineHandle pipeline) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    const GraphicsPipeline* object = device_->pools_->pipelines.get(pipeline);
    TY_ASSERT(object != nullptr, "bind_pipeline: null or destroyed pipeline handle");
    if (object != nullptr) {
        SDL_BindGPUGraphicsPipeline(rp(pass_), object->handle);
    }
}

void RenderPass::bind_vertex_buffer(BufferHandle buffer, std::uint32_t offset) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    const Buffer* object = device_->pools_->buffers.get(buffer);
    TY_ASSERT(object != nullptr, "bind_vertex_buffer: null or destroyed buffer handle");
    if (object != nullptr) {
        const SDL_GPUBufferBinding binding{object->handle, offset};
        SDL_BindGPUVertexBuffers(rp(pass_), 0, &binding, 1);
    }
}

void RenderPass::bind_index_buffer(BufferHandle buffer, IndexType type, std::uint32_t offset) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    const Buffer* object = device_->pools_->buffers.get(buffer);
    TY_ASSERT(object != nullptr, "bind_index_buffer: null or destroyed buffer handle");
    if (object != nullptr) {
        const SDL_GPUBufferBinding binding{object->handle, offset};
        SDL_BindGPUIndexBuffer(rp(pass_), &binding,
                               type == IndexType::Uint16 ? SDL_GPU_INDEXELEMENTSIZE_16BIT
                                                         : SDL_GPU_INDEXELEMENTSIZE_32BIT);
    }
}

void RenderPass::bind_fragment_texture(std::uint32_t slot, TextureHandle texture, SamplerHandle sampler) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    const Texture* texture_object = device_->pools_->textures.get(texture);
    const Sampler* sampler_object = device_->pools_->samplers.get(sampler);
    TY_ASSERT(texture_object != nullptr, "bind_fragment_texture: null or destroyed texture handle");
    TY_ASSERT(sampler_object != nullptr, "bind_fragment_texture: null or destroyed sampler handle");
    if (texture_object != nullptr && sampler_object != nullptr) {
        const SDL_GPUTextureSamplerBinding binding{texture_object->handle, sampler_object->handle};
        SDL_BindGPUFragmentSamplers(rp(pass_), slot, &binding, 1);
    }
}

void RenderPass::push_vertex_uniforms(std::uint32_t slot, const void* data, std::uint32_t size) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    SDL_PushGPUVertexUniformData(cmd(command_buffer_), slot, data, size);
}

void RenderPass::push_fragment_uniforms(std::uint32_t slot, const void* data, std::uint32_t size) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    SDL_PushGPUFragmentUniformData(cmd(command_buffer_), slot, data, size);
}

void RenderPass::draw(std::uint32_t vertex_count, std::uint32_t instance_count) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    SDL_DrawGPUPrimitives(rp(pass_), vertex_count, instance_count, 0, 0);
}

void RenderPass::draw_indexed(std::uint32_t index_count, std::uint32_t first_index, std::int32_t vertex_offset,
                              std::uint32_t instance_count) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    SDL_DrawGPUIndexedPrimitives(rp(pass_), index_count, instance_count, first_index, vertex_offset, 0);
}

void RenderPass::end() noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    if (pass_ != nullptr) {
        SDL_EndGPURenderPass(rp(pass_));
        pass_ = nullptr;
        if (labelled_) {
            SDL_PopGPUDebugGroup(cmd(command_buffer_));
        }
    }
}

// --------------------------------------------------------------------- Frame

Frame::Frame(Device* device, void* command_buffer, TextureHandle swapchain, std::uint32_t width,
             std::uint32_t height) noexcept
    : device_(device), command_buffer_(command_buffer), swapchain_(swapchain), width_(width),
      height_(height) {}

Frame::Frame(Frame&& other) noexcept
    : device_(other.device_), command_buffer_(std::exchange(other.command_buffer_, nullptr)),
      swapchain_(std::exchange(other.swapchain_, TextureHandle{})), width_(other.width_),
      height_(other.height_) {}

Frame& Frame::operator=(Frame&& other) noexcept {
    if (this != &other) {
        submit();
        device_ = other.device_;
        command_buffer_ = std::exchange(other.command_buffer_, nullptr);
        swapchain_ = std::exchange(other.swapchain_, TextureHandle{});
        width_ = other.width_;
        height_ = other.height_;
    }
    return *this;
}

Frame::~Frame() {
    submit();
}

std::optional<RenderPass> Frame::begin_pass(const RenderPassDesc& desc) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    if (command_buffer_ == nullptr) {
        SDL_SetError("rhi::Frame::begin_pass: the frame was submitted");
        return std::nullopt;
    }
    if (desc.color_count > kMaxColorTargets) {
        SDL_SetError("rhi::Frame::begin_pass: %u color attachments (at most %u)", desc.color_count,
                     kMaxColorTargets);
        return std::nullopt;
    }
    if (desc.color_count == 0 && !desc.depth.has_value()) {
        SDL_SetError("rhi::Frame::begin_pass: a pass needs at least one attachment");
        return std::nullopt;
    }

    // Every attachment resolves to a live texture of the right usage, and
    // they all agree on a size.
    Extent2D extent{};
    const auto resolve = [&](TextureHandle handle, TextureUsage needed, const char* what) -> const Texture* {
        const Texture* object = device_->pools_->textures.get(handle);
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

    SDL_GPUColorTargetInfo colors[kMaxColorTargets]{};
    for (std::uint32_t i = 0; i < desc.color_count; ++i) {
        const ColorAttachment& a = desc.colors[i];
        const Texture* object = resolve(a.texture, TextureUsage::ColorTarget, "color");
        if (object == nullptr) {
            return std::nullopt;
        }
        colors[i].texture = object->handle;
        colors[i].clear_color = SDL_FColor{a.clear.r, a.clear.g, a.clear.b, a.clear.a};
        colors[i].load_op = to_sdl(a.load);
        colors[i].store_op = to_sdl(a.store);
        // Not loading last time's contents means SDL may hand the pass a
        // fresh copy if the old one is still in flight — no stall. The
        // swapchain image is SDL's own and is never cycled.
        colors[i].cycle = !object->borrowed && a.load != LoadOp::Load;
    }
    SDL_GPUDepthStencilTargetInfo depth_info{};
    if (desc.depth.has_value()) {
        const DepthAttachment& a = *desc.depth;
        const Texture* object = resolve(a.texture, TextureUsage::DepthStencilTarget, "depth");
        if (object == nullptr) {
            return std::nullopt;
        }
        depth_info.texture = object->handle;
        depth_info.clear_depth = a.clear;
        depth_info.load_op = to_sdl(a.load);
        depth_info.store_op = to_sdl(a.store);
        depth_info.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        depth_info.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        depth_info.cycle = a.load != LoadOp::Load;
    }

    const bool labelled = desc.name != nullptr;
    if (labelled) {
        SDL_PushGPUDebugGroup(cmd(command_buffer_), desc.name);
    }
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd(command_buffer_), colors, desc.color_count,
                                                     desc.depth.has_value() ? &depth_info : nullptr);
    if (pass == nullptr) {
        if (labelled) {
            SDL_PopGPUDebugGroup(cmd(command_buffer_));
        }
        return std::nullopt;
    }
    return RenderPass(device_, command_buffer_, pass, labelled);
}

std::optional<RenderPass> Frame::begin_swapchain_pass(const ClearColor& clear, TextureHandle depth,
                                                      float depth_clear) noexcept {
    if (!swapchain_) {
        SDL_SetError("rhi::Frame::begin_swapchain_pass: no swapchain image this frame");
        return std::nullopt;
    }
    RenderPassDesc desc;
    desc.colors[0] = ColorAttachment{.texture = swapchain_, .load = LoadOp::Clear, .store = StoreOp::Store,
                                     .clear = clear};
    desc.color_count = 1;
    if (depth) {
        desc.depth = DepthAttachment{.texture = depth, .load = LoadOp::Clear, .store = StoreOp::DontCare,
                                     .clear = depth_clear};
    }
    return begin_pass(desc);
}

void Frame::submit() noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    if (command_buffer_ != nullptr) {
        TY_PROFILE_SCOPE_NAMED("rhi::Frame::submit");
        SDL_SubmitGPUCommandBuffer(cmd(command_buffer_));
        command_buffer_ = nullptr;
        if (swapchain_) {
            device_->pools_->textures.destroy(swapchain_); // the entry, not SDL's texture
            swapchain_ = {};
        }
    }
}

// -------------------------------------------------------------------- Device

Device::Device(void* device, const DeviceDesc& desc) noexcept
    : device_(device), pools_(new Pools(desc)), vsync_(desc.vsync), want_linear_swapchain_(desc.linear_swapchain) {}

std::unique_ptr<Device> Device::create(const DeviceDesc& desc) {
    TY_EXTERNAL_ALLOCATIONS();
    // The formats we can hand the backend today. SDL picks a backend that
    // consumes at least one of them.
    SDL_GPUDevice* device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, desc.debug, nullptr);
    if (device == nullptr) {
        return nullptr;
    }
    return std::unique_ptr<Device>(new Device(device, desc));
}

Device::~Device() {
    TY_EXTERNAL_ALLOCATIONS();
    SDL_WaitForGPUIdle(dev(device_));

    // Whatever is still alive is a leak by the caller; release it anyway so
    // the driver does not keep it, and say so.
    const ResourceCounts leaked = resource_counts();
    if (leaked.shaders + leaked.pipelines + leaked.buffers + leaked.textures + leaked.samplers > 0) {
        TY_LOG_WARN("rhi", "%u shader(s), %u pipeline(s), %u buffer(s), %u texture(s), %u sampler(s) were never "
                           "destroyed",
                    leaked.shaders, leaked.pipelines, leaked.buffers, leaked.textures, leaked.samplers);
    }
    pools_->pipelines.for_each([&](PipelineHandle, GraphicsPipeline& p) {
        SDL_ReleaseGPUGraphicsPipeline(dev(device_), p.handle);
    });
    pools_->shaders.for_each([&](ShaderHandle, Shader& s) { SDL_ReleaseGPUShader(dev(device_), s.handle); });
    pools_->buffers.for_each([&](BufferHandle, Buffer& b) { SDL_ReleaseGPUBuffer(dev(device_), b.handle); });
    pools_->textures.for_each([&](TextureHandle, Texture& t) { SDL_ReleaseGPUTexture(dev(device_), t.handle); });
    pools_->samplers.for_each([&](SamplerHandle, Sampler& s) { SDL_ReleaseGPUSampler(dev(device_), s.handle); });
    delete pools_;

    if (window_ != nullptr) {
        SDL_ReleaseWindowFromGPUDevice(dev(device_), win(window_));
    }
    SDL_DestroyGPUDevice(dev(device_));
}

bool Device::attach_window(platform::Window& window) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    SDL_Window* handle = static_cast<SDL_Window*>(window.native_handle());
    if (!SDL_ClaimWindowForGPUDevice(dev(device_), handle)) {
        return false;
    }
    window_ = handle;

    // Immediate present if asked for and available; otherwise stay with vsync rather than fail.
    SDL_GPUPresentMode present = SDL_GPU_PRESENTMODE_VSYNC;
    if (!vsync_ && SDL_WindowSupportsGPUPresentMode(dev(device_), handle, SDL_GPU_PRESENTMODE_IMMEDIATE)) {
        present = SDL_GPU_PRESENTMODE_IMMEDIATE;
    }
    SDL_GPUSwapchainComposition composition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    if (want_linear_swapchain_ &&
        SDL_WindowSupportsGPUSwapchainComposition(dev(device_), handle, SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR)) {
        composition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR;
    }
    swapchain_linear_ = composition == SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR;
    if (!SDL_SetGPUSwapchainParameters(dev(device_), handle, composition, present)) {
        swapchain_linear_ = false; // keep whatever SDL gave us by default
    }
    if (!from_sdl(SDL_GetGPUSwapchainTextureFormat(dev(device_), handle), swapchain_format_)) {
        SDL_SetError("rhi::Device::attach_window: the swapchain format is not one the engine knows");
        SDL_ReleaseWindowFromGPUDevice(dev(device_), handle);
        window_ = nullptr;
        return false;
    }
    return true;
}

const char* Device::backend_name() const noexcept {
    const char* name = SDL_GetGPUDeviceDriver(dev(device_));
    return name != nullptr ? name : "unknown";
}

ShaderFormat Device::shader_format() const noexcept {
    const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(dev(device_));
    if (formats & SDL_GPU_SHADERFORMAT_MSL) {
        return ShaderFormat::Msl;
    }
    if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
        return ShaderFormat::SpirV;
    }
    return ShaderFormat::Dxil;
}

TextureFormat Device::preferred_depth_format() const noexcept {
    for (const TextureFormat candidate : {TextureFormat::Depth32Float, TextureFormat::Depth24Stencil8}) {
        if (SDL_GPUTextureSupportsFormat(dev(device_), to_sdl(candidate), SDL_GPU_TEXTURETYPE_2D,
                                         SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
            return candidate;
        }
    }
    return TextureFormat::Depth16;
}

bool Device::valid(ShaderHandle handle) const noexcept {
    return pools_->shaders.contains(handle);
}
bool Device::valid(PipelineHandle handle) const noexcept {
    return pools_->pipelines.contains(handle);
}
bool Device::valid(BufferHandle handle) const noexcept {
    return pools_->buffers.contains(handle);
}
bool Device::valid(TextureHandle handle) const noexcept {
    return pools_->textures.contains(handle);
}
bool Device::valid(SamplerHandle handle) const noexcept {
    return pools_->samplers.contains(handle);
}

Device::ResourceCounts Device::resource_counts() const noexcept {
    return {pools_->shaders.size(), pools_->pipelines.size(), pools_->buffers.size(), pools_->textures.size(),
            pools_->samplers.size()};
}

ShaderHandle Device::create_shader(const ShaderDesc& desc) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    if (pools_->shaders.full()) {
        SDL_SetError("rhi: shader budget of %u exhausted", pools_->shaders.capacity());
        return {};
    }
    SDL_GPUShaderCreateInfo info{};
    info.code_size = desc.code_size;
    info.code = static_cast<const Uint8*>(desc.code);
    info.entrypoint = desc.entry_point;
    info.format = to_sdl(desc.format);
    info.stage = desc.stage == ShaderStage::Vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
    info.num_samplers = desc.num_samplers;
    info.num_uniform_buffers = desc.num_uniform_buffers;
    SDL_GPUShader* handle = SDL_CreateGPUShader(dev(device_), &info);
    if (handle == nullptr) {
        return {};
    }
    return pools_->shaders.create(Shader{handle});
}

void Device::destroy_shader(ShaderHandle shader) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    if (const Shader* object = pools_->shaders.get(shader)) {
        SDL_ReleaseGPUShader(dev(device_), object->handle);
        pools_->shaders.destroy(shader);
    }
}

PipelineHandle Device::create_graphics_pipeline(const GraphicsPipelineDesc& desc) noexcept {
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

    SDL_GPUColorTargetDescription colors[kMaxColorTargets]{};
    for (std::uint32_t i = 0; i < desc.color_target_count; ++i) {
        colors[i].format = to_sdl(desc.color_formats[i]);
    }

    SDL_GPUVertexBufferDescription vertex_buffer{};
    vertex_buffer.slot = 0;
    vertex_buffer.pitch = desc.vertex_layout.stride;
    vertex_buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attributes[16]{};
    for (std::uint32_t i = 0; i < desc.vertex_layout.attribute_count; ++i) {
        const VertexAttribute& a = desc.vertex_layout.attributes[i];
        attributes[i].location = a.location;
        attributes[i].buffer_slot = 0;
        attributes[i].format = to_sdl(a.format);
        attributes[i].offset = a.offset;
    }

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vertex->handle;
    info.fragment_shader = fragment->handle;
    info.primitive_type = to_sdl(desc.topology);
    if (desc.vertex_layout.stride > 0) {
        info.vertex_input_state.vertex_buffer_descriptions = &vertex_buffer;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = attributes;
        info.vertex_input_state.num_vertex_attributes = desc.vertex_layout.attribute_count;
    }
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = to_sdl(desc.cull);
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.depth_stencil_state.compare_op = to_sdl(desc.depth.compare);
    info.depth_stencil_state.enable_depth_test = desc.depth.test;
    info.depth_stencil_state.enable_depth_write = desc.depth.write;
    info.target_info.color_target_descriptions = colors;
    info.target_info.num_color_targets = desc.color_target_count;
    if (desc.depth_format.has_value()) {
        info.target_info.depth_stencil_format = to_sdl(*desc.depth_format);
        info.target_info.has_depth_stencil_target = true;
    }

    SDL_GPUGraphicsPipeline* handle = SDL_CreateGPUGraphicsPipeline(dev(device_), &info);
    if (handle == nullptr) {
        return {};
    }
    return pools_->pipelines.create(GraphicsPipeline{handle});
}

void Device::destroy_graphics_pipeline(PipelineHandle pipeline) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    if (const GraphicsPipeline* object = pools_->pipelines.get(pipeline)) {
        SDL_ReleaseGPUGraphicsPipeline(dev(device_), object->handle);
        pools_->pipelines.destroy(pipeline);
    }
}

BufferHandle Device::create_buffer(const BufferDesc& desc) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    if (desc.size == 0) {
        SDL_SetError("rhi::Device::create_buffer: size must be non-zero");
        return {};
    }
    if (pools_->buffers.full()) {
        SDL_SetError("rhi: buffer budget of %u exhausted", pools_->buffers.capacity());
        return {};
    }
    SDL_GPUBufferCreateInfo info{};
    info.usage = to_sdl(desc.usage);
    info.size = desc.size;
    SDL_GPUBuffer* handle = SDL_CreateGPUBuffer(dev(device_), &info);
    if (handle == nullptr) {
        return {};
    }
    return pools_->buffers.create(Buffer{handle, desc.size});
}

// Copies `size` bytes already staged in `transfer` into a buffer (at byte
// offset `level_or_offset`) or into one texture mip level (`level_or_offset`),
// submits, and waits, so the caller may free its CPU copy on return. GPU
// memory is not host-visible; this is the price.
bool Device::run_copy_and_wait(void* transfer, void* target, std::uint32_t size, std::uint32_t level_or_offset,
                               Extent2D extent, bool is_texture) noexcept {
    SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(dev(device_));
    if (command_buffer == nullptr) {
        return false;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command_buffer);
    if (is_texture) {
        SDL_GPUTextureTransferInfo source{};
        source.transfer_buffer = static_cast<SDL_GPUTransferBuffer*>(transfer);
        source.pixels_per_row = extent.width;
        source.rows_per_layer = extent.height;
        SDL_GPUTextureRegion destination{};
        destination.texture = static_cast<SDL_GPUTexture*>(target);
        destination.mip_level = level_or_offset;
        destination.w = extent.width;
        destination.h = extent.height;
        destination.d = 1;
        SDL_UploadToGPUTexture(copy, &source, &destination, false);
    } else {
        const SDL_GPUTransferBufferLocation source{static_cast<SDL_GPUTransferBuffer*>(transfer), 0};
        const SDL_GPUBufferRegion destination{static_cast<SDL_GPUBuffer*>(target), level_or_offset, size};
        SDL_UploadToGPUBuffer(copy, &source, &destination, false);
    }
    SDL_EndGPUCopyPass(copy);
    bool ok = false;
    if (SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command_buffer)) {
        ok = SDL_WaitForGPUFences(dev(device_), true, &fence, 1);
        SDL_ReleaseGPUFence(dev(device_), fence);
    }
    return ok;
}

bool Device::upload_buffer(BufferHandle buffer, const void* data, std::uint32_t size, std::uint32_t offset) noexcept {
    TY_PROFILE_SCOPE_NAMED("rhi::upload_buffer");
    TY_EXTERNAL_ALLOCATIONS();
    const Buffer* object = pools_->buffers.get(buffer);
    if (object == nullptr) {
        SDL_SetError("rhi::Device::upload_buffer: null or destroyed buffer handle");
        return false;
    }
    if (size == 0 || offset + size > object->size) {
        SDL_SetError("rhi::Device::upload_buffer: %u bytes at offset %u exceed the buffer (%u bytes)", size, offset,
                     object->size);
        return false;
    }
    SDL_GPUTransferBuffer* transfer = stage(dev(device_), data, size);
    if (transfer == nullptr) {
        return false;
    }
    const bool ok = run_copy_and_wait(transfer, object->handle, size, offset, {}, false);
    SDL_ReleaseGPUTransferBuffer(dev(device_), transfer);
    return ok;
}

BufferHandle Device::create_buffer_with_data(BufferUsage usage, const void* data, std::uint32_t size) noexcept {
    const BufferHandle buffer = create_buffer({.usage = usage, .size = size});
    if (buffer && !upload_buffer(buffer, data, size)) {
        destroy_buffer(buffer);
        return {};
    }
    return buffer;
}

void Device::destroy_buffer(BufferHandle buffer) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    if (const Buffer* object = pools_->buffers.get(buffer)) {
        SDL_ReleaseGPUBuffer(dev(device_), object->handle);
        pools_->buffers.destroy(buffer);
    }
}

TextureHandle Device::create_texture(const TextureDesc& desc) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    if (desc.width == 0 || desc.height == 0) {
        SDL_SetError("rhi::Device::create_texture: zero extent");
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
    const std::uint32_t levels = desc.mip_levels == 0 ? mip_level_count(desc.width, desc.height) : desc.mip_levels;
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = to_sdl(desc.format);
    info.usage = to_sdl(usage);
    info.width = desc.width;
    info.height = desc.height;
    info.layer_count_or_depth = 1;
    info.num_levels = levels;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* handle = SDL_CreateGPUTexture(dev(device_), &info);
    if (handle == nullptr) {
        return {};
    }
    return pools_->textures.create(
        Texture{handle, {desc.width, desc.height}, desc.format, levels, usage, false});
}

Extent2D Device::texture_extent(TextureHandle texture) const noexcept {
    const Texture* object = pools_->textures.get(texture);
    return object != nullptr ? object->extent : Extent2D{};
}

bool Device::upload_texture(TextureHandle texture, const void* pixels, std::uint32_t size,
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
    const std::uint32_t expected = level.width * level.height * bytes_per_pixel(object->format);
    if (expected == 0 || size != expected) {
        SDL_SetError("rhi::Device::upload_texture: level %u wants %u bytes, got %u", mip_level, expected, size);
        return false;
    }
    SDL_GPUTransferBuffer* transfer = stage(dev(device_), pixels, size);
    if (transfer == nullptr) {
        return false;
    }
    const bool ok = run_copy_and_wait(transfer, object->handle, size, mip_level, level, true);
    SDL_ReleaseGPUTransferBuffer(dev(device_), transfer);
    return ok;
}

bool Device::generate_mipmaps(TextureHandle texture) noexcept {
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
    SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(dev(device_));
    if (command_buffer == nullptr) {
        return false;
    }
    SDL_GenerateMipmapsForGPUTexture(command_buffer, object->handle);
    bool ok = false;
    if (SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command_buffer)) {
        ok = SDL_WaitForGPUFences(dev(device_), true, &fence, 1);
        SDL_ReleaseGPUFence(dev(device_), fence);
    }
    return ok;
}

TextureHandle Device::create_texture_with_data(TextureFormat format, std::uint32_t width, std::uint32_t height,
                                               const void* pixels, std::uint32_t size, bool mipmaps) noexcept {
    if (is_depth_format(format)) {
        SDL_SetError("rhi::Device::create_texture_with_data: depth formats cannot be uploaded");
        return {};
    }
    const bool chain = mipmaps && mip_level_count(width, height) > 1;
    const TextureHandle texture = create_texture({.format = format,
                                                  .width = width,
                                                  .height = height,
                                                  .mip_levels = chain ? 0u : 1u,
                                                  .usage = chain ? (TextureUsage::Sampled | TextureUsage::ColorTarget)
                                                                 : TextureUsage::Sampled});
    if (!texture) {
        return {};
    }
    if (!upload_texture(texture, pixels, size, 0) || (chain && !generate_mipmaps(texture))) {
        destroy_texture(texture);
        return {};
    }
    return texture;
}

void Device::destroy_texture(TextureHandle texture) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    if (const Texture* object = pools_->textures.get(texture)) {
        if (!object->borrowed) {
            SDL_ReleaseGPUTexture(dev(device_), object->handle);
        }
        pools_->textures.destroy(texture);
    }
}

SamplerHandle Device::create_sampler(const SamplerDesc& desc) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    if (pools_->samplers.full()) {
        SDL_SetError("rhi: sampler budget of %u exhausted", pools_->samplers.capacity());
        return {};
    }
    SDL_GPUSamplerCreateInfo info{};
    info.min_filter = to_sdl(desc.min_filter);
    info.mag_filter = to_sdl(desc.mag_filter);
    info.mipmap_mode =
        desc.mip_filter == Filter::Nearest ? SDL_GPU_SAMPLERMIPMAPMODE_NEAREST : SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    info.address_mode_u = to_sdl(desc.address_u);
    info.address_mode_v = to_sdl(desc.address_v);
    info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    info.max_anisotropy = desc.max_anisotropy;
    info.enable_anisotropy = desc.max_anisotropy > 1.0f;
    info.min_lod = 0.0f;
    info.max_lod = 1000.0f;
    SDL_GPUSampler* handle = SDL_CreateGPUSampler(dev(device_), &info);
    if (handle == nullptr) {
        return {};
    }
    return pools_->samplers.create(Sampler{handle});
}

void Device::destroy_sampler(SamplerHandle sampler) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    if (const Sampler* object = pools_->samplers.get(sampler)) {
        SDL_ReleaseGPUSampler(dev(device_), object->handle);
        pools_->samplers.destroy(sampler);
    }
}

std::optional<Frame> Device::begin_frame() noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    if (window_ == nullptr) {
        SDL_SetError("rhi::Device::begin_frame: no window attached");
        return std::nullopt;
    }
    SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(dev(device_));
    if (command_buffer == nullptr) {
        return std::nullopt;
    }
    SDL_GPUTexture* texture = nullptr;
    Uint32 width = 0;
    Uint32 height = 0;
    TY_PROFILE_SCOPE_NAMED("rhi::acquire swapchain (vsync wait)");
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(command_buffer, win(window_), &texture, &width, &height)) {
        SDL_CancelGPUCommandBuffer(command_buffer);
        return std::nullopt;
    }
    // The swapchain image joins the texture pool for the frame, so passes can
    // name it like any other target; submit() takes the entry back.
    TextureHandle swapchain;
    if (texture != nullptr) {
        swapchain = pools_->textures.create(Texture{texture, Extent2D{width, height}, swapchain_format_, 1,
                                                    TextureUsage::ColorTarget, true});
        if (!swapchain) {
            SDL_CancelGPUCommandBuffer(command_buffer);
            SDL_SetError("rhi: texture budget of %u exhausted, no room for the swapchain image",
                         pools_->textures.capacity());
            return std::nullopt;
        }
    }
    return Frame(this, command_buffer, swapchain, width, height);
}

} // namespace tynima::rhi
