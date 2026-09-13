#include <tynima/platform/window.h>

#include <tynima/platform/platform.h>

#include "sdl.h"

namespace tynima::platform {

std::unique_ptr<Window> Window::create(const WindowDesc& desc) {
    if (!is_initialized()) {
        SDL_SetError("platform::init() has not been called");
        return nullptr;
    }
    SDL_WindowFlags flags = 0;
    if (desc.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (desc.high_pixel_density) {
        flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    }
    SDL_Window* handle = SDL_CreateWindow(desc.title, desc.width, desc.height, flags);
    if (handle == nullptr) {
        return nullptr;
    }
    return std::unique_ptr<Window>(new Window(handle));
}

Window::~Window() {
    SDL_DestroyWindow(handle_);
}

std::uint32_t Window::id() const noexcept {
    return SDL_GetWindowID(handle_);
}

int Window::width() const noexcept {
    int w = 0;
    SDL_GetWindowSize(handle_, &w, nullptr);
    return w;
}

int Window::height() const noexcept {
    int h = 0;
    SDL_GetWindowSize(handle_, nullptr, &h);
    return h;
}

int Window::pixel_width() const noexcept {
    int w = 0;
    SDL_GetWindowSizeInPixels(handle_, &w, nullptr);
    return w;
}

int Window::pixel_height() const noexcept {
    int h = 0;
    SDL_GetWindowSizeInPixels(handle_, nullptr, &h);
    return h;
}

float Window::pixel_density() const noexcept {
    const float density = SDL_GetWindowPixelDensity(handle_);
    return density > 0.0f ? density : 1.0f;
}

void Window::set_title(const char* title) noexcept {
    SDL_SetWindowTitle(handle_, title);
}

void Window::set_relative_mouse_mode(bool enabled) noexcept {
    SDL_SetWindowRelativeMouseMode(handle_, enabled);
}

void* Window::native_handle() const noexcept {
    return handle_;
}

} // namespace tynima::platform
