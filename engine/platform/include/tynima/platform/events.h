#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tynima::platform {

class Input;

enum class EventType : std::uint8_t {
    Quit,             // the OS asked the application to exit (Cmd-Q, SIGTERM)
    WindowClose,      // the close button on one window
    WindowResized,    // width/height and pixel_width/pixel_height are filled in
    WindowFocusGained,
    WindowFocusLost,
    TextInput,        // text typed, once Window::set_text_input(true): `text` is filled in
};

inline constexpr std::size_t kMaxEventText = 32; // one TextInput event's UTF-8 bytes, plus the terminator

struct Event {
    EventType type;
    std::uint32_t window_id = 0; // 0 for Quit
    int width = 0;               // WindowResized: points
    int height = 0;
    int pixel_width = 0;         // WindowResized: pixels
    int pixel_height = 0;
    char text[kMaxEventText] = {}; // TextInput: UTF-8, NUL-terminated; longer input arrives as several events
};

// Drains the OS event queue once: starts a new input frame on `input`, applies
// this frame's key/mouse events to it, and replaces `events` with this frame's
// window and application events. Call exactly once per frame, on the main thread.
void pump_events(Input& input, std::vector<Event>& events);

} // namespace tynima::platform
