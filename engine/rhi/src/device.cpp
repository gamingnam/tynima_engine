#include <tynima/rhi/device.h>

#include <tynima/core/profile.h>
#include <tynima/platform/window.h>

#include <SDL3/SDL.h>

#include <cstring>
#include <utility>

namespace tynima::rhi {

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
};

struct Sampler {
    SDL_GPUSampler* handle;
};

namespace {

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
    case TextureFormat::Depth32Float: return SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    case TextureFormat::Depth24Stencil8: return SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    case TextureFormat::Depth16: return SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    }
    return SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
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

} // namespace

const char* shader_format_name(ShaderFormat format) noexcept {
    switch (format) {
    case ShaderFormat::Msl: return "MSL";
    case ShaderFormat::SpirV: return "SPIR-V";
    case ShaderFormat::Dxil: return "DXIL";
    }
    return "unknown";
}

std::uint32_t mip_level_count(std::uint32_t width, std::uint32_t height) noexcept {
    std::uint32_t levels = 1;
    for (std::uint32_t size = width > height ? width : height; size > 1; size >>= 1) {
        ++levels;
    }
    return levels;
}

const char* texture_format_name(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::Rgba8Unorm: return "RGBA8_UNORM";
    case TextureFormat::Rgba8Srgb: return "RGBA8_SRGB";
    case TextureFormat::Depth32Float: return "D32_FLOAT";
    case TextureFormat::Depth24Stencil8: return "D24_UNORM_S8";
    case TextureFormat::Depth16: return "D16_UNORM";
    }
    return "unknown";
}

// ---------------------------------------------------------------- RenderPass

RenderPass::RenderPass(RenderPass&& other) noexcept
    : command_buffer_(other.command_buffer_), pass_(std::exchange(other.pass_, nullptr)) {}

RenderPass& RenderPass::operator=(RenderPass&& other) noexcept {
    if (this != &other) {
        end();
        command_buffer_ = other.command_buffer_;
        pass_ = std::exchange(other.pass_, nullptr);
    }
    return *this;
}

RenderPass::~RenderPass() {
    end();
}

void RenderPass::bind_pipeline(GraphicsPipeline& pipeline) noexcept {
    SDL_BindGPUGraphicsPipeline(rp(pass_), pipeline.handle);
}

void RenderPass::bind_vertex_buffer(Buffer& buffer, std::uint32_t offset) noexcept {
    const SDL_GPUBufferBinding binding{buffer.handle, offset};
    SDL_BindGPUVertexBuffers(rp(pass_), 0, &binding, 1);
}

void RenderPass::bind_index_buffer(Buffer& buffer, IndexType type, std::uint32_t offset) noexcept {
    const SDL_GPUBufferBinding binding{buffer.handle, offset};
    SDL_BindGPUIndexBuffer(rp(pass_), &binding,
                           type == IndexType::Uint16 ? SDL_GPU_INDEXELEMENTSIZE_16BIT : SDL_GPU_INDEXELEMENTSIZE_32BIT);
}

void RenderPass::bind_fragment_texture(std::uint32_t slot, Texture& texture, Sampler& sampler) noexcept {
    const SDL_GPUTextureSamplerBinding binding{texture.handle, sampler.handle};
    SDL_BindGPUFragmentSamplers(rp(pass_), slot, &binding, 1);
}

void RenderPass::push_vertex_uniforms(std::uint32_t slot, const void* data, std::uint32_t size) noexcept {
    SDL_PushGPUVertexUniformData(cmd(command_buffer_), slot, data, size);
}

void RenderPass::push_fragment_uniforms(std::uint32_t slot, const void* data, std::uint32_t size) noexcept {
    SDL_PushGPUFragmentUniformData(cmd(command_buffer_), slot, data, size);
}

void RenderPass::draw(std::uint32_t vertex_count, std::uint32_t instance_count) noexcept {
    SDL_DrawGPUPrimitives(rp(pass_), vertex_count, instance_count, 0, 0);
}

