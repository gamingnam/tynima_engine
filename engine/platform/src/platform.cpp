#include <tynima/platform/platform.h>

#include <tynima/core/memory.h>
#include <tynima/core/version.h>

#include "sdl.h"

namespace tynima::platform {

namespace {
bool g_initialized = false;
}

bool init(const InitOptions& options) {
    if (g_initialized) {
        return true;
    }
    TY_EXTERNAL_ALLOCATIONS();
    SDL_SetAppMetadata("Tynima", TYNIMA_VERSION_STRING, "dev.tynima.engine");
    if (options.headless) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    } else {
        SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return false;
    }
    g_initialized = true;
    return true;
}

void shutdown() noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    if (g_initialized) {
        SDL_Quit();
        g_initialized = false;
    }
}

bool is_initialized() noexcept {
    return g_initialized;
}

const char* last_error() noexcept {
    return SDL_GetError();
}

} // namespace tynima::platform
