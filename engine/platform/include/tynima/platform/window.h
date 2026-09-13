#pragma once

#include <cstdint>
#include <memory>

struct SDL_Window;

namespace tynima::platform {

struct WindowDesc {
    const char* title = "Tynima";
    int width = 1280;  // points
    int height = 720;  // points
    bool resizable = true;
    bool high_pixel_density = true; // ask for a Retina/HiDPI backbuffer
};

// One OS window. Owns the handle; destroying the object closes the window.
class Window {
public:
    // nullptr on failure — last_error() says why. Main thread only.
    [[nodiscard]] static std::unique_ptr<Window> create(const WindowDesc& desc);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    [[nodiscard]] std::uint32_t id() const noexcept;

    // Points are what the OS lays out in; pixels are what you render to.
    // On a Retina display the two differ by pixel_density() (2.0).
    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;
    [[nodiscard]] int pixel_width() const noexcept;
    [[nodiscard]] int pixel_height() const noexcept;
    [[nodiscard]] float pixel_density() const noexcept;

    void set_title(const char* title) noexcept;

    // Hides the cursor and reports unbounded relative motion — for fly cameras.
    void set_relative_mouse_mode(bool enabled) noexcept;

    // The underlying SDL_Window*, for rhi/ to attach a swapchain. Nothing above
    // rhi/ should need this.
    [[nodiscard]] void* native_handle() const noexcept;

private:
    explicit Window(SDL_Window* handle) noexcept : handle_(handle) {}
    SDL_Window* handle_;
};

} // namespace tynima::platform