void RenderPass::draw_indexed(std::uint32_t index_count, std::uint32_t first_index, std::int32_t vertex_offset,
                              std::uint32_t instance_count) noexcept {
    SDL_DrawGPUIndexedPrimitives(rp(pass_), index_count, instance_count, first_index, vertex_offset, 0);
}

void RenderPass::end() noexcept {
    if (pass_ != nullptr) {
        SDL_EndGPURenderPass(rp(pass_));
        pass_ = nullptr;
    }
}

// --------------------------------------------------------------------- Frame

Frame::Frame(void* command_buffer, void* swapchain_texture, std::uint32_t width, std::uint32_t height) noexcept
    : command_buffer_(command_buffer), swapchain_texture_(swapchain_texture), width_(width), height_(height) {}

Frame::Frame(Frame&& other) noexcept
    : command_buffer_(std::exchange(other.command_buffer_, nullptr)),
      swapchain_texture_(std::exchange(other.swapchain_texture_, nullptr)), width_(other.width_),
      height_(other.height_) {}

Frame& Frame::operator=(Frame&& other) noexcept {
    if (this != &other) {
        submit();
        command_buffer_ = std::exchange(other.command_buffer_, nullptr);
        swapchain_texture_ = std::exchange(other.swapchain_texture_, nullptr);
        width_ = other.width_;
        height_ = other.height_;
    }
    return *this;
}

Frame::~Frame() {
    submit();
}

std::optional<RenderPass> Frame::begin_swapchain_pass(const ClearColor& clear, Texture* depth,
                                                      float depth_clear) noexcept {
    if (command_buffer_ == nullptr || swapchain_texture_ == nullptr) {
        return std::nullopt;
    }
    SDL_GPUColorTargetInfo color{};
    color.texture = static_cast<SDL_GPUTexture*>(swapchain_texture_);
    color.clear_color = SDL_FColor{clear.r, clear.g, clear.b, clear.a};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPUDepthStencilTargetInfo depth_info{};
    if (depth != nullptr) {
        depth_info.texture = depth->handle;
        depth_info.clear_depth = depth_clear;
        depth_info.load_op = SDL_GPU_LOADOP_CLEAR;
        depth_info.store_op = SDL_GPU_STOREOP_DONT_CARE; // never read back: stays on-chip on a tiler
        depth_info.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        depth_info.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        depth_info.cycle = true; // let SDL swap in a fresh texture if last frame's is still in flight
    }

    SDL_GPURenderPass* pass =
        SDL_BeginGPURenderPass(cmd(command_buffer_), &color, 1, depth != nullptr ? &depth_info : nullptr);
    if (pass == nullptr) {
        return std::nullopt;
    }
    return RenderPass(command_buffer_, pass);
}

void Frame::submit() noexcept {
    if (command_buffer_ != nullptr) {
        TY_PROFILE_SCOPE_NAMED("rhi::Frame::submit");
        SDL_SubmitGPUCommandBuffer(cmd(command_buffer_));
        command_buffer_ = nullptr;
        swapchain_texture_ = nullptr;
    }
}

// -------------------------------------------------------------------- Device

std::unique_ptr<Device> Device::create(const DeviceDesc& desc) {
    // The formats we can hand the backend today. SDL picks a backend that
    // consumes at least one of them.
    SDL_GPUDevice* device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, desc.debug, nullptr);
    if (device == nullptr) {
        return nullptr;
    }
    return std::unique_ptr<Device>(new Device(device, desc.vsync, desc.linear_swapchain));
}

Device::~Device() {
    SDL_WaitForGPUIdle(dev(device_));
    if (window_ != nullptr) {
        SDL_ReleaseWindowFromGPUDevice(dev(device_), win(window_));
    }
    SDL_DestroyGPUDevice(dev(device_));
}

bool Device::attach_window(platform::Window& window) noexcept {
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

Shader* Device::create_shader(const ShaderDesc& desc) noexcept {
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
        return nullptr;
    }
    return new Shader{handle};
}

