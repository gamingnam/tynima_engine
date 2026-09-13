#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>

namespace tynima::platform {

// Physical key positions (USB HID scancodes underneath), so WASD is WASD on
// every keyboard layout. Typed text arrives through events, never through here.
// The enum, key_name() and the scancode table are generated from one list;
// keep them together if you add a key.
enum class Key : std::uint8_t {
    Unknown, A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z, Digit0,
    Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9, F1, F2, F3, F4, F5, F6,
    F7, F8, F9, F10, F11, F12, Escape, Enter, Tab, Backspace, Space, Minus, Equals, LeftBracket,
    RightBracket, Backslash, Semicolon, Apostrophe, Grave, Comma, Period, Slash, CapsLock, Insert,
    Delete, Home, End, PageUp, PageDown, Left, Right, Up, Down, LeftShift, RightShift, LeftCtrl,
    RightCtrl, LeftAlt, RightAlt, LeftSuper, RightSuper,
    Count
};
inline constexpr std::size_t kKeyCount = static_cast<std::size_t>(Key::Count);

enum class MouseButton : std::uint8_t { Left = 0, Middle, Right, X1, X2, Count };
inline constexpr std::size_t kMouseButtonCount = static_cast<std::size_t>(MouseButton::Count);

// Stable, human-readable name ("A", "LeftShift"). Never null.
const char* key_name(Key key) noexcept;

// Frame-coherent input snapshot. pump_events() advances it once per frame:
// down() is the level, pressed()/released() are the edges inside that frame,
// and a press and release within one frame report both edges so short taps
// are never lost. Key repeat never re-triggers pressed().
class Input {
public:
    [[nodiscard]] bool key_down(Key key) const noexcept;
    [[nodiscard]] bool key_pressed(Key key) const noexcept;
    [[nodiscard]] bool key_released(Key key) const noexcept;

    [[nodiscard]] bool mouse_down(MouseButton button) const noexcept;
    [[nodiscard]] bool mouse_pressed(MouseButton button) const noexcept;
    [[nodiscard]] bool mouse_released(MouseButton button) const noexcept;

    // Cursor position in window points, and motion accumulated over the frame.
    // In relative mouse mode the position stops meaning anything; use the deltas.
    [[nodiscard]] float mouse_x() const noexcept { return mouse_x_; }
    [[nodiscard]] float mouse_y() const noexcept { return mouse_y_; }
    [[nodiscard]] float mouse_dx() const noexcept { return mouse_dx_; }
    [[nodiscard]] float mouse_dy() const noexcept { return mouse_dy_; }

    // Scroll accumulated over the frame, in notches; +y scrolls content up.
    [[nodiscard]] float wheel_x() const noexcept { return wheel_x_; }
    [[nodiscard]] float wheel_y() const noexcept { return wheel_y_; }

    // The write side. pump_events() drives it from OS events; tests drive it
    // directly, which is why it is public.
    struct Writer {
        static void begin_frame(Input& input) noexcept;
        static void key(Input& input, Key key, bool down, bool repeat) noexcept;
        static void mouse_button(Input& input, MouseButton button, bool down) noexcept;
        static void mouse_move(Input& input, float x, float y, float dx, float dy) noexcept;
        static void wheel(Input& input, float dx, float dy) noexcept;
    };

private:
    std::bitset<kKeyCount> keys_down_;
    std::bitset<kKeyCount> keys_pressed_;
    std::bitset<kKeyCount> keys_released_;
    std::bitset<kMouseButtonCount> buttons_down_;
    std::bitset<kMouseButtonCount> buttons_pressed_;
    std::bitset<kMouseButtonCount> buttons_released_;
    float mouse_x_ = 0.0f;
    float mouse_y_ = 0.0f;
    float mouse_dx_ = 0.0f;
    float mouse_dy_ = 0.0f;
    float wheel_x_ = 0.0f;
    float wheel_y_ = 0.0f;
};

} // namespace tynima::platform
