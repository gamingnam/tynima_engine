#pragma once

#include <tynima/core/jobs.h>
#include <tynima/render/model.h>

#include <cstddef>
#include <string>

// glTF 2.0 import (.gltf and .glb) into render::ModelData. Every mesh
// primitive in the default scene is baked into one vertex/index array with
// node transforms applied, one Submesh per primitive. Only triangle
// primitives are imported; missing normals are computed, missing UVs are
// zero. Materials come through as metallic-roughness parameters, and every
// image the file references is decoded to RGBA8, tagged sRGB when a base
// color or emissive slot uses it. Phase 5 moves this into the offline
// cooker — a shipping build never parses glTF.
namespace tynima::assets {

struct ImportOptions {
    // Decodes images in parallel when given; a file with several large
    // textures loads several times faster.
    core::JobSystem* jobs = nullptr;
};

// External buffers and images resolve relative to `path`.
[[nodiscard]] bool import_gltf_file(const char* path, render::ModelData& out, std::string& error,
                                    const ImportOptions& options = {});

// Only embedded buffers (GLB binary chunk, data: URIs) can be resolved.
[[nodiscard]] bool import_gltf_memory(const void* data, std::size_t size, render::ModelData& out, std::string& error,
                                      const ImportOptions& options = {});

} // namespace tynima::assets