void Device::destroy_shader(Shader* shader) noexcept {
    if (shader != nullptr) {
        SDL_ReleaseGPUShader(dev(device_), shader->handle);
        delete shader;
    }
}

GraphicsPipeline* Device::create_graphics_pipeline(const GraphicsPipelineDesc& desc) noexcept {
    if (window_ == nullptr) {
        SDL_SetError("rhi::Device::create_graphics_pipeline: attach_window() first — the pipeline targets the "
                     "swapchain format");
        return nullptr;
    }
    if (desc.vertex_shader == nullptr || desc.fragment_shader == nullptr) {
        SDL_SetError("rhi::Device::create_graphics_pipeline: both shaders are required");
        return nullptr;
    }
    if (desc.vertex_layout.attribute_count > 16) {
        SDL_SetError("rhi::Device::create_graphics_pipeline: too many vertex attributes");
        return nullptr;
    }

    SDL_GPUColorTargetDescription color{};
    color.format = SDL_GetGPUSwapchainTextureFormat(dev(device_), win(window_));

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
    info.vertex_shader = desc.vertex_shader->handle;
    info.fragment_shader = desc.fragment_shader->handle;
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
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1;
    if (desc.has_depth_target) {
        info.target_info.depth_stencil_format = to_sdl(preferred_depth_format());
        info.target_info.has_depth_stencil_target = true;
    }

    SDL_GPUGraphicsPipeline* handle = SDL_CreateGPUGraphicsPipeline(dev(device_), &info);
    if (handle == nullptr) {
        return nullptr;
    }
    return new GraphicsPipeline{handle};
}

void Device::destroy_graphics_pipeline(GraphicsPipeline* pipeline) noexcept {
    if (pipeline != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(dev(device_), pipeline->handle);
        delete pipeline;
    }
}

