#include <tynima/platform/input.h>

#include "sdl.h"

#include <array>

namespace tynima::platform {

namespace {

constexpr const char* kKeyNames[] = {
    "Unknown", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q",
    "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "Digit0", "Digit1", "Digit2", "Digit3", "Digit4",
    "Digit5", "Digit6", "Digit7", "Digit8", "Digit9", "F1", "F2", "F3", "F4", "F5", "F6", "F7",
    "F8", "F9", "F10", "F11", "F12", "Escape", "Enter", "Tab", "Backspace", "Space", "Minus",
    "Equals", "LeftBracket", "RightBracket", "Backslash", "Semicolon", "Apostrophe", "Grave",
    "Comma", "Period", "Slash", "CapsLock", "Insert", "Delete", "Home", "End", "PageUp",
    "PageDown", "Left", "Right", "Up", "Down", "LeftShift", "RightShift", "LeftCtrl", "RightCtrl",
    "LeftAlt", "RightAlt", "LeftSuper", "RightSuper",
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
        t[SDL_SCANCODE_A] = Key::A;
        t[SDL_SCANCODE_B] = Key::B;
        t[SDL_SCANCODE_C] = Key::C;
        t[SDL_SCANCODE_D] = Key::D;
        t[SDL_SCANCODE_E] = Key::E;
        t[SDL_SCANCODE_F] = Key::F;
        t[SDL_SCANCODE_G] = Key::G;
        t[SDL_SCANCODE_H] = Key::H;
        t[SDL_SCANCODE_I] = Key::I;
        t[SDL_SCANCODE_J] = Key::J;
        t[SDL_SCANCODE_K] = Key::K;
        t[SDL_SCANCODE_L] = Key::L;
        t[SDL_SCANCODE_M] = Key::M;
        t[SDL_SCANCODE_N] = Key::N;
        t[SDL_SCANCODE_O] = Key::O;
        t[SDL_SCANCODE_P] = Key::P;
        t[SDL_SCANCODE_Q] = Key::Q;
        t[SDL_SCANCODE_R] = Key::R;
        t[SDL_SCANCODE_S] = Key::S;
        t[SDL_SCANCODE_T] = Key::T;
        t[SDL_SCANCODE_U] = Key::U;
        t[SDL_SCANCODE_V] = Key::V;
        t[SDL_SCANCODE_W] = Key::W;
        t[SDL_SCANCODE_X] = Key::X;
        t[SDL_SCANCODE_Y] = Key::Y;
        t[SDL_SCANCODE_Z] = Key::Z;
        t[SDL_SCANCODE_0] = Key::Digit0;
        t[SDL_SCANCODE_1] = Key::Digit1;
        t[SDL_SCANCODE_2] = Key::Digit2;
        t[SDL_SCANCODE_3] = Key::Digit3;
        t[SDL_SCANCODE_4] = Key::Digit4;
        t[SDL_SCANCODE_5] = Key::Digit5;
        t[SDL_SCANCODE_6] = Key::Digit6;
        t[SDL_SCANCODE_7] = Key::Digit7;
        t[SDL_SCANCODE_8] = Key::Digit8;
        t[SDL_SCANCODE_9] = Key::Digit9;
        t[SDL_SCANCODE_F1] = Key::F1;
        t[SDL_SCANCODE_F2] = Key::F2;
        t[SDL_SCANCODE_F3] = Key::F3;
        t[SDL_SCANCODE_F4] = Key::F4;
        t[SDL_SCANCODE_F5] = Key::F5;
        t[SDL_SCANCODE_F6] = Key::F6;
        t[SDL_SCANCODE_F7] = Key::F7;
        t[SDL_SCANCODE_F8] = Key::F8;
        t[SDL_SCANCODE_F9] = Key::F9;
        t[SDL_SCANCODE_F10] = Key::F10;
        t[SDL_SCANCODE_F11] = Key::F11;
        t[SDL_SCANCODE_F12] = Key::F12;
        t[SDL_SCANCODE_ESCAPE] = Key::Escape;
        t[SDL_SCANCODE_RETURN] = Key::Enter;
        t[SDL_SCANCODE_TAB] = Key::Tab;
        t[SDL_SCANCODE_BACKSPACE] = Key::Backspace;
        t[SDL_SCANCODE_SPACE] = Key::Space;
        t[SDL_SCANCODE_MINUS] = Key::Minus;
        t[SDL_SCANCODE_EQUALS] = Key::Equals;
        t[SDL_SCANCODE_LEFTBRACKET] = Key::LeftBracket;
        t[SDL_SCANCODE_RIGHTBRACKET] = Key::RightBracket;
        t[SDL_SCANCODE_BACKSLASH] = Key::Backslash;
        t[SDL_SCANCODE_SEMICOLON] = Key::Semicolon;
        t[SDL_SCANCODE_APOSTROPHE] = Key::Apostrophe;
        t[SDL_SCANCODE_GRAVE] = Key::Grave;
        t[SDL_SCANCODE_COMMA] = Key::Comma;
        t[SDL_SCANCODE_PERIOD] = Key::Period;
        t[SDL_SCANCODE_SLASH] = Key::Slash;
        t[SDL_SCANCODE_CAPSLOCK] = Key::CapsLock;
        t[SDL_SCANCODE_INSERT] = Key::Insert;
        t[SDL_SCANCODE_DELETE] = Key::Delete;
        t[SDL_SCANCODE_HOME] = Key::Home;
        t[SDL_SCANCODE_END] = Key::End;
        t[SDL_SCANCODE_PAGEUP] = Key::PageUp;
        t[SDL_SCANCODE_PAGEDOWN] = Key::PageDown;
        t[SDL_SCANCODE_LEFT] = Key::Left;
        t[SDL_SCANCODE_RIGHT] = Key::Right;
        t[SDL_SCANCODE_UP] = Key::Up;
        t[SDL_SCANCODE_DOWN] = Key::Down;
        t[SDL_SCANCODE_LSHIFT] = Key::LeftShift;
        t[SDL_SCANCODE_RSHIFT] = Key::RightShift;
        t[SDL_SCANCODE_LCTRL] = Key::LeftCtrl;
        t[SDL_SCANCODE_RCTRL] = Key::RightCtrl;
        t[SDL_SCANCODE_LALT] = Key::LeftAlt;
        t[SDL_SCANCODE_RALT] = Key::RightAlt;
        t[SDL_SCANCODE_LGUI] = Key::LeftSuper;
        t[SDL_SCANCODE_RGUI] = Key::RightSuper;
        return t;
    }();
    const auto i = static_cast<std::size_t>(scancode);
    return i < table.size() ? table[i] : Key::Unknown;
}

} // namespace internal

} // namespace tynima::platform
