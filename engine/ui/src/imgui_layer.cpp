#include <tynima/core/assert.h>
#include <tynima/core/log.h>
#include <tynima/platform/input.h>
#include <tynima/platform/platform.h>
#include <tynima/platform/window.h>
#include <tynima/ui/imgui_layer.h>

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace tynima::ui {

namespace {

// ImGui's vertex: two floats of position in points, two of texture
// coordinates, four bytes of colour. Twenty bytes, as ImDrawVert says.
static_assert(sizeof(ImDrawVert) == 20, "ImDrawVert is the layout below");
static_assert(sizeof(ImDrawIdx) == 2, "ImDrawIdx is 16-bit, which IndexType::Uint16 draws");
constexpr rhi::VertexAttribute kAttributes[3] = {
    {.location = 0, .format = rhi::VertexFormat::Float2, .offset = offsetof(ImDrawVert, pos)},
    {.location = 1, .format = rhi::VertexFormat::Float2, .offset = offsetof(ImDrawVert, uv)},
    {.location = 2, .format = rhi::VertexFormat::Ubyte4Norm, .offset = offsetof(ImDrawVert, col)},
};

// Points to clip space through the projection in the uniforms; colour times
// texture, blended straight over the target. When the target is
// sRGB-encoded by the hardware, ImGui's colours (which are sRGB) are
// decoded first so they encode back to what ImGui meant.
constexpr const char* kMsl = R"(
#include <metal_stdlib>
using namespace metal;
struct VertexIn {
    float2 position [[attribute(0)]];
    float2 uv [[attribute(1)]];
    float4 color [[attribute(2)]];
};
struct Uniforms {
    float4x4 projection;
    float4 params;
};
struct VSOut {
    float4 position [[position]];
    float2 uv;
    float4 color;
};
vertex VSOut vs_main(VertexIn in [[stage_in]], constant Uniforms& u [[buffer(0)]]) {
    VSOut out;
    out.position = u.projection * float4(in.position, 0.0, 1.0);
    out.uv = in.uv;
    out.color = in.color;
    return out;
}
float3 srgb_to_linear(float3 c) {
    return select(pow((c + 0.055) / 1.055, float3(2.4)), c / 12.92, c <= 0.04045);
}
fragment float4 fs_main(VSOut in [[stage_in]], constant Uniforms& u [[buffer(0)]],
                        texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) {
    float4 color = in.color;
    if (u.params.x > 0.5) {
        color.rgb = srgb_to_linear(color.rgb);
    }
    return color * tex.sample(s, in.uv);
}
)";

ImGuiKey to_imgui(platform::Key key) noexcept {
    using platform::Key;
    switch (key) {
    case Key::A:
        return ImGuiKey_A;
    case Key::B:
        return ImGuiKey_B;
    case Key::C:
        return ImGuiKey_C;
    case Key::D:
        return ImGuiKey_D;
    case Key::E:
        return ImGuiKey_E;
    case Key::F:
        return ImGuiKey_F;
    case Key::G:
        return ImGuiKey_G;
    case Key::H:
        return ImGuiKey_H;
    case Key::I:
        return ImGuiKey_I;
    case Key::J:
        return ImGuiKey_J;
    case Key::K:
        return ImGuiKey_K;
    case Key::L:
        return ImGuiKey_L;
    case Key::M:
        return ImGuiKey_M;
    case Key::N:
        return ImGuiKey_N;
    case Key::O:
        return ImGuiKey_O;
    case Key::P:
        return ImGuiKey_P;
    case Key::Q:
        return ImGuiKey_Q;
    case Key::R:
        return ImGuiKey_R;
    case Key::S:
        return ImGuiKey_S;
    case Key::T:
        return ImGuiKey_T;
    case Key::U:
        return ImGuiKey_U;
    case Key::V:
        return ImGuiKey_V;
    case Key::W:
        return ImGuiKey_W;
    case Key::X:
        return ImGuiKey_X;
    case Key::Y:
        return ImGuiKey_Y;
    case Key::Z:
        return ImGuiKey_Z;
    case Key::Digit0:
        return ImGuiKey_0;
    case Key::Digit1:
        return ImGuiKey_1;
    case Key::Digit2:
        return ImGuiKey_2;
    case Key::Digit3:
        return ImGuiKey_3;
    case Key::Digit4:
        return ImGuiKey_4;
    case Key::Digit5:
        return ImGuiKey_5;
    case Key::Digit6:
        return ImGuiKey_6;
    case Key::Digit7:
        return ImGuiKey_7;
    case Key::Digit8:
        return ImGuiKey_8;
    case Key::Digit9:
        return ImGuiKey_9;
    case Key::F1:
        return ImGuiKey_F1;
    case Key::F2:
        return ImGuiKey_F2;
    case Key::F3:
        return ImGuiKey_F3;
    case Key::F4:
        return ImGuiKey_F4;
    case Key::F5:
        return ImGuiKey_F5;
    case Key::F6:
        return ImGuiKey_F6;
    case Key::F7:
        return ImGuiKey_F7;
    case Key::F8:
        return ImGuiKey_F8;
    case Key::F9:
        return ImGuiKey_F9;
    case Key::F10:
        return ImGuiKey_F10;
    case Key::F11:
        return ImGuiKey_F11;
    case Key::F12:
        return ImGuiKey_F12;
    case Key::Escape:
        return ImGuiKey_Escape;
    case Key::Enter:
        return ImGuiKey_Enter;
    case Key::Tab:
        return ImGuiKey_Tab;
    case Key::Backspace:
        return ImGuiKey_Backspace;
    case Key::Space:
        return ImGuiKey_Space;
    case Key::Minus:
        return ImGuiKey_Minus;
    case Key::Equals:
        return ImGuiKey_Equal;
    case Key::LeftBracket:
        return ImGuiKey_LeftBracket;
    case Key::RightBracket:
        return ImGuiKey_RightBracket;
    case Key::Backslash:
        return ImGuiKey_Backslash;
    case Key::Semicolon:
        return ImGuiKey_Semicolon;
    case Key::Apostrophe:
        return ImGuiKey_Apostrophe;
    case Key::Grave:
        return ImGuiKey_GraveAccent;
    case Key::Comma:
        return ImGuiKey_Comma;
    case Key::Period:
        return ImGuiKey_Period;
    case Key::Slash:
        return ImGuiKey_Slash;
    case Key::CapsLock:
        return ImGuiKey_CapsLock;
    case Key::Insert:
        return ImGuiKey_Insert;
    case Key::Delete:
        return ImGuiKey_Delete;
    case Key::Home:
        return ImGuiKey_Home;
    case Key::End:
        return ImGuiKey_End;
    case Key::PageUp:
        return ImGuiKey_PageUp;
    case Key::PageDown:
        return ImGuiKey_PageDown;
    case Key::Left:
        return ImGuiKey_LeftArrow;
    case Key::Right:
        return ImGuiKey_RightArrow;
    case Key::Up:
        return ImGuiKey_UpArrow;
    case Key::Down:
        return ImGuiKey_DownArrow;
    case Key::LeftShift:
        return ImGuiKey_LeftShift;
    case Key::RightShift:
        return ImGuiKey_RightShift;
    case Key::LeftCtrl:
        return ImGuiKey_LeftCtrl;
    case Key::RightCtrl:
        return ImGuiKey_RightCtrl;
    case Key::LeftAlt:
        return ImGuiKey_LeftAlt;
    case Key::RightAlt:
        return ImGuiKey_RightAlt;
    case Key::LeftSuper:
        return ImGuiKey_LeftSuper;
    case Key::RightSuper:
        return ImGuiKey_RightSuper;
    case Key::Unknown:
    case Key::Count:
        break;
    }
    return ImGuiKey_None;
}

platform::Cursor to_platform(ImGuiMouseCursor cursor) noexcept {
    switch (cursor) {
    case ImGuiMouseCursor_None:
        return platform::Cursor::Hidden;
    case ImGuiMouseCursor_TextInput:
        return platform::Cursor::Text;
    case ImGuiMouseCursor_ResizeAll:
        return platform::Cursor::ResizeAll;
    case ImGuiMouseCursor_ResizeNS:
        return platform::Cursor::ResizeNS;
    case ImGuiMouseCursor_ResizeEW:
        return platform::Cursor::ResizeEW;
    case ImGuiMouseCursor_ResizeNESW:
        return platform::Cursor::ResizeNESW;
    case ImGuiMouseCursor_ResizeNWSE:
        return platform::Cursor::ResizeNWSE;
    case ImGuiMouseCursor_Hand:
        return platform::Cursor::Hand;
    case ImGuiMouseCursor_Wait:
        return platform::Cursor::Wait;
    case ImGuiMouseCursor_Progress:
        return platform::Cursor::Progress;
    case ImGuiMouseCursor_NotAllowed:
        return platform::Cursor::NotAllowed;
    default:
        break;
    }
    return platform::Cursor::Arrow;
}

// The mouse buttons ImGui numbers 0..4: left, right, middle, then the extras.
constexpr platform::MouseButton kMouseButtons[5] = {platform::MouseButton::Left, platform::MouseButton::Right,
                                                    platform::MouseButton::Middle, platform::MouseButton::X1,
                                                    platform::MouseButton::X2};

const char* get_clipboard(ImGuiContext*) {
    return platform::clipboard_text();
}
void set_clipboard(ImGuiContext*, const char* text) {
    platform::set_clipboard_text(text);
}
// The callback a draw list uses to ask for the render state back: only its
// identity matters, draw() recognises it and rebinds.
void reset_render_state(const ImDrawList*, const ImDrawCmd*) {}

} // namespace

