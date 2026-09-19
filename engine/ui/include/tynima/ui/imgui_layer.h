#pragma once

#include <tynima/platform/events.h>
#include <tynima/rhi/device.h>

#include <cstdint>
#include <vector>

namespace tynima::platform {
class Input;
class Window;
} // namespace tynima::platform

// Dear ImGui, hosted by the engine. One context, fed from the platform
// layer's input snapshot and events, its draw lists streamed and drawn
// through the RHI — ImGui's own backends are not used, so ImGui never sees
// SDL or Metal and draws through the same device as the scene. Every frame:
//
//   layer.begin_frame(window, input, events, dt); // right after pump_events()
//   ... ImGui::Begin(), widgets, ImGui::End(), as usual ...
//   layer.end_frame();                            // ImGui::Render(); cursor, text input mode
//   layer.upload(frame);                          // between passes: vertices, indices, new glyphs
//   layer.draw(pass);                             // inside a pass on the format it was created for
//
// The layer works without a device too (headless, or before one exists):
// ImGui runs, nothing is uploaded or drawn.
//
// Not a member of the engine's frame rule: ImGui allocates through malloc
// (uncounted); the layer itself allocates nothing after create().
namespace tynima::ui {

class ImGuiLayer {
public:
    ImGuiLayer() = default;
    ~ImGuiLayer();
    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    // Creates the context and, with a device, the pipeline for drawing onto
    // `target_format` (`target_is_srgb`: the target encodes sRGB itself, so
    // ImGui's colours are decoded to linear on the way in). false, with
    // platform::last_error(), when the pipeline fails; the context exists
    // regardless and ImGui can be used.
    [[nodiscard]] bool create(rhi::Device* device, rhi::TextureFormat target_format, bool target_is_srgb);
    void destroy() noexcept;
    [[nodiscard]] bool ready() const noexcept { return context_ != nullptr; }
    [[nodiscard]] bool can_draw() const noexcept { return static_cast<bool>(pipeline_); }
    // Whether the frame just ended left anything to draw.
    [[nodiscard]] bool has_draw_data() const noexcept;

    // Starts ImGui's frame: display size and scale from the window, this
    // frame's mouse, keys, wheel and typed text, `dt` seconds since the last.
    void begin_frame(platform::Window& window, const platform::Input& input,
                     const std::vector<platform::Event>& events, float dt) noexcept;
    // Ends it: ImGui::Render(); sets the OS cursor to what ImGui wants and
    // turns text input on or off as a text field takes or loses focus.
    void end_frame(platform::Window& window) noexcept;

    // Streams this frame's vertices and indices on the frame's timeline and
    // creates, updates or destroys the textures ImGui asks for (its font
    // atlas). Only between passes. false when a buffer could not be made.
    [[nodiscard]] bool upload(rhi::Frame& frame) noexcept;
    // Draws this frame's lists. The pass's attachments must be the size
    // begin_frame() saw the window at, in pixels.
    void draw(rhi::RenderPass& pass) noexcept;

    // True when ImGui wants the mouse (it is over a window) or the keyboard
    // (a text field is active): the game behind should ignore that input.
    [[nodiscard]] bool wants_mouse() const noexcept;
    [[nodiscard]] bool wants_keyboard() const noexcept;

    // A texture handle as ImGui names one, for ImGui::Image(): the handle
    // packed into ImTextureID. The texture must have Sampled usage.
    [[nodiscard]] static std::uint64_t texture_id(rhi::TextureHandle texture) noexcept {
        return texture.packed();
    }
    [[nodiscard]] static rhi::TextureHandle texture_from_id(std::uint64_t id) noexcept {
        return rhi::TextureHandle::from_packed(id);
    }

    // The ImGuiContext*, for code across a library boundary (a game module)
    // to hand to ImGui::SetCurrentContext().
    [[nodiscard]] void* context() const noexcept { return context_; }

private:
    struct Uniforms {
        float projection[16];
        float params[4]; // x: decode the vertex colours from sRGB
    };

    [[nodiscard]] bool ensure_buffers(std::uint32_t vertex_bytes, std::uint32_t index_bytes) noexcept;
    void update_textures() noexcept;

    void* context_ = nullptr; // ImGuiContext*
    rhi::Device* device_ = nullptr;
    bool target_is_srgb_ = false;
    rhi::PipelineHandle pipeline_;
    rhi::SamplerHandle sampler_;
    rhi::BufferHandle vertex_buffer_, index_buffer_;
    std::uint32_t vertex_capacity_ = 0, index_capacity_ = 0; // bytes
    std::uint32_t framebuffer_width_ = 0, framebuffer_height_ = 0;
    bool frame_open_ = false;
    bool have_draw_data_ = false;
    bool text_input_ = false;
};

} // namespace tynima::ui