Buffer* Device::create_buffer(const BufferDesc& desc) noexcept {
    if (desc.size == 0) {
        SDL_SetError("rhi::Device::create_buffer: size must be non-zero");
        return nullptr;
    }
    SDL_GPUBufferCreateInfo info{};
    info.usage = to_sdl(desc.usage);
    info.size = desc.size;
    SDL_GPUBuffer* handle = SDL_CreateGPUBuffer(dev(device_), &info);
    if (handle == nullptr) {
        return nullptr;
    }
    return new Buffer{handle, desc.size};
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

namespace {

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

bool Device::upload_buffer(Buffer& buffer, const void* data, std::uint32_t size, std::uint32_t offset) noexcept {
    TY_PROFILE_SCOPE_NAMED("rhi::upload_buffer");
    if (size == 0 || offset + size > buffer.size) {
        SDL_SetError("rhi::Device::upload_buffer: %u bytes at offset %u exceed the buffer (%u bytes)", size, offset,
                     buffer.size);
        return false;
    }
    SDL_GPUTransferBuffer* transfer = stage(dev(device_), data, size);
    if (transfer == nullptr) {
        return false;
    }
    const bool ok = run_copy_and_wait(transfer, buffer.handle, size, offset, {}, false);
    SDL_ReleaseGPUTransferBuffer(dev(device_), transfer);
    return ok;
}

Buffer* Device::create_buffer_with_data(BufferUsage usage, const void* data, std::uint32_t size) noexcept {
    Buffer* buffer = create_buffer({.usage = usage, .size = size});
    if (buffer != nullptr && !upload_buffer(*buffer, data, size)) {
        destroy_buffer(buffer);
        return nullptr;
    }
    return buffer;
}

void Device::destroy_buffer(Buffer* buffer) noexcept {
    if (buffer != nullptr) {
        SDL_ReleaseGPUBuffer(dev(device_), buffer->handle);
        delete buffer;
    }
}

Texture* Device::create_texture(const TextureDesc& desc) noexcept {
    if (desc.width == 0 || desc.height == 0) {
        SDL_SetError("rhi::Device::create_texture: zero extent");
        return nullptr;
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
        return nullptr;
    }
    return new Texture{handle, {desc.width, desc.height}, desc.format, levels, usage};
}

Extent2D Device::texture_extent(const Texture& texture) const noexcept {
    return texture.extent;
}

bool Device::upload_texture(Texture& texture, const void* pixels, std::uint32_t size, std::uint32_t mip_level) noexcept {
    TY_PROFILE_SCOPE_NAMED("rhi::upload_texture");
    if (mip_level >= texture.mip_levels) {
        SDL_SetError("rhi::Device::upload_texture: mip level %u of %u", mip_level, texture.mip_levels);
        return false;
    }
    const Extent2D level{texture.extent.width >> mip_level > 0 ? texture.extent.width >> mip_level : 1,
                         texture.extent.height >> mip_level > 0 ? texture.extent.height >> mip_level : 1};
    const std::uint32_t expected = level.width * level.height * bytes_per_pixel(texture.format);
    if (expected == 0 || size != expected) {
        SDL_SetError("rhi::Device::upload_texture: level %u wants %u bytes, got %u", mip_level, expected, size);
        return false;
    }
    SDL_GPUTransferBuffer* transfer = stage(dev(device_), pixels, size);
    if (transfer == nullptr) {
        return false;
    }
    const bool ok = run_copy_and_wait(transfer, texture.handle, size, mip_level, level, true);
    SDL_ReleaseGPUTransferBuffer(dev(device_), transfer);
    return ok;
}

bool Device::generate_mipmaps(Texture& texture) noexcept {
    TY_PROFILE_SCOPE_NAMED("rhi::generate_mipmaps");
    if (texture.mip_levels <= 1) {
        SDL_SetError("rhi::Device::generate_mipmaps: the texture has a single level");
        return false;
    }
    if (!has_usage(texture.usage, TextureUsage::ColorTarget)) {
        SDL_SetError("rhi::Device::generate_mipmaps: the texture needs ColorTarget usage");
        return false;
    }
    SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(dev(device_));
    if (command_buffer == nullptr) {
        return false;
    }
    SDL_GenerateMipmapsForGPUTexture(command_buffer, texture.handle);
    bool ok = false;
    if (SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command_buffer)) {
        ok = SDL_WaitForGPUFences(dev(device_), true, &fence, 1);
        SDL_ReleaseGPUFence(dev(device_), fence);
    }
    return ok;
}

Texture* Device::create_texture_with_data(TextureFormat format, std::uint32_t width, std::uint32_t height,
                                          const void* pixels, std::uint32_t size, bool mipmaps) noexcept {
    if (is_depth_format(format)) {
        SDL_SetError("rhi::Device::create_texture_with_data: depth formats cannot be uploaded");
        return nullptr;
    }
    const bool chain = mipmaps && mip_level_count(width, height) > 1;
    Texture* texture = create_texture({.format = format,
                                       .width = width,
                                       .height = height,
                                       .mip_levels = chain ? 0u : 1u,
                                       .usage = chain ? (TextureUsage::Sampled | TextureUsage::ColorTarget)
                                                      : TextureUsage::Sampled});
    if (texture == nullptr) {
        return nullptr;
    }
    if (!upload_texture(*texture, pixels, size, 0) || (chain && !generate_mipmaps(*texture))) {
        destroy_texture(texture);
        return nullptr;
    }
    return texture;
}

void Device::destroy_texture(Texture* texture) noexcept {
    if (texture != nullptr) {
        SDL_ReleaseGPUTexture(dev(device_), texture->handle);
        delete texture;
    }
}

Sampler* Device::create_sampler(const SamplerDesc& desc) noexcept {
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
        return nullptr;
    }
    return new Sampler{handle};
}

void Device::destroy_sampler(Sampler* sampler) noexcept {
    if (sampler != nullptr) {
        SDL_ReleaseGPUSampler(dev(device_), sampler->handle);
        delete sampler;
    }
}

std::optional<Frame> Device::begin_frame() noexcept {
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
    return Frame(command_buffer, texture, width, height);
}

} // namespace tynima::rhi