ImGuiLayer::~ImGuiLayer() {
    destroy();
}

bool ImGuiLayer::create(rhi::Device* device, rhi::TextureFormat target_format, bool target_is_srgb) {
    destroy();
    IMGUI_CHECKVERSION();
    ImGuiContext* context = ImGui::CreateContext();
    context_ = context;
    ImGui::SetCurrentContext(context);
    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = "tynima::platform";
    io.BackendRendererName = "tynima::rhi";
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset; // draw_indexed() takes a vertex offset
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;  // upload() serves the atlas's requests
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // no imgui.ini in the working directory unless the host names one
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    platform_io.Platform_GetClipboardTextFn = get_clipboard;
    platform_io.Platform_SetClipboardTextFn = set_clipboard;
    platform_io.DrawCallback_ResetRenderState = reset_render_state;
    ImGui::StyleColorsDark();

    device_ = device;
    target_is_srgb_ = target_is_srgb;
    if (device == nullptr) {
        return true;
    }
    if (device->shader_format() != rhi::ShaderFormat::Msl) {
        TY_LOG_WARN("ui", "the UI carries MSL only until SDL_shadercross lands; drawing nothing");
        return true;
    }
    const rhi::ShaderDesc common{
        .format = rhi::ShaderFormat::Msl, .code = kMsl, .code_size = std::strlen(kMsl)};
    rhi::ShaderDesc vs_desc = common;
    vs_desc.stage = rhi::ShaderStage::Vertex;
    vs_desc.entry_point = "vs_main";
    vs_desc.num_uniform_buffers = 1;
    rhi::ShaderDesc fs_desc = common;
    fs_desc.stage = rhi::ShaderStage::Fragment;
    fs_desc.entry_point = "fs_main";
    fs_desc.num_uniform_buffers = 1;
    fs_desc.num_samplers = 1;
    const rhi::ShaderHandle vs = device->create_shader(vs_desc);
    const rhi::ShaderHandle fs = device->create_shader(fs_desc);
    if (vs && fs) {
        rhi::GraphicsPipelineDesc desc;
        desc.vertex_shader = vs;
        desc.fragment_shader = fs;
        desc.vertex_layout = {.stride = sizeof(ImDrawVert), .attributes = kAttributes, .attribute_count = 3};
        desc.color_formats[0] = target_format;
        desc.color_target_count = 1;
        desc.blend[0] = rhi::BlendMode::Alpha;
        pipeline_ = device->create_graphics_pipeline(desc);
    }
    device->destroy_shader(vs);
    device->destroy_shader(fs);
    sampler_ = device->create_sampler(
        {.address_u = rhi::AddressMode::ClampToEdge, .address_v = rhi::AddressMode::ClampToEdge});
    if (!pipeline_ || !sampler_) {
        TY_LOG_ERROR("ui", "pipeline failed: %s", platform::last_error());
        device->destroy_graphics_pipeline(pipeline_);
        device->destroy_sampler(sampler_);
        pipeline_ = {};
        sampler_ = {};
        return false;
    }
    return true;
}

