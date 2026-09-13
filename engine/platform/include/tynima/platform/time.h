#pragma once

#include <cstdint>

namespace tynima::platform {

// Monotonic nanoseconds from an arbitrary origin; only differences mean anything.
[[nodiscard]] std::uint64_t now_ns() noexcept;

// Seconds since the first call to a time function in this process.
[[nodiscard]] double now_seconds() noexcept;

// Blocks the calling thread. Not precise enough for frame pacing — vsync is.
void sleep_ns(std::uint64_t ns) noexcept;

} // namespace tynima::platform
