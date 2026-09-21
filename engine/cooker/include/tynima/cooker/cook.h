#pragma once

#include <tynima/assets/model_blob.h>
#include <tynima/core/jobs.h>
#include <tynima/render/model.h>

#include <string>

// The cooker: a source file in, a cooked blob out. For a model that is
// glTF import (gltf.h), the mesh put in the order a GPU reads it best
// (meshoptimizer: identical vertices welded, each submesh's triangles
// ordered for the post-transform cache and then against overdraw, the
// vertices themselves ordered for fetch), every image's mip chain built
// (mipmaps.h), and the whole written as assets/model_blob.h. Everything a
// loader would otherwise do on every start happens here once.
//
// cook_model_file() is what tynima-cook and the runtime's cook-on-load both
// call; cooked_path_for() and cooked_is_current() are the make-style rule
// they share — a blob is current when it is newer than its source and of
// this engine's version — so the two never disagree about what needs
// cooking.
namespace tynima::cooker {

struct CookOptions {
    core::JobSystem* jobs = nullptr; // images decode and build their mips in parallel when given
    bool optimize_mesh = true;       // meshoptimizer's ordering; off keeps the source's
    bool mipmaps = true;             // every level; off keeps level 0 alone
};

// Whether a path names a file the cooker can take: .gltf or .glb.
[[nodiscard]] bool is_model_source(const char* path) noexcept;
// Whether it names a cooked blob: .tymodel.
[[nodiscard]] bool is_cooked_model(const char* path) noexcept;

// Where `source` cooks to: `<cook_dir>/<name>.tymodel`, or with no cook_dir
// `<source's directory>/.cooked/<name>.tymodel`.
[[nodiscard]] std::string cooked_path_for(const char* source, const char* cook_dir = nullptr);
// True when `cooked` exists, was written after `source` last was, and is of
// this engine's blob version.
[[nodiscard]] bool cooked_is_current(const char* source, const char* cooked) noexcept;

// Import, order, mip and write. The directory of `destination` is made if
// it is missing; the file is written whole (a temporary renamed over it).
[[nodiscard]] bool cook_model_file(const char* source, const char* destination, const CookOptions& options,
                                   std::string& error);
// The middle of that, for a model already in memory (a test's, a
// generator's): `data` ordered and mipped into `out`.
void cook_model_data(const render::ModelData& data, const CookOptions& options, assets::CookedModel& out);

// cook_model_file() when cooked_is_current() is false, else nothing: what
// the runtime does for a source it is asked to load. `cooked` receives the
// blob's path either way; true when the blob is there to load afterwards.
[[nodiscard]] bool ensure_cooked(const char* source, const char* cook_dir, const CookOptions& options,
                                 std::string& cooked, std::string& error, bool* cooked_now = nullptr);

} // namespace tynima::cooker