void ImGuiLayer::destroy() noexcept {
    if (context_ == nullptr) {
        return;
    }
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context_));
    if (frame_open_) {
        ImGui::EndFrame();
        frame_open_ = false;
    }
    if (device_ != nullptr) {
        // The textures ImGui still holds: ours to free, whatever their status.
        for (ImTextureData* texture : ImGui::GetPlatformIO().Textures) {
            if (texture->GetTexID() != ImTextureID_Invalid) {
                device_->destroy_texture(texture_from_id(texture->GetTexID()));
                texture->SetTexID(ImTextureID_Invalid);
            }
            texture->SetStatus(ImTextureStatus_Destroyed);
        }
        device_->destroy_buffer(vertex_buffer_);
        device_->destroy_buffer(index_buffer_);
        device_->destroy_graphics_pipeline(pipeline_);
        device_->destroy_sampler(sampler_);
    }
    vertex_buffer_ = index_buffer_ = {};
    vertex_capacity_ = index_capacity_ = 0;
    pipeline_ = {};
    sampler_ = {};
    ImGui::DestroyContext(static_cast<ImGuiContext*>(context_));
    context_ = nullptr;
    device_ = nullptr;
    have_draw_data_ = false;
}

void ImGuiLayer::begin_frame(platform::Window& window, const platform::Input& input,
                             const std::vector<platform::Event>& events, float dt) noexcept {
    TY_ASSERT(context_ != nullptr, "ImGuiLayer::begin_frame before create()");
    if (context_ == nullptr) {
        return;
    }
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context_));
    ImGuiIO& io = ImGui::GetIO();
    if (frame_open_) {
        ImGui::EndFrame(); // a frame nobody ended: never let two open
    }

    const float width = static_cast<float>(std::max(window.width(), 1));
    const float height = static_cast<float>(std::max(window.height(), 1));
    framebuffer_width_ = static_cast<std::uint32_t>(std::max(window.pixel_width(), 1));
    framebuffer_height_ = static_cast<std::uint32_t>(std::max(window.pixel_height(), 1));
    io.DisplaySize = ImVec2(width, height);
    io.DisplayFramebufferScale = ImVec2(static_cast<float>(framebuffer_width_) / width,
                                        static_cast<float>(framebuffer_height_) / height);
    io.DeltaTime = std::max(dt, 1.0f / 100000.0f); // ImGui insists on a positive step

    // Focus and typed text come as events; everything else is the frame's
    // snapshot, whose edges become ImGui's events in order.
    for (const platform::Event& event : events) {
        switch (event.type) {
        case platform::EventType::WindowFocusGained:
            io.AddFocusEvent(true);
            break;
        case platform::EventType::WindowFocusLost:
            io.AddFocusEvent(false);
            break;
        case platform::EventType::TextInput:
            io.AddInputCharactersUTF8(event.text);
            break;
        default:
            break;
        }
    }
    using platform::Key;
    const auto down = [&](Key a, Key b) { return input.key_down(a) || input.key_down(b); };
    io.AddKeyEvent(ImGuiMod_Ctrl, down(Key::LeftCtrl, Key::RightCtrl));
    io.AddKeyEvent(ImGuiMod_Shift, down(Key::LeftShift, Key::RightShift));
    io.AddKeyEvent(ImGuiMod_Alt, down(Key::LeftAlt, Key::RightAlt));
    io.AddKeyEvent(ImGuiMod_Super, down(Key::LeftSuper, Key::RightSuper));
    for (std::size_t i = 0; i < platform::kKeyCount; ++i) {
        const auto key = static_cast<Key>(i);
        const ImGuiKey mapped = to_imgui(key);
        if (mapped == ImGuiKey_None) {
            continue;
        }
        const bool pressed = input.key_pressed(key), released = input.key_released(key);
        if (pressed && released) {
            // Both edges in one frame: the level says which came last.
            const bool level = input.key_down(key);
            io.AddKeyEvent(mapped, !level);
            io.AddKeyEvent(mapped, level);
        } else if (pressed) {
            io.AddKeyEvent(mapped, true);
        } else if (released) {
            io.AddKeyEvent(mapped, false);
        }
    }
    io.AddMousePosEvent(input.mouse_x(), input.mouse_y());
    for (int b = 0; b < 5; ++b) {
        const platform::MouseButton button = kMouseButtons[b];
        const bool pressed = input.mouse_pressed(button), released = input.mouse_released(button);
        if (pressed && released) {
            const bool level = input.mouse_down(button);
            io.AddMouseButtonEvent(b, !level);
            io.AddMouseButtonEvent(b, level);
        } else if (pressed) {
            io.AddMouseButtonEvent(b, true);
        } else if (released) {
            io.AddMouseButtonEvent(b, false);
        }
    }
    if (input.wheel_x() != 0.0f || input.wheel_y() != 0.0f) {
        io.AddMouseWheelEvent(input.wheel_x(), input.wheel_y());
    }

    ImGui::NewFrame();
    frame_open_ = true;
}

