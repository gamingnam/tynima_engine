// The Device's backend-independent half: what every implementation shares
// (the helpers, the composite operations) and the Frame, RenderPass and
// ComputePass wrappers, which hand each call to the backend behind them.
#include <tynima/core/assert.h>
#include <tynima/core/memory.h>

#include <SDL3/SDL_error.h>
#include <utility>

#include "backend.h"

namespace tynima::rhi {

std::uint32_t mip_level_count(std::uint32_t width, std::uint32_t height) noexcept {
    std::uint32_t levels = 1;
    for (std::uint32_t size = width > height ? width : height; size > 1; size >>= 1) {
        ++levels;
    }
    return levels;
}

const char* shader_format_name(ShaderFormat format) noexcept {
    switch (format) {
    case ShaderFormat::Msl:
        return "MSL";
    case ShaderFormat::SpirV:
        return "SPIR-V";
    case ShaderFormat::Dxil:
        return "DXIL";
    }
    return "unknown";
}

const char* texture_format_name(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::Rgba8Unorm:
        return "RGBA8_UNORM";
    case TextureFormat::Rgba8Srgb:
        return "RGBA8_SRGB";
    case TextureFormat::Bgra8Unorm:
        return "BGRA8_UNORM";
    case TextureFormat::Bgra8Srgb:
        return "BGRA8_SRGB";
    case TextureFormat::Rgba16Float:
        return "RGBA16_FLOAT";
    case TextureFormat::Rg32Float:
        return "RG32_FLOAT";
    case TextureFormat::Depth32Float:
        return "D32_FLOAT";
    case TextureFormat::Depth24Stencil8:
        return "D24_UNORM_S8";
    case TextureFormat::Depth16:
        return "D16_UNORM";
    }
    return "unknown";
}

BufferHandle Device::create_buffer_with_data(BufferUsage usage, const void* data,
                                             std::uint32_t size) noexcept {
    const BufferHandle buffer = create_buffer({.usage = usage, .size = size});
    if (buffer && !upload_buffer(buffer, data, size)) {
        destroy_buffer(buffer);
        return {};
    }
    return buffer;
}

TextureHandle Device::create_texture_with_data(TextureFormat format, std::uint32_t width,
                                               std::uint32_t height, const void* pixels, std::uint32_t size,
                                               bool mipmaps) noexcept {
    if (is_depth_format(format)) {
        SDL_SetError("rhi::Device::create_texture_with_data: depth formats cannot be uploaded");
        return {};
    }
    const bool chain = mipmaps && mip_level_count(width, height) > 1;
    const TextureHandle texture = create_texture(
        {.format = format,
         .width = width,
         .height = height,
         .mip_levels = chain ? 0u : 1u,
         .usage = chain ? (TextureUsage::Sampled | TextureUsage::ColorTarget) : TextureUsage::Sampled});
    if (!texture) {
        return {};
    }
    if (!upload_texture(texture, pixels, size, 0) || (chain && !generate_mipmaps(texture))) {
        destroy_texture(texture);
        return {};
    }
    return texture;
}

const char* backend_name(Backend backend) noexcept {
    switch (backend) {
    case Backend::Auto:
        return "auto";
    case Backend::SdlGpu:
        return "SDL GPU";
    case Backend::Metal:
        return "Metal";
    }
    return "unknown";
}

std::unique_ptr<Device> Device::create(const DeviceDesc& desc) {
    TY_EXTERNAL_ALLOCATIONS();
    switch (desc.backend) {
    case Backend::Metal:
#if defined(__APPLE__)
        return create_metal_device(desc);
#else
        SDL_SetError("rhi: the Metal backend exists on Apple platforms only");
        return nullptr;
#endif
    case Backend::Auto:
#if defined(__APPLE__)
        // The engine's own path onto Apple GPUs, where it exists; SDL GPU if
        // it somehow does not.
        if (std::unique_ptr<Device> metal = create_metal_device(desc)) {
            return metal;
        }
#endif
        return create_sdl_device(desc);
    case Backend::SdlGpu:
        return create_sdl_device(desc);
    }
    return nullptr;
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
    device_->pass_bind_pipeline(pass_, pipeline);
}

void RenderPass::bind_vertex_buffer(BufferHandle buffer, std::uint32_t offset) noexcept {
    device_->pass_bind_vertex_buffer(pass_, buffer, offset);
}

void RenderPass::bind_index_buffer(BufferHandle buffer, IndexType type, std::uint32_t offset) noexcept {
    device_->pass_bind_index_buffer(pass_, buffer, type, offset);
}

void RenderPass::bind_fragment_texture(std::uint32_t slot, TextureHandle texture,
                                       SamplerHandle sampler) noexcept {
    device_->pass_bind_fragment_texture(pass_, slot, texture, sampler);
}

void RenderPass::bind_fragment_storage_buffer(std::uint32_t slot, BufferHandle buffer) noexcept {
    device_->pass_bind_fragment_storage_buffer(pass_, slot, buffer);
}

