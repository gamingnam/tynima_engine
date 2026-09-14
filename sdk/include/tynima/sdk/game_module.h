#pragma once

#include <tynima.h>

#include <tynima/physics/physics.h>
#include <tynima/platform/input.h>
#include <tynima/scene/world.h>

#include <cstdint>
#include <string>

// What the engine hands a game module: filled in by the host each frame,
// read by the C API functions in api.cpp. The module only ever sees the
// opaque pointer. Log lines go to the engine's logger under "game".
struct tynima_engine {
    tynima::scene::World* world = nullptr;
    const tynima::platform::Input* input = nullptr;
    tynima::physics::PhysicsWorld* physics = nullptr; // optional
    double time_seconds = 0.0;
};

namespace tynima::sdk {

// The C API table, pointing at this engine's implementations.
[[nodiscard]] const tynima_api& api() noexcept;

// A hot-reloadable game module: a shared library exporting tynima_game_entry.
//
// The library is copied before it is loaded, so the compiler can overwrite
// the original while the copy runs; poll() notices the original changing
// and swaps in the new build — unload(reloading), load(reloaded) — while the
// world, the window and everything else in the engine stay exactly as they were.
class GameModule {
public:
    explicit GameModule(std::string path);
    ~GameModule();
    GameModule(const GameModule&) = delete;
    GameModule& operator=(const GameModule&) = delete;

    // Loads the library and calls its load(). False (with a message) on failure.
    [[nodiscard]] bool load(tynima_engine& engine);
    void unload(tynima_engine& engine, bool reloading = false);

    // Reloads when the file on disk has changed since the last load. Cheap:
    // one stat per call, so call it every frame. Returns true after a reload.
    bool poll(tynima_engine& engine);

    void update(tynima_engine& engine, float dt) noexcept;

    [[nodiscard]] bool loaded() const noexcept { return game_ != nullptr; }
    [[nodiscard]] std::uint32_t load_count() const noexcept { return load_count_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] const char* last_error() const noexcept { return error_.c_str(); }

private:
    bool open_library();
    void close_library() noexcept;

    std::string path_;         // the build output
    std::string loaded_path_;  // the copy actually mapped
    std::string error_;
    void* handle_ = nullptr;   // dlopen / LoadLibrary
    const tynima_game* game_ = nullptr;
    std::uint64_t loaded_write_time_ = 0;
    std::uint64_t pending_write_time_ = 0; // a change seen but not yet settled
    double pending_since_ = 0.0;
    std::uint32_t load_count_ = 0;
};

} // namespace tynima::sdk