void ImGuiLayer::end_frame(platform::Window& window) noexcept {
    if (context_ == nullptr || !frame_open_) {
        return;
    }
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context_));
    ImGui::Render();
    frame_open_ = false;
    have_draw_data_ = true;
    ImGuiIO& io = ImGui::GetIO();
    if ((io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) == 0) {
        platform::set_cursor(io.MouseDrawCursor ? platform::Cursor::Hidden
                                                : to_platform(ImGui::GetMouseCursor()));
    }
    if (io.WantTextInput != text_input_) {
        text_input_ = io.WantTextInput;
        window.set_text_input(text_input_);
    }
}

bool ImGuiLayer::ensure_buffers(std::uint32_t vertex_bytes, std::uint32_t index_bytes) noexcept {
    // Grown to the next power of two and never shrunk: a UI's size settles
    // after a few frames. The old buffer may still be read by the frame in
    // flight; the device keeps it alive until that frame completes.
    const auto grow = [&](rhi::BufferHandle& buffer, std::uint32_t& capacity, std::uint32_t needed,
                          rhi::BufferUsage usage) {
        if (needed <= capacity && buffer) {
            return true;
        }
        std::uint32_t size = std::max(capacity, 64u * 1024u);
        while (size < needed) {
            size *= 2;
        }
        const rhi::BufferHandle fresh = device_->create_buffer({.usage = usage, .size = size});
        if (!fresh) {
            return false;
        }
        device_->destroy_buffer(buffer);
        buffer = fresh;
        capacity = size;
        return true;
    };
    return grow(vertex_buffer_, vertex_capacity_, vertex_bytes, rhi::BufferUsage::Vertex) &&
           grow(index_buffer_, index_capacity_, index_bytes, rhi::BufferUsage::Index);
}

