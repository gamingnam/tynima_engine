#include <tynima/platform/events.h>
#include <tynima/platform/input.h>
#include <tynima/platform/platform.h>
#include <tynima/platform/window.h>
#include <tynima/ui/imgui_layer.h>

#include <doctest/doctest.h>
#include <imgui.h>

#include <string>
#include <vector>

using namespace tynima;

namespace {

struct Session {
    Session() { REQUIRE(platform::init({.headless = true})); }
    ~Session() { platform::shutdown(); }
};

} // namespace

TEST_CASE("the layer runs ImGui without a device: frames, input, a window with widgets") {
    Session session;
    auto window = platform::Window::create({.title = "ui test", .width = 640, .height = 360});
    REQUIRE(window != nullptr);
    ui::ImGuiLayer layer;
    REQUIRE(layer.create(nullptr, rhi::TextureFormat::Bgra8Unorm, false));
    CHECK(layer.ready());
    CHECK_FALSE(layer.can_draw());
    CHECK(layer.context() != nullptr);

    platform::Input input;
    std::vector<platform::Event> events;
    bool clicked = false;
    for (int frame = 0; frame < 5; ++frame) {
        platform::Input::Writer::begin_frame(input);
        // Frame 2: the mouse lands on the button and presses; frame 3 releases it.
        platform::Input::Writer::mouse_move(input, 40.0f, 40.0f, 0.0f, 0.0f);
        if (frame == 2) {
            platform::Input::Writer::mouse_button(input, platform::MouseButton::Left, true);
        } else if (frame == 3) {
            platform::Input::Writer::mouse_button(input, platform::MouseButton::Left, false);
        }
        events.clear();
        if (frame == 1) {
            platform::Event text{};
            text.type = platform::EventType::TextInput;
            text.text[0] = 'h';
            text.text[1] = 'i';
            events.push_back(text);
        }
        layer.begin_frame(*window, input, events, 1.0f / 60.0f);
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(300.0f, 200.0f));
        ImGui::Begin("panel");
        if (ImGui::Button("press me", ImVec2(200.0f, 60.0f))) {
            clicked = true;
        }
        static char buffer[16] = "";
        ImGui::InputText("text", buffer, sizeof buffer);
        ImGui::End();
        layer.end_frame(*window);
        CHECK(ImGui::GetDrawData() != nullptr);
    }
    CHECK(clicked);
    CHECK(layer.wants_mouse()); // the cursor is over the panel
    // The display: the window's size in points, the framebuffer scale from its pixels.
    const ImGuiIO& io = ImGui::GetIO();
    CHECK(io.DisplaySize.x == 640.0f);
    CHECK(io.DisplaySize.y == 360.0f);
    CHECK(io.DisplayFramebufferScale.x > 0.0f);
    // Without a device the font atlas is asked for and never served: ImGui
    // keeps the request open rather than failing.
    bool atlas_pending = false;
    for (const ImTextureData* texture : ImGui::GetPlatformIO().Textures) {
        atlas_pending = atlas_pending || texture->Status == ImTextureStatus_WantCreate;
    }
    CHECK(atlas_pending);
    layer.destroy();
    CHECK_FALSE(layer.ready());
}

TEST_CASE("texture ids round-trip a handle and never collide with ImGui's invalid id") {
    const rhi::TextureHandle handle{7, 3};
    const std::uint64_t id = ui::ImGuiLayer::texture_id(handle);
    CHECK(id != 0);
    CHECK(ui::ImGuiLayer::texture_from_id(id) == handle);
    CHECK(ui::ImGuiLayer::texture_id(rhi::TextureHandle{}) == 0); // the null handle is ImTextureID_Invalid
    CHECK(ui::ImGuiLayer::texture_id(rhi::TextureHandle{0, 1}) != 0);
}
