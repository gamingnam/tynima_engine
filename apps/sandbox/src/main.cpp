// Engine developer's playground. Phase 0 turns this into a window, then a
// triangle through SDL3 GPU with MSL compiled at runtime.
#include <tynima/core/version.h>

#include <cstdio>

int main() {
    std::printf("tynima sandbox - engine %s\n", tynima::core::version_string());
    return 0;
}
