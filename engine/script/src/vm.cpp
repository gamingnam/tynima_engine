#include <tynima/assets/file_watch.h>
#include <tynima/core/log.h>
#include <tynima/core/profile.h>
#include <tynima/platform/file.h>
#include <tynima/platform/time.h>
#include <tynima/script/vm.h>

#include <cstdint>
#include <string>

#if defined(TYNIMA_LUA)
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}
#endif

namespace tynima::script {

namespace {
std::string g_create_error;
}

bool Vm::available() noexcept {
#if defined(TYNIMA_LUA)
    return true;
#else
    return false;
#endif
}

const char* Vm::last_create_error() noexcept {
    return g_create_error.c_str();
}

#if !defined(TYNIMA_LUA)

std::unique_ptr<Vm> Vm::create(const VmDesc&) {
    g_create_error = "this engine was built without a script VM (TYNIMA_LUA=OFF)";
    return nullptr;
}

#else

namespace {

#ifndef TYNIMA_LUA_DIR
#define TYNIMA_LUA_DIR "."
#endif

// What the script's print() and its errors become: the engine's log, in the
// "lua" category, so a script's output lands in the same console as
// everything else and a game module's.
// A value as Lua itself would show it. lua_tostring alone answers nothing
// for a boolean, a nil or a table, which is most of what a script returns.
std::string to_display(lua_State* lua, int index) {
    lua_getglobal(lua, "tostring");
    lua_pushvalue(lua, index);
    if (lua_pcall(lua, 1, 1, 0) != 0) {
        lua_pop(lua, 1);
        return luaL_typename(lua, index);
    }
    const char* text = lua_tostring(lua, -1);
    std::string out = text != nullptr ? text : "?";
    lua_pop(lua, 1);
    return out;
}

int lua_print(lua_State* lua) {
    std::string line;
    const int count = lua_gettop(lua);
    for (int i = 1; i <= count; ++i) {
        line += to_display(lua, i);
        if (i < count) {
            line += '\t';
        }
    }
    TY_LOG_INFO("lua", "%s", line.c_str());
    return 0;
}

// A traceback around whatever threw, so an error says which line of which
// script, not merely what.
int lua_traceback(lua_State* lua) {
    const char* message = lua_tostring(lua, 1);
    if (message == nullptr) {
        message = "(a script threw something that is not a message)";
    }
    luaL_traceback(lua, lua, message, 1);
    return 1;
}

class LuaVm final : public Vm {
public:
    explicit LuaVm(const VmDesc& desc) : desc_(desc), watch_(static_cast<double>(desc.poll_seconds)) {
        lua_path_ = desc.lua_path != nullptr && desc.lua_path[0] != '\0' ? desc.lua_path : TYNIMA_LUA_DIR;
    }

    ~LuaVm() override {
        if (lua_ != nullptr) {
            unload(false);
            lua_close(lua_);
        }
    }

    [[nodiscard]] bool open() {
        lua_ = luaL_newstate();
        if (lua_ == nullptr) {
            g_create_error = "LuaJIT could not make a state (out of memory)";
            return false;
        }
        luaL_openlibs(lua_);
        lua_pushcfunction(lua_, lua_print);
        lua_setglobal(lua_, "print");
        // Where require() looks: the engine's own Lua first, then a script's
        // own directory, then whatever LUA_PATH already said.
        lua_getglobal(lua_, "package");
        lua_getfield(lua_, -1, "path");
        const char* existing = lua_tostring(lua_, -1);
        const std::string path =
            lua_path_ + "/?.lua;" + lua_path_ + "/?/init.lua;" + (existing != nullptr ? existing : "");
        lua_pop(lua_, 1);
        lua_pushlstring(lua_, path.c_str(), path.size());
        lua_setfield(lua_, -2, "path");
        lua_pop(lua_, 1);
        // The engine, as the script sees it. tynima.lua turns the two
        // pointers into the world, the input and the rest.
        if (!run("local ty = require('tynima'); ty.bind(...); return true", "=[tynima bind]", 2)) {
            g_create_error = "the engine's Lua could not be loaded: " + error_;
            return false;
        }
        TY_LOG_DEBUG("lua", "%s", lua_version_text().c_str());
        return true;
    }

