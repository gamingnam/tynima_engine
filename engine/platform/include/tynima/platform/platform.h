#pragma once

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

} // namespace tynima::platform
