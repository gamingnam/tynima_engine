#include <tynima/sdk/game_module.h>

#include <tynima/core/profile.h>
#include <tynima/platform/file.h>
#include <tynima/platform/time.h>

#include <cstdio>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace tynima::sdk {

namespace {

void* open_shared_library(const char* path, std::string& error) {
#if defined(_WIN32)
    HMODULE handle = LoadLibraryA(path);
    if (handle == nullptr) {
        error = "LoadLibrary failed with error " + std::to_string(GetLastError());
    }
    return handle;
#else
    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* reason = dlerror();
        error = reason != nullptr ? reason : "dlopen failed";
    }
    return handle;
#endif
}

void* find_symbol(void* handle, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    return dlsym(handle, name);
#endif
}

void close_shared_library(void* handle) {
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

} // namespace

GameModule::GameModule(std::string path) : path_(std::move(path)) {}

GameModule::~GameModule() {
    // A module still loaded at shutdown was never unloaded by the host; do it
    // without an engine context, which the module must tolerate.
    close_library();
}

bool GameModule::open_library() {
    close_library();
    const std::uint64_t write_time = platform::file_write_time(path_.c_str());
    if (write_time == 0) {
        error_ = "game module not found: " + path_;
        return false;
    }

    // Load a copy so the build can overwrite the original at any time. A new
    // name per load: the dynamic loader caches by path.
    loaded_path_ = path_ + ".loaded." + std::to_string(load_count_ + 1);
    if (!platform::copy_file(path_.c_str(), loaded_path_.c_str())) {
        error_ = "could not copy the game module to " + loaded_path_;
        return false;
    }
    handle_ = open_shared_library(loaded_path_.c_str(), error_);
    if (handle_ == nullptr) {
        (void)platform::remove_file(loaded_path_.c_str());
        return false;
    }
    using EntryFn = const tynima_game* (*)();
    const auto entry = reinterpret_cast<EntryFn>(find_symbol(handle_, TYNIMA_GAME_ENTRY_NAME));
    if (entry == nullptr) {
        error_ = "the game module does not export " TYNIMA_GAME_ENTRY_NAME;
        close_library();
        return false;
    }
    const tynima_game* game = entry();
    if (game == nullptr || game->api_version != TYNIMA_API_VERSION) {
        error_ = "the game module was built against API version " +
                 std::to_string(game != nullptr ? game->api_version : 0u) + ", the engine speaks " +
                 std::to_string(TYNIMA_API_VERSION);
        close_library();
        return false;
    }
    game_ = game;
    loaded_write_time_ = write_time;
    ++load_count_;
    return true;
}

void GameModule::close_library() noexcept {
    game_ = nullptr;
    if (handle_ != nullptr) {
        close_shared_library(handle_);
        handle_ = nullptr;
    }
    if (!loaded_path_.empty()) {
        (void)platform::remove_file(loaded_path_.c_str());
        loaded_path_.clear();
    }
}

bool GameModule::load(tynima_engine& engine) {
    const bool reloaded = load_count_ > 0;
    if (!open_library()) {
        return false;
    }
    if (game_->load != nullptr) {
        game_->load(&api(), &engine, reloaded);
    }
    return true;
}

void GameModule::unload(tynima_engine& engine, bool reloading) {
    if (game_ != nullptr && game_->unload != nullptr) {
        game_->unload(&api(), &engine, reloading);
    }
    close_library();
}

bool GameModule::poll(tynima_engine& engine) {
    TY_PROFILE_SCOPE_NAMED("sdk::GameModule::poll");
    const std::uint64_t write_time = platform::file_write_time(path_.c_str());
    if (write_time == 0 || write_time == loaded_write_time_) {
        return false;
    }
    // The linker may still be writing: wait until the timestamp holds still
    // for a moment before trusting the file.
    const double now = platform::now_seconds();
    if (pending_write_time_ != write_time) {
        pending_write_time_ = write_time;
        pending_since_ = now;
        return false;
    }
    if (now - pending_since_ < 0.2) {
        return false;
    }
    pending_write_time_ = 0;

    const double start = platform::now_seconds();
    unload(engine, true);
    if (!load(engine)) {
        std::fprintf(stderr, "game    reload failed: %s\n", error_.c_str());
        return false;
    }
    std::printf("game    reloaded %s (load #%u) in %.0f ms\n", path_.c_str(), load_count_,
                (platform::now_seconds() - start) * 1000.0);
    return true;
}

void GameModule::update(tynima_engine& engine, float dt) noexcept {
    if (game_ != nullptr && game_->update != nullptr) {
        TY_PROFILE_SCOPE_NAMED("game::update");
        game_->update(&api(), &engine, dt);
    }
}

} // namespace tynima::sdk
