#pragma once

#include <tynima/core/bytes.h>

#include <cstddef>
#include <cstdint>

namespace tynima::platform {

// Last modification time of a file, as an opaque monotonic-enough integer:
// compare for change, not for meaning. 0 when the file does not exist.
// No allocation, so it is safe to poll every frame.
[[nodiscard]] std::uint64_t file_write_time(const char* path) noexcept;

// Byte-for-byte copy; false with errno-style failure when either side fails.
[[nodiscard]] bool copy_file(const char* from, const char* to) noexcept;

[[nodiscard]] bool remove_file(const char* path) noexcept;

// The whole file into `out` (replaced). false, with `out` empty, when it
// cannot be opened or read to the end.
[[nodiscard]] bool read_file(const char* path, core::Bytes& out) noexcept;
// `size` bytes over whatever was there. Not atomic: a reader can see a
// partial file — write beside and rename_file() over when that matters.
[[nodiscard]] bool write_file(const char* path, const void* data, std::size_t size) noexcept;
// Moves `from` over `to`, replacing it in one step on every platform, so a
// reader sees the old file or the new one, never a piece of either.
[[nodiscard]] bool rename_file(const char* from, const char* to) noexcept;
// Every directory along `path` that does not exist yet (mkdir -p). true
// when the directory is there afterwards, whoever made it.
[[nodiscard]] bool make_directories(const char* path) noexcept;
[[nodiscard]] bool directory_exists(const char* path) noexcept;
// This process's id: what makes a temporary file's name its own.
[[nodiscard]] std::uint32_t process_id() noexcept;

} // namespace tynima::platform
