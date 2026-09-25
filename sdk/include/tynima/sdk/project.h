#pragma once

#include <cstdint>
#include <string>

// What a game is, as a file the tools read: tynima.toml at the root of a
// project. The `tynima` command reads it to know what to run, what to cook
// and where; a game that outgrows the command can still be built by hand,
// since everything in here is a path and a number.
//
//     # Tynima project
//     [project]
//     name = "mygame"
//
//     [game]
//     script = "src/game.lua"   # the game, in Lua
//     module = ""               # or a native one, built beside it
//     title  = "My Game"
//     width  = 1280
//     height = 720
//     physics = "tynima"        # or "jolt"
//
//     [assets]
//     source = "assets"         # models to cook
//     cooked = "cooked"         # where the blobs go
//
// The reader takes the same subset of TOML the scene format does — sections,
// one key a line, strings, numbers and booleans, comments — and says which
// line it did not understand. Every path is relative to the file, and
// resolved against it: a project can be moved or built from anywhere.
namespace tynima::sdk {

inline constexpr const char* kProjectFileName = "tynima.toml";

struct Project {
    std::string name = "game";
    // [game]
    std::string script; // a Lua file, relative to the project
    std::string module; // a native game module (a shared library)
    std::string title;  // the window's; empty: the project's name
    int width = 1280;   // points
    int height = 720;
    std::string physics = "tynima"; // or "jolt"
    // [assets]
    std::string asset_dir = "assets";
    std::string cooked_dir = "cooked";

    // Where the file was read from, and the directory holding it. Both are
    // set by load(); a Project made by hand has neither.
    std::string path;
    std::string root;

    // `relative` against the project's root, as an absolute path. An empty
    // string stays empty; an absolute path is returned as it is.
    [[nodiscard]] std::string resolve(const std::string& relative) const;
    [[nodiscard]] std::string script_path() const { return resolve(script); }
    [[nodiscard]] std::string module_path() const { return resolve(module); }
    [[nodiscard]] std::string asset_path() const { return resolve(asset_dir); }
    [[nodiscard]] std::string cooked_path() const { return resolve(cooked_dir); }
    [[nodiscard]] const std::string& window_title() const { return title.empty() ? name : title; }
};

// Reads a project file. `path` may be the file itself or a directory holding
// one. False with `error` naming the line, or saying nothing was found.
[[nodiscard]] bool load_project(const char* path, Project& out, std::string& error);
// Searches `start` and every directory above it for a project file: what the
// command does when it is run somewhere inside a project.
[[nodiscard]] bool find_project(const char* start, Project& out, std::string& error);
[[nodiscard]] std::string write_project_text(const Project& project);
[[nodiscard]] bool save_project(const Project& project, const char* path, std::string& error);

} // namespace tynima::sdk
