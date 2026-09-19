#include <tynima/platform/platform.h>

#include <tynima/core/memory.h>
#include <tynima/core/version.h>

#include "sdl.h"

#include <cstddef>
#include <string>

namespace tynima::platform {

namespace {
bool g_initialized = false;
// The system cursors, made the first time each is asked for and freed at shutdown.
constexpr std::size_t kCursorCount = static_cast<std::size_t>(Cursor::Hidden) + 1;
SDL_Cursor* g_cursors[kCursorCount] = {};
Cursor g_cursor = Cursor::Arrow;
bool g_cursor_set = false;
std::string g_clipboard; // the last text read, for a caller that keeps the pointer for a while

SDL_SystemCursor to_sdl(Cursor cursor) noexcept {
    switch (cursor) {
    case Cursor::Arrow: return SDL_SYSTEM_CURSOR_DEFAULT;
    case Cursor::Text: return SDL_SYSTEM_CURSOR_TEXT;
    case Cursor::ResizeAll: return SDL_SYSTEM_CURSOR_MOVE;
    case Cursor::ResizeNS: return SDL_SYSTEM_CURSOR_NS_RESIZE;
    case Cursor::ResizeEW: return SDL_SYSTEM_CURSOR_EW_RESIZE;
    case Cursor::ResizeNESW: return SDL_SYSTEM_CURSOR_NESW_RESIZE;
    case Cursor::ResizeNWSE: return SDL_SYSTEM_CURSOR_NWSE_RESIZE;
    case Cursor::Hand: return SDL_SYSTEM_CURSOR_POINTER;
    case Cursor::NotAllowed: return SDL_SYSTEM_CURSOR_NOT_ALLOWED;
    case Cursor::Wait: return SDL_SYSTEM_CURSOR_WAIT;
    case Cursor::Progress: return SDL_SYSTEM_CURSOR_PROGRESS;
    case Cursor::Hidden: break;
    }
    return SDL_SYSTEM_CURSOR_DEFAULT;
}
} // namespace

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
        for (SDL_Cursor*& cursor : g_cursors) {
            if (cursor != nullptr) {
                SDL_DestroyCursor(cursor);
                cursor = nullptr;
            }
        }
        g_cursor_set = false;
        SDL_Quit();
        g_initialized = false;
    }
}

void set_cursor(Cursor cursor) noexcept {
    if (!g_initialized || (g_cursor_set && cursor == g_cursor)) {
        return;
    }
    TY_EXTERNAL_ALLOCATIONS();
    g_cursor = cursor;
    g_cursor_set = true;
    if (cursor == Cursor::Hidden) {
        SDL_HideCursor();
        return;
    }
    SDL_Cursor*& handle = g_cursors[static_cast<std::size_t>(cursor)];
    if (handle == nullptr) {
        handle = SDL_CreateSystemCursor(to_sdl(cursor));
    }
    if (handle != nullptr) {
        SDL_SetCursor(handle);
    }
    SDL_ShowCursor();
}

const char* clipboard_text() noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    char* text = SDL_GetClipboardText(); // never null: "" when there is nothing, freed by us
    g_clipboard = text != nullptr ? text : "";
    SDL_free(text);
    return g_clipboard.c_str();
}

void set_clipboard_text(const char* text) noexcept {
    TY_EXTERNAL_ALLOCATIONS();
    SDL_SetClipboardText(text != nullptr ? text : "");
}

bool is_initialized() noexcept {
    return g_initialized;
}

const char* last_error() noexcept {
    return SDL_GetError();
}

} // namespace tynima::platform