void ImGuiLayer::update_textures() noexcept {
    // The 1.92 protocol: ImGui lists the textures it wants and what it
    // wants done with each; the backend does it and says so. The atlas grows
    // as glyphs are used, so updates arrive after the first frame too; a
    // whole re-upload of the level is simplest and rare.
    for (ImTextureData* texture : ImGui::GetPlatformIO().Textures) {
        if (texture->Status == ImTextureStatus_OK) {
            continue;
        }
        if (texture->Status == ImTextureStatus_WantCreate) {
            TY_ASSERT(texture->Format == ImTextureFormat_RGBA32, "ImGui textures are RGBA8");
            const rhi::TextureHandle handle =
                device_->create_texture({.format = rhi::TextureFormat::Rgba8Unorm,
                                         .width = static_cast<std::uint32_t>(texture->Width),
                                         .height = static_cast<std::uint32_t>(texture->Height),
                                         .usage = rhi::TextureUsage::Sampled});
            if (!handle) {
                TY_LOG_ERROR("ui", "texture %dx%d failed: %s", texture->Width, texture->Height,
                             platform::last_error());
                continue; // asked again next frame
            }
            texture->SetTexID(texture_id(handle));
        }
        if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates) {
            const rhi::TextureHandle handle = texture_from_id(texture->GetTexID());
            const auto bytes = static_cast<std::uint32_t>(texture->GetSizeInBytes());
            if (!device_->upload_texture(handle, texture->GetPixels(), bytes)) {
                TY_LOG_ERROR("ui", "texture upload failed: %s", platform::last_error());
                continue;
            }
            texture->SetStatus(ImTextureStatus_OK);
        }
        if (texture->Status == ImTextureStatus_WantDestroy && texture->UnusedFrames > 0) {
            if (texture->GetTexID() != ImTextureID_Invalid) {
                device_->destroy_texture(texture_from_id(texture->GetTexID()));
                texture->SetTexID(ImTextureID_Invalid);
            }
            texture->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

bool ImGuiLayer::upload(rhi::Frame& frame) noexcept {
    if (context_ == nullptr || device_ == nullptr || !pipeline_ || !have_draw_data_) {
        return true;
    }
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context_));
    update_textures();
    const ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data == nullptr || draw_data->TotalVtxCount == 0) {
        return true;
    }
    const auto vertex_bytes = static_cast<std::uint32_t>(draw_data->TotalVtxCount) * sizeof(ImDrawVert);
    const auto index_bytes = static_cast<std::uint32_t>(draw_data->TotalIdxCount) * sizeof(ImDrawIdx);
    if (!ensure_buffers(static_cast<std::uint32_t>(vertex_bytes), static_cast<std::uint32_t>(index_bytes))) {
        TY_LOG_ERROR("ui", "vertex or index buffer failed: %s", platform::last_error());
        return false;
    }
    std::uint32_t vertex_offset = 0, index_offset = 0;
    for (const ImDrawList* list : draw_data->CmdLists) {
        const auto list_vertex_bytes = static_cast<std::uint32_t>(list->VtxBuffer.Size) * sizeof(ImDrawVert);
        const auto list_index_bytes = static_cast<std::uint32_t>(list->IdxBuffer.Size) * sizeof(ImDrawIdx);
        if (list_vertex_bytes > 0 &&
            !frame.write_buffer(vertex_buffer_, list->VtxBuffer.Data,
                                static_cast<std::uint32_t>(list_vertex_bytes), vertex_offset)) {
            return false;
        }
        if (list_index_bytes > 0 &&
            !frame.write_buffer(index_buffer_, list->IdxBuffer.Data,
                                static_cast<std::uint32_t>(list_index_bytes), index_offset)) {
            return false;
        }
        vertex_offset += static_cast<std::uint32_t>(list_vertex_bytes);
        index_offset += static_cast<std::uint32_t>(list_index_bytes);
    }
    return true;
}

