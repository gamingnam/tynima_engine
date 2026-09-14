#include <tynima/platform/input.h>

#include "sdl.h"

#include <array>

namespace tynima::platform {

namespace {

constexpr const char* kKeyNames[] = {
#define TYNIMA_KEY(name, scancode) #name,
#include <tynima/platform/keys.def>
#undef TYNIMA_KEY
};
static_assert(sizeof(kKeyNames) / sizeof(kKeyNames[0]) == kKeyCount, "key_name table is out of step with Key");

constexpr std::size_t index(Key key) noexcept {
    return static_cast<std::size_t>(key);
}
constexpr std::size_t index(MouseButton button) noexcept {
    return static_cast<std::size_t>(button);
}

} // namespace

const char* key_name(Key key) noexcept {
    const std::size_t i = index(key);
    return i < kKeyCount ? kKeyNames[i] : "Invalid";
}

bool Input::key_down(Key key) const noexcept {
    return index(key) < kKeyCount && keys_down_[index(key)];
}
bool Input::key_pressed(Key key) const noexcept {
    return index(key) < kKeyCount && keys_pressed_[index(key)];
}
bool Input::key_released(Key key) const noexcept {
    return index(key) < kKeyCount && keys_released_[index(key)];
}

bool Input::mouse_down(MouseButton button) const noexcept {
    return index(button) < kMouseButtonCount && buttons_down_[index(button)];
}
bool Input::mouse_pressed(MouseButton button) const noexcept {
    return index(button) < kMouseButtonCount && buttons_pressed_[index(button)];
}
bool Input::mouse_released(MouseButton button) const noexcept {
    return index(button) < kMouseButtonCount && buttons_released_[index(button)];
}

void Input::Writer::begin_frame(Input& input) noexcept {
    input.keys_pressed_.reset();
    input.keys_released_.reset();
    input.buttons_pressed_.reset();
    input.buttons_released_.reset();
    input.mouse_dx_ = 0.0f;
    input.mouse_dy_ = 0.0f;
    input.wheel_x_ = 0.0f;
    input.wheel_y_ = 0.0f;
}

void Input::Writer::key(Input& input, Key key, bool down, bool repeat) noexcept {
    const std::size_t i = index(key);
    if (i >= kKeyCount) {
        return;
    }
    if (down) {
        if (!repeat) {
            input.keys_pressed_[i] = true;
        }
        input.keys_down_[i] = true;
    } else {
        input.keys_released_[i] = true;
        input.keys_down_[i] = false;
    }
}

void Input::Writer::mouse_button(Input& input, MouseButton button, bool down) noexcept {
    const std::size_t i = index(button);
    if (i >= kMouseButtonCount) {
        return;
    }
    if (down) {
        input.buttons_pressed_[i] = true;
        input.buttons_down_[i] = true;
    } else {
        input.buttons_released_[i] = true;
        input.buttons_down_[i] = false;
    }
}

void Input::Writer::mouse_move(Input& input, float x, float y, float dx, float dy) noexcept {
    input.mouse_x_ = x;
    input.mouse_y_ = y;
    input.mouse_dx_ += dx;
    input.mouse_dy_ += dy;
}

void Input::Writer::wheel(Input& input, float dx, float dy) noexcept {
    input.wheel_x_ += dx;
    input.wheel_y_ += dy;
}

namespace internal {

Key key_from_scancode(SDL_Scancode scancode) noexcept {
    static const auto table = [] {
        std::array<Key, SDL_SCANCODE_COUNT> t{};
        t.fill(Key::Unknown);
#define TYNIMA_KEY(name, scancode) t[scancode] = Key::name;
#include <tynima/platform/keys.def>
#undef TYNIMA_KEY
        return t;
    }();
    const auto i = static_cast<std::size_t>(scancode);
    return i < table.size() ? table[i] : Key::Unknown;
}

} // namespace internal

} // namespace tynima::platform
