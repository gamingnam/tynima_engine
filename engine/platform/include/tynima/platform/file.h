#pragma once

#include <cstdint>

namespace tynima::platform {

// Last modification time of a file, as an opaque monotonic-enough integer:
// compare for change, not for meaning. 0 when the file does not exist.
// No allocation, so it is safe to poll every frame.
[[nodiscard]] std::uint64_t file_write_time(const char* path) noexcept;

// Byte-for-byte copy; false with errno-style failure when either side fails.
[[nodiscard]] bool copy_file(const char* from, const char* to) noexcept;

[[nodiscard]] bool remove_file(const char* path) noexcept;

} // namespace tynima::platform
