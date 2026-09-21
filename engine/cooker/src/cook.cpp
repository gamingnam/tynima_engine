#include <tynima/cooker/cook.h>

#include <tynima/cooker/gltf.h>
#include <tynima/cooker/mipmaps.h>
#include <tynima/core/log.h>
#include <tynima/core/profile.h>
#include <tynima/platform/file.h>

#include <cstring>
#include <initializer_list>
#include <meshoptimizer.h>
#include <vector>

namespace tynima::cooker {

namespace {

bool ends_with_nocase(const char* path, const char* suffix) noexcept {
    if (path == nullptr) {
        return false;
    }
    const std::size_t length = std::strlen(path);
    const std::size_t suffix_length = std::strlen(suffix);
    if (length < suffix_length) {
        return false;
    }
    for (std::size_t i = 0; i < suffix_length; ++i) {
        char a = path[length - suffix_length + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

// "dir/name.ext" -> dir = "dir/" (may be empty), name = "name"
void split_path(const char* path, std::string& dir, std::string& name) {
    const std::string full(path != nullptr ? path : "");
    const std::size_t slash = full.find_last_of("/\\");
    dir = slash == std::string::npos ? std::string() : full.substr(0, slash + 1);
    name = slash == std::string::npos ? full : full.substr(slash + 1);
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        name.erase(dot);
    }
}

// meshoptimizer over one vertex stream: weld, then per submesh the vertex
// cache and overdraw orderings, then the fetch ordering over everything.
void optimize_mesh(render::MeshData& mesh) {
    TY_PROFILE_SCOPE_NAMED("cooker::optimize_mesh");
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        return;
    }
    const std::size_t vertex_size = sizeof(render::Vertex);
    // Weld: two vertices with the same 48 bytes become one.
    std::vector<unsigned int> remap(mesh.vertices.size());
    const std::size_t unique =
        meshopt_generateVertexRemap(remap.data(), mesh.indices.data(), mesh.indices.size(),
                                    mesh.vertices.data(), mesh.vertices.size(), vertex_size);
    meshopt_remapIndexBuffer(mesh.indices.data(), mesh.indices.data(), mesh.indices.size(), remap.data());
    meshopt_remapVertexBuffer(mesh.vertices.data(), mesh.vertices.data(), mesh.vertices.size(), vertex_size,
                              remap.data());
    mesh.vertices.resize(unique);

    for (const render::Submesh& sub : mesh.submeshes) {
        unsigned int* indices = mesh.indices.data() + sub.first_index;
        meshopt_optimizeVertexCache(indices, indices, sub.index_count, mesh.vertices.size());
        meshopt_optimizeOverdraw(indices, indices, sub.index_count, &mesh.vertices[0].position.x,
                                 mesh.vertices.size(), vertex_size, 1.05f);
    }
    const std::size_t fetched =
        meshopt_optimizeVertexFetch(mesh.vertices.data(), mesh.indices.data(), mesh.indices.size(),
                                    mesh.vertices.data(), mesh.vertices.size(), vertex_size);
    mesh.vertices.resize(fetched); // a vertex no triangle used is gone
    mesh.compute_bounds();
}

} // namespace

bool is_model_source(const char* path) noexcept {
    return ends_with_nocase(path, ".gltf") || ends_with_nocase(path, ".glb");
}

bool is_cooked_model(const char* path) noexcept {
    return ends_with_nocase(path, assets::kModelBlobExtension);
}

std::string cooked_path_for(const char* source, const char* cook_dir) {
    std::string dir, name;
    split_path(source, dir, name);
    std::string out;
    if (cook_dir != nullptr && cook_dir[0] != '\0') {
        out = cook_dir;
        if (out.back() != '/' && out.back() != '\\') {
            out += '/';
        }
    } else {
        out = dir + ".cooked/";
    }
    return out + name + assets::kModelBlobExtension;
}

bool cooked_is_current(const char* source, const char* cooked) noexcept {
    const std::uint64_t source_time = platform::file_write_time(source);
    const std::uint64_t cooked_time = platform::file_write_time(cooked);
    return source_time != 0 && cooked_time > source_time &&
           assets::model_blob_version(cooked) == assets::kModelBlobVersion;
}

void cook_model_data(const render::ModelData& data, const CookOptions& options, assets::CookedModel& out) {
    TY_PROFILE_SCOPE_NAMED("cooker::cook_model_data");
    out = assets::CookedModel{};
    out.mesh = data.mesh;
    out.materials = data.materials;
    if (options.optimize_mesh) {
        optimize_mesh(out.mesh);
    }
    out.images.resize(data.images.size());
    auto build_one = [&](std::size_t i) { build_mip_chain(data.images[i], options.mipmaps, out.images[i]); };
    if (options.jobs != nullptr && data.images.size() > 1) {
        options.jobs->parallel_for(static_cast<std::uint32_t>(data.images.size()), 1,
                                   [&](std::uint32_t begin, std::uint32_t end) {
                                       for (std::uint32_t i = begin; i < end; ++i) {
                                           build_one(i);
                                       }
                                   });
    } else {
        for (std::size_t i = 0; i < data.images.size(); ++i) {
            build_one(i);
        }
    }
    // An image that failed to decode has no pixels. The blob holds no such
    // image: it is dropped, the rest renumbered, and the materials that used
    // it fall back, as they would have at load.
    std::vector<std::int32_t> renumbered(out.images.size(), -1);
    std::vector<assets::CookedImage> kept;
    for (std::size_t i = 0; i < out.images.size(); ++i) {
        if (!out.images[i].levels.empty()) {
            renumbered[i] = static_cast<std::int32_t>(kept.size());
            kept.push_back(std::move(out.images[i]));
        }
    }
    for (render::MaterialData& m : out.materials) {
        for (std::int32_t* slot : {&m.base_color_image, &m.metallic_roughness_image, &m.normal_image,
                                   &m.occlusion_image, &m.emissive_image}) {
            *slot =
                *slot >= 0 && static_cast<std::size_t>(*slot) < renumbered.size() ? renumbered[*slot] : -1;
        }
    }
    out.images = std::move(kept);
}

bool cook_model_file(const char* source, const char* destination, const CookOptions& options,
                     std::string& error) {
    TY_PROFILE_SCOPE_NAMED("cooker::cook_model_file");
    if (!is_model_source(source)) {
        error = std::string("'") + (source != nullptr ? source : "(null)") +
                "' is not a model the cooker reads (.gltf, .glb)";
        return false;
    }
    render::ModelData data;
    if (!import_gltf_file(source, data, error, {.jobs = options.jobs})) {
        return false;
    }
    assets::CookedModel cooked;
    cook_model_data(data, options, cooked);
    std::string dir, name;
    split_path(destination, dir, name);
    if (!dir.empty() && !platform::make_directories(dir.c_str())) {
        error = "cannot make the directory '" + dir + "'";
        return false;
    }
    return assets::save_model_blob(cooked, destination, error);
}

bool ensure_cooked(const char* source, const char* cook_dir, const CookOptions& options, std::string& cooked,
                   std::string& error, bool* cooked_now) {
    cooked = cooked_path_for(source, cook_dir);
    if (cooked_now != nullptr) {
        *cooked_now = false;
    }
    if (cooked_is_current(source, cooked.c_str())) {
        return true;
    }
    if (platform::file_write_time(source) == 0) {
        error = std::string("cannot read '") + source + "'";
        return false;
    }
    if (cooked_now != nullptr) {
        *cooked_now = true;
    }
    return cook_model_file(source, cooked.c_str(), options, error);
}

} // namespace tynima::cooker
