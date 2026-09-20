#pragma once

#include <tynima/scene/world.h>

#include <string>
#include <string_view>

// The scene as a text file, in TOML: one entity after another, each a
// table per component, each a line per field, in an order that never
// changes for the same world — what a version control system diffs and
// merges. Written from the fields reflection describes (core/reflect.h),
// so no component needs code of its own; a component with no description
// is saved as its name alone and loaded with its defaults.
//
//   # Tynima scene
//   version = 1
//
//   [[entities]]
//   [entities.Name]
//   text = "floor"
//   [entities.Transform]
//   position = [0.0, -0.25, 0.0]
//   rotation = [0.0, 0.0, 0.0, 1.0]
//   scale = [1.0, 1.0, 1.0]
//   [entities.MeshRenderer]
//   model = 1
//   visible = true
//
// Numbers are written with the fewest digits that read back to the same
// value, so a value saved and loaded is the same bits, and a file saved
// again is the same text. Entities are numbered by their position in the
// file, and an entity-valued field (Parent) names one by that number; the
// numbers are given again on the next save, so a reference is stable
// until an entity before it is removed. Fields that are read-only or
// hidden are derived or runtime state and are not saved; handles (a
// physics body) are not either, since they mean nothing in another run.
//
// The reader takes the subset of TOML the writer produces — tables, arrays
// of tables, one key per line, strings, numbers, booleans and arrays of
// them on one line, comments — and says which line it did not understand.
namespace tynima::scene {

inline constexpr std::uint32_t kSceneFileVersion = 1;

// Every live entity of `world`, as text.
[[nodiscard]] std::string save_scene_text(World& world);
[[nodiscard]] bool save_scene_file(World& world, const char* path, std::string& error);

struct LoadOptions {
    bool clear = true; // destroy every entity first; false adds the file's to what is there
};

// Creates the file's entities in `world`. Every component named in the file
// must be registered already (the module that defines it loaded first).
// The whole file is read and checked before the world is touched: false,
// with `error` naming the line, leaves the world as it was.
[[nodiscard]] bool load_scene_text(World& world, std::string_view text, std::string& error,
                                   const LoadOptions& options = {});
[[nodiscard]] bool load_scene_file(World& world, const char* path, std::string& error,
                                   const LoadOptions& options = {});

} // namespace tynima::scene
