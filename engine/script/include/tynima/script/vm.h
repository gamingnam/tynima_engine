#pragma once

#include <memory>
#include <string>

// The VM a game's scripts run in: LuaJIT, hosting Lua that talks to the
// engine through the same tynima_api table a C game module gets. Nothing is
// bound by hand — LuaJIT's FFI calls C directly from declarations generated
// out of tynima.h (tools/gen_lua.py, engine/script/lua/tynima_ffi.lua), and
// the components come from reflection while the game runs, so a script can
// read a component a module defined in C++ after the script was written.
//
// This layer knows none of that. It opens a VM, puts two pointers in front
// of the script — what the engine calls its api table and its context — and
// runs the file. What those pointers mean is tynima.lua's business, which
// is what keeps the VM underneath the public API rather than beside it.
//
// A script is a table of the same three moments a game module has:
//
//     local ty = require("tynima")
//     local game = {}
//     function game.load(reloaded) ... end
//     function game.update(dt) ... end
//     function game.unload(reloading) ... end
//     return game
//
// and, like a module, it is reloaded when its file changes — but in a
// quarter of a second, with nothing to compile.
namespace tynima::script {

struct VmDesc {
    // Where tynima.lua and tynima_ffi.lua are; also the first place a
    // script's own require() looks. Null: the directory the engine was
    // built with.
    const char* lua_path = nullptr;
    // The engine, as the script will see it: a tynima_api* and the
    // tynima_engine* to pass back to it. Opaque here on purpose.
    const void* api = nullptr;
    void* engine = nullptr;
    // How often the script's file is looked at for a change, in seconds;
    // 0 at every poll(). A script loaded from source has no file and is
    // never reloaded whatever this says.
    float poll_seconds = 0.25f;
};

class Vm {
public:
    // Null when this build has no VM in it (TYNIMA_LUA off) or the VM
    // cannot start; last_error() of the class says which.
    [[nodiscard]] static std::unique_ptr<Vm> create(const VmDesc& desc);
    // Whether this build has a VM at all.
    [[nodiscard]] static bool available() noexcept;
    // Why the last create() or load() failed. Never null.
    [[nodiscard]] static const char* last_create_error() noexcept;

    virtual ~Vm() = default;
    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;

    // Runs `path` and keeps the table it returns. The script's load() is
    // called with `reloaded` false. False with last_error() saying why —
    // a syntax error, a script that returns nothing, a script that threw.
    [[nodiscard]] virtual bool load(const char* path) = 0;
    // Runs `source` under `name` instead of a file: for tests and for a
    // console. Not watched, since there is no file to watch.
    [[nodiscard]] virtual bool load_source(const char* source, const char* name) = 0;
    virtual void unload(bool reloading = false) = 0;
    [[nodiscard]] virtual bool loaded() const noexcept = 0;
    [[nodiscard]] virtual const char* path() const noexcept = 0;

    // The script's update(dt). A script that throws is reported once and
    // then left alone until it is loaded again, so a broken frame does not
    // fill the log sixty times a second.
    virtual void update(float dt) = 0;
    // Reloads when the file has changed and settled. True after a reload.
    virtual bool poll() = 0;

    // Anything the script left behind: the last error, or "".
    [[nodiscard]] virtual const char* last_error() const noexcept = 0;
    // How many times the script has been loaded, the first time included.
    [[nodiscard]] virtual std::uint32_t load_count() const noexcept = 0;

    // Evaluates `source` and returns what it printed or returned, for a
    // console in a tool. False when it threw, with the error in `out`.
    [[nodiscard]] virtual bool eval(const char* source, std::string& out) = 0;

protected:
    Vm() = default;
};

} // namespace tynima::script
