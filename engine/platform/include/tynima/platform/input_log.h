#pragma once

#include <tynima/platform/input.h>

#include <cstdint>
#include <string>
#include <vector>

// A recording of every frame's input and frame time, and where the run ended
// up: enough to play the run again and check it went the same way. This is
// the other half of a deterministic simulation — the half that turns it into
// a regression test. A text file, so it diffs and can be edited by hand:
//
//   tynima-input-log 1
//   backend Tynima
//   frame 0x1.111112p-6 k=0000...0 p=... m=12,34,0,0
//   ...
//   end frames=300 steps=300 hash=8f3c...
//
// A frame line carries the frame's dt as a hex float (exact), then only the
// fields that are not zero: keys down (k), pressed (p) and released (r) as
// hex bit strings, mouse buttons the same (b, bp, br), the mouse (m: x, y,
// dx, dy) and the wheel (w: x, y), floats again as hex.
namespace tynima::platform {

struct InputFrame {
    float dt = 0.0f;
    std::bitset<kKeyCount> keys_down, keys_pressed, keys_released;
    std::bitset<kMouseButtonCount> buttons_down, buttons_pressed, buttons_released;
    float mouse_x = 0.0f, mouse_y = 0.0f, mouse_dx = 0.0f, mouse_dy = 0.0f;
    float wheel_x = 0.0f, wheel_y = 0.0f;
};

// What `input` holds this frame (after pump_events()), with the frame's dt.
[[nodiscard]] InputFrame capture_frame(const Input& input, float dt) noexcept;

// Drives `input` to hold exactly `frame`, edges and all, as pump_events()
// would have from the original events. Call once per frame instead of it.
void apply_frame(const InputFrame& frame, Input& input) noexcept;

struct InputLog {
    std::string backend;            // whose simulation this was recorded on
    std::vector<InputFrame> frames; // one per frame, in order
    std::uint64_t steps = 0;        // physics steps the run took
    std::uint64_t state_hash = 0;   // the world's hash at the end
    bool has_end = false;           // false for a recording that never finished

    bool save(const char* path, std::string& error) const;
    bool load(const char* path, std::string& error);
};

} // namespace tynima::platform
