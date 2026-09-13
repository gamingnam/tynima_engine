#pragma once

#include <cstdint>

// Engine version. This header is the single source of truth; the top-level
// CMakeLists.txt project() line mirrors it and core's tests check they agree.
#define TYNIMA_VERSION_MAJOR 0
#define TYNIMA_VERSION_MINOR 1
#define TYNIMA_VERSION_PATCH 0

#define TYNIMA_STRINGIFY_(x) #x
#define TYNIMA_STRINGIFY(x) TYNIMA_STRINGIFY_(x)
#define TYNIMA_VERSION_STRING                                                       \
    TYNIMA_STRINGIFY(TYNIMA_VERSION_MAJOR) "." TYNIMA_STRINGIFY(TYNIMA_VERSION_MINOR) \
    "." TYNIMA_STRINGIFY(TYNIMA_VERSION_PATCH)

namespace tynima::core {

struct Version {
    std::uint16_t major;
    std::uint16_t minor;
    std::uint16_t patch;
};

inline constexpr Version kVersion{TYNIMA_VERSION_MAJOR, TYNIMA_VERSION_MINOR, TYNIMA_VERSION_PATCH};

// "major.minor.patch", static storage — never freed by the caller.
const char* version_string() noexcept;

} // namespace tynima::core