void ImGuiLayer::draw(rhi::RenderPass& pass) noexcept {
    if (context_ == nullptr || device_ == nullptr || !pipeline_ || !have_draw_data_) {
        return;
    }
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context_));
    const ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data == nullptr || draw_data->TotalVtxCount == 0 || !vertex_buffer_ || !index_buffer_) {
        return;
    }
    const float fb_width = draw_data->DisplaySize.x * draw_data->FramebufferScale.x;
    const float fb_height = draw_data->DisplaySize.y * draw_data->FramebufferScale.y;
    if (fb_width <= 0.0f || fb_height <= 0.0f) {
        return;
    }

    // Points to clip: the display rectangle onto [-1, 1], y up.
    const float l = draw_data->DisplayPos.x, r = l + draw_data->DisplaySize.x;
    const float t = draw_data->DisplayPos.y, b = t + draw_data->DisplaySize.y;
    Uniforms uniforms{};
    const float projection[16] = {
        2.0f / (r - l), 0.0f, 0.0f, 0.0f, 0.0f, 2.0f / (t - b),    0.0f,
        0.0f,           0.0f, 0.0f, 1.0f, 0.0f, (r + l) / (l - r), (t + b) / (b - t),
        0.0f,           1.0f};
    std::memcpy(uniforms.projection, projection, sizeof projection);
    uniforms.params[0] = target_is_srgb_ ? 1.0f : 0.0f;

    const auto reset_state = [&] {
        pass.bind_pipeline(pipeline_);
        pass.bind_vertex_buffer(vertex_buffer_);
        pass.bind_index_buffer(index_buffer_, rhi::IndexType::Uint16);
        pass.push_vertex_uniforms(0, &uniforms, sizeof uniforms);
        pass.push_fragment_uniforms(0, &uniforms, sizeof uniforms);
    };
    reset_state();

    const ImVec2 clip_offset = draw_data->DisplayPos;
    const ImVec2 clip_scale = draw_data->FramebufferScale;
    std::uint32_t vertex_base = 0, index_base = 0;
    for (const ImDrawList* list : draw_data->CmdLists) {
        for (const ImDrawCmd& command : list->CmdBuffer) {
            if (command.UserCallback != nullptr) {
                if (command.UserCallback == reset_render_state) {
                    reset_state();
                } else {
                    command.UserCallback(list, &command);
                }
                continue;
            }
            const float x0 = std::max((command.ClipRect.x - clip_offset.x) * clip_scale.x, 0.0f);
            const float y0 = std::max((command.ClipRect.y - clip_offset.y) * clip_scale.y, 0.0f);
            const float x1 = std::min((command.ClipRect.z - clip_offset.x) * clip_scale.x, fb_width);
            const float y1 = std::min((command.ClipRect.w - clip_offset.y) * clip_scale.y, fb_height);
            if (x1 <= x0 || y1 <= y0) {
                continue;
            }
            pass.set_scissor(static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0),
                             static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0));
            const rhi::TextureHandle texture = texture_from_id(command.GetTexID());
            if (!device_->valid(texture)) {
                continue; // a texture that went away: nothing sensible to draw it with
            }
            pass.bind_fragment_texture(0, texture, sampler_);
            pass.draw_indexed(command.ElemCount, command.IdxOffset + index_base,
                              static_cast<std::int32_t>(command.VtxOffset + vertex_base));
        }
        vertex_base += static_cast<std::uint32_t>(list->VtxBuffer.Size);
        index_base += static_cast<std::uint32_t>(list->IdxBuffer.Size);
    }
    pass.set_scissor(0, 0, static_cast<std::uint32_t>(fb_width), static_cast<std::uint32_t>(fb_height));
}

bool ImGuiLayer::has_draw_data() const noexcept {
    if (context_ == nullptr || !have_draw_data_) {
        return false;
    }
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context_));
    const ImDrawData* draw_data = ImGui::GetDrawData();
    return draw_data != nullptr && draw_data->TotalVtxCount > 0;
}

bool ImGuiLayer::wants_mouse() const noexcept {
    if (context_ == nullptr) {
        return false;
    }
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context_));
    return ImGui::GetIO().WantCaptureMouse;
}

bool ImGuiLayer::wants_keyboard() const noexcept {
    if (context_ == nullptr) {
        return false;
    }
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context_));
    return ImGui::GetIO().WantCaptureKeyboard;
}

} // namespace tynima::ui