void RenderPass::push_vertex_uniforms(std::uint32_t slot, const void* data, std::uint32_t size) noexcept {
    device_->pass_push_vertex_uniforms(command_buffer_, pass_, slot, data, size);
}

void RenderPass::push_fragment_uniforms(std::uint32_t slot, const void* data, std::uint32_t size) noexcept {
    device_->pass_push_fragment_uniforms(command_buffer_, pass_, slot, data, size);
}

void RenderPass::set_scissor(std::uint32_t x, std::uint32_t y, std::uint32_t width,
                             std::uint32_t height) noexcept {
    device_->pass_set_scissor(pass_, x, y, width, height);
}

void RenderPass::draw(std::uint32_t vertex_count, std::uint32_t instance_count) noexcept {
    device_->pass_draw(pass_, vertex_count, instance_count);
}

void RenderPass::draw_indexed(std::uint32_t index_count, std::uint32_t first_index,
                              std::int32_t vertex_offset, std::uint32_t instance_count) noexcept {
    device_->pass_draw_indexed(pass_, index_count, first_index, vertex_offset, instance_count);
}

void RenderPass::end() noexcept {
    if (pass_ != nullptr) {
        device_->pass_end(command_buffer_, pass_, labelled_);
        pass_ = nullptr;
    }
}

// --------------------------------------------------------------- ComputePass

ComputePass::ComputePass(ComputePass&& other) noexcept
    : device_(other.device_), command_buffer_(other.command_buffer_),
      pass_(std::exchange(other.pass_, nullptr)), labelled_(other.labelled_) {}

ComputePass& ComputePass::operator=(ComputePass&& other) noexcept {
    if (this != &other) {
        end();
        device_ = other.device_;
        command_buffer_ = other.command_buffer_;
        pass_ = std::exchange(other.pass_, nullptr);
        labelled_ = other.labelled_;
    }
    return *this;
}

ComputePass::~ComputePass() {
    end();
}

void ComputePass::bind_pipeline(ComputePipelineHandle pipeline) noexcept {
    device_->compute_bind_pipeline(pass_, pipeline);
}

void ComputePass::bind_storage_buffer(std::uint32_t slot, BufferHandle buffer) noexcept {
    device_->compute_bind_storage_buffer(pass_, slot, buffer);
}

void ComputePass::push_uniforms(std::uint32_t slot, const void* data, std::uint32_t size) noexcept {
    device_->compute_push_uniforms(command_buffer_, pass_, slot, data, size);
}

void ComputePass::dispatch(std::uint32_t groups_x, std::uint32_t groups_y, std::uint32_t groups_z) noexcept {
    device_->compute_dispatch(pass_, groups_x, groups_y, groups_z);
}

void ComputePass::end() noexcept {
    if (pass_ != nullptr) {
        device_->compute_end(command_buffer_, pass_, labelled_);
        pass_ = nullptr;
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
    if (command_buffer_ == nullptr) {
        SDL_SetError("rhi::Frame::begin_pass: the frame was submitted");
        return std::nullopt;
    }
    return device_->frame_begin_pass(command_buffer_, desc);
}

std::optional<ComputePass> Frame::begin_compute_pass(const ComputePassDesc& desc) noexcept {
    if (command_buffer_ == nullptr) {
        SDL_SetError("rhi::Frame::begin_compute_pass: the frame was submitted");
        return std::nullopt;
    }
    return device_->frame_begin_compute_pass(command_buffer_, desc);
}

bool Frame::write_buffer(BufferHandle buffer, const void* data, std::uint32_t size,
                         std::uint32_t offset) noexcept {
    if (command_buffer_ == nullptr) {
        SDL_SetError("rhi::Frame::write_buffer: the frame was submitted");
        return false;
    }
    return device_->frame_write_buffer(command_buffer_, buffer, data, size, offset);
}

std::optional<RenderPass> Frame::begin_swapchain_pass(const ClearColor& clear, TextureHandle depth,
                                                      float depth_clear) noexcept {
    if (!swapchain_) {
        SDL_SetError("rhi::Frame::begin_swapchain_pass: no swapchain image this frame");
        return std::nullopt;
    }
    RenderPassDesc desc;
    desc.colors[0] = ColorAttachment{
        .texture = swapchain_, .load = LoadOp::Clear, .store = StoreOp::Store, .clear = clear};
    desc.color_count = 1;
    if (depth) {
        desc.depth = DepthAttachment{
            .texture = depth, .load = LoadOp::Clear, .store = StoreOp::DontCare, .clear = depth_clear};
    }
    return begin_pass(desc);
}

void Frame::submit() noexcept {
    if (command_buffer_ != nullptr) {
        device_->frame_submit(command_buffer_, swapchain_);
        command_buffer_ = nullptr;
        swapchain_ = {};
    }
}

} // namespace tynima::rhi
