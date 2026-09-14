#include <tynima/core/assert.h>

#include <cstdio>
#include <cstdlib>

namespace tynima::core {

void assert_failed(const char* condition, const char* message, const char* file, int line) noexcept {
    std::fprintf(stderr, "%s:%d: assertion failed: %s\n    %s\n", file, line, condition, message);
    std::fflush(stderr);
    TY_DEBUG_BREAK();
    std::abort(); // reached only when the trap did not stop the process
}

} // namespace tynima::core