    [[nodiscard]] bool load(const char* path) override {
        TY_PROFILE_SCOPE_NAMED("script::Vm::load");
        if (path == nullptr || path[0] == '\0') {
            error_ = "no script to load";
            return false;
        }
        core::Bytes source;
        if (!platform::read_file(path, source)) {
            error_ = std::string("cannot read '") + path + "'";
            return false;
        }
        script_path_ = path;
        if (watched_ == assets::FileWatch::kNone) {
            watched_ = watch_.watch(path);
        }
        const std::string chunk_name = "@" + script_path_; // @: a file name, for the traceback
        return load_text(reinterpret_cast<const char*>(source.data()), source.size(), chunk_name.c_str());
    }

    [[nodiscard]] bool load_source(const char* source, const char* name) override {
        if (source == nullptr) {
            error_ = "no source to load";
            return false;
        }
        script_path_.clear();
        const std::string chunk_name = std::string("=") + (name != nullptr ? name : "[script]");
        return load_text(source, std::char_traits<char>::length(source), chunk_name.c_str());
    }

    void unload(bool reloading) override {
        if (!loaded_) {
            return;
        }
        (void)call_hook("unload", reloading);
        luaL_unref(lua_, LUA_REGISTRYINDEX, game_ref_);
        game_ref_ = LUA_NOREF;
        loaded_ = false;
    }

    [[nodiscard]] bool loaded() const noexcept override { return loaded_; }
    [[nodiscard]] const char* path() const noexcept override { return script_path_.c_str(); }
    [[nodiscard]] const char* last_error() const noexcept override { return error_.c_str(); }
    [[nodiscard]] std::uint32_t load_count() const noexcept override { return load_count_; }

    void update(float dt) override {
        TY_PROFILE_SCOPE_NAMED("script::Vm::update");
        if (!loaded_ || failed_) {
            return;
        }
        if (!begin_hook("update")) {
            return;
        }
        lua_pushnumber(lua_, static_cast<lua_Number>(dt));
        if (!finish_hook(1)) {
            // A script that throws every frame would fill the log; it is
            // said once, and the script is left alone until it is loaded again.
            TY_LOG_ERROR("lua", "update: %s", error_.c_str());
            TY_LOG_WARN("lua", "%s is stopped until it changes", script_path_.c_str());
            failed_ = true;
        }
    }

    bool poll() override {
        if (script_path_.empty() || watched_ == assets::FileWatch::kNone) {
            return false;
        }
        assets::FileWatch::Id changed[1];
        if (watch_.poll(platform::now_seconds(), changed, 1) == 0) {
            return false;
        }
        TY_LOG_INFO("lua", "%s changed: loading it again", script_path_.c_str());
        unload(true);
        failed_ = false;
        if (!load(script_path_.c_str())) {
            TY_LOG_ERROR("lua", "%s", error_.c_str());
            return false;
        }
        return true;
    }

    [[nodiscard]] bool eval(const char* source, std::string& out) override {
        out.clear();
        if (source == nullptr) {
            return false;
        }
        const int base = lua_gettop(lua_);
        lua_pushcfunction(lua_, lua_traceback);
        if (luaL_loadbuffer(lua_, source, std::char_traits<char>::length(source), "=[eval]") != 0 ||
            lua_pcall(lua_, 0, LUA_MULTRET, base + 1) != 0) {
            const char* message = lua_tostring(lua_, -1);
            out = message != nullptr ? message : "?";
            error_ = out;
            lua_settop(lua_, base);
            return false;
        }
        const int top = lua_gettop(lua_);
        for (int i = base + 2; i <= top; ++i) {
            if (i > base + 2) {
                out += '\t';
            }
            out += to_display(lua_, i);
        }
        lua_settop(lua_, base);
        return true;
    }

private:
    [[nodiscard]] std::string lua_version_text() {
        std::string out;
        (void)eval("return jit.version .. ' on ' .. jit.arch .. ', jit ' .. (jit.status() and 'on' or 'off')",
                   out);
        return out;
    }

