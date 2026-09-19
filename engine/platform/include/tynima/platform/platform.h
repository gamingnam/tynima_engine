#pragma once

#include <cstdint>

namespace tynima::platform {

struct InitOptions {
    // Use SDL's dummy video driver: no OS window is created, but Window, Input
    // and pump_events() all behave. For unit tests and CI runners without a display.
    bool headless = false;
};

// Starts the platform layer (video and events). Call once, on the main thread —
// macOS requires window and event work to happen there. Returns false on
// failure; last_error() says why. Calling it again while initialised is a no-op.
[[nodiscard]] bool init(const InitOptions& options = {});
void shutdown() noexcept;
[[nodiscard]] bool is_initialized() noexcept;

// Text for the most recent platform-layer failure. Never null, may be empty.
[[nodiscard]] const char* last_error() noexcept;

// The shape of the mouse cursor, from the OS's own set.
enum class Cursor : std::uint8_t {
    Arrow,
    Text,       // an I-beam, over text
    ResizeAll,  // four arrows
    ResizeNS,   // a horizontal border
    ResizeEW,   // a vertical border
    ResizeNESW, // a corner
    ResizeNWSE,
    Hand,       // over a link
    NotAllowed,
    Wait,
    Progress,   // busy, but still interactive
    Hidden,     // no cursor at all
};
// Shows `cursor` until the next call. Cheap to call every frame with the
// same value. Main thread only.
void set_cursor(Cursor cursor) noexcept;

// The system clipboard as text. get returns "" when there is none, and the
// pointer stays valid until the next call. set copies `text`.
[[nodiscard]] const char* clipboard_text() noexcept;
void set_clipboard_text(const char* text) noexcept;

} // namespace tynima::platform