    // Runs a chunk that takes the api and the engine as its arguments.
    [[nodiscard]] bool run(const char* source, const char* name, int arguments) {
        const int base = lua_gettop(lua_);
        lua_pushcfunction(lua_, lua_traceback);
        if (luaL_loadbuffer(lua_, source, std::char_traits<char>::length(source), name) != 0) {
            return fail(base);
        }
        if (arguments == 2) {
            lua_pushlightuserdata(lua_, const_cast<void*>(desc_.api));
            lua_pushlightuserdata(lua_, desc_.engine);
        }
        if (lua_pcall(lua_, arguments, 1, base + 1) != 0) {
            return fail(base);
        }
        lua_settop(lua_, base);
        return true;
    }

    [[nodiscard]] bool load_text(const char* source, std::size_t size, const char* chunk_name) {
        unload(true);
        failed_ = false;
        const int base = lua_gettop(lua_);
        lua_pushcfunction(lua_, lua_traceback);
        if (luaL_loadbuffer(lua_, source, size, chunk_name) != 0 || lua_pcall(lua_, 0, 1, base + 1) != 0) {
            return fail(base);
        }
        if (!lua_istable(lua_, -1)) {
            error_ = std::string(chunk_name + 1) +
                     " must return a table of what to call: { load = ..., update = ..., unload = ... }";
            lua_settop(lua_, base);
            return false;
        }
        game_ref_ = luaL_ref(lua_, LUA_REGISTRYINDEX);
        lua_settop(lua_, base);
        loaded_ = true;
        ++load_count_;
        error_.clear();
        if (!call_hook("load", load_count_ > 1)) {
            TY_LOG_ERROR("lua", "load: %s", error_.c_str());
            failed_ = true;
            return false;
        }
        return true;
    }

    // Pushes the traceback handler and the named function; false when the
    // script has no such function, which is allowed for all three.
    [[nodiscard]] bool begin_hook(const char* name) {
        hook_base_ = lua_gettop(lua_);
        lua_pushcfunction(lua_, lua_traceback);
        lua_rawgeti(lua_, LUA_REGISTRYINDEX, game_ref_);
        lua_getfield(lua_, -1, name);
        if (!lua_isfunction(lua_, -1)) {
            lua_settop(lua_, hook_base_);
            return false;
        }
        lua_remove(lua_, -2); // the game table; the function has what it needs
        return true;
    }

    [[nodiscard]] bool finish_hook(int arguments) {
        if (lua_pcall(lua_, arguments, 0, hook_base_ + 1) != 0) {
            const char* message = lua_tostring(lua_, -1);
            error_ = message != nullptr ? message : "?";
            lua_settop(lua_, hook_base_);
            return false;
        }
        lua_settop(lua_, hook_base_);
        return true;
    }

    [[nodiscard]] bool call_hook(const char* name, bool flag) {
        if (!begin_hook(name)) {
            return true; // a script need not want every moment
        }
        lua_pushboolean(lua_, flag ? 1 : 0);
        return finish_hook(1);
    }

    [[nodiscard]] bool fail(int base) {
        const char* message = lua_tostring(lua_, -1);
        error_ = message != nullptr ? message : "?";
        lua_settop(lua_, base);
        return false;
    }

    VmDesc desc_;
    assets::FileWatch watch_;
    assets::FileWatch::Id watched_ = assets::FileWatch::kNone;
    lua_State* lua_ = nullptr;
    std::string lua_path_;
    std::string script_path_;
    std::string error_;
    int game_ref_ = LUA_NOREF;
    int hook_base_ = 0;
    std::uint32_t load_count_ = 0;
    bool loaded_ = false;
    bool failed_ = false;
};

} // namespace

std::unique_ptr<Vm> Vm::create(const VmDesc& desc) {
    TY_PROFILE_SCOPE_NAMED("script::Vm::create");
    g_create_error.clear();
    auto vm = std::make_unique<LuaVm>(desc);
    if (!vm->open()) {
        return nullptr;
    }
    return vm;
}

#endif // TYNIMA_LUA

} // namespace tynima::script
