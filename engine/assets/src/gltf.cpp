#include <tynima/assets/gltf.h>

#include <tynima/assets/image.h>
#include <tynima/core/profile.h>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace tynima::assets {

namespace {

const char* result_name(cgltf_result result) noexcept {
    switch (result) {
    case cgltf_result_success: return "success";
    case cgltf_result_data_too_short: return "data too short";
    case cgltf_result_unknown_format: return "unknown format";
    case cgltf_result_invalid_json: return "invalid JSON";
    case cgltf_result_invalid_gltf: return "invalid glTF";
    case cgltf_result_invalid_options: return "invalid options";
    case cgltf_result_file_not_found: return "file not found";
    case cgltf_result_io_error: return "I/O error";
    case cgltf_result_out_of_memory: return "out of memory";
    case cgltf_result_legacy_gltf: return "legacy glTF 1.0";
    default: return "unknown error";
    }
}

const cgltf_accessor* find_attribute(const cgltf_primitive& prim, cgltf_attribute_type type, int index = 0) noexcept {
    for (cgltf_size i = 0; i < prim.attributes_count; ++i) {
        if (prim.attributes[i].type == type && prim.attributes[i].index == index) {
            return prim.attributes[i].data;
        }
    }
    return nullptr;
}

math::Mat4 world_transform(const cgltf_node& node) noexcept {
    float m[16];
    cgltf_node_transform_world(&node, m); // column-major, like ours
    math::Mat4 out;
    std::memcpy(&out.cols[0].x, m, sizeof(m));
    return out;
}

std::int32_t image_index(const cgltf_data& data, const cgltf_texture_view& view) noexcept {
    if (view.texture == nullptr || view.texture->image == nullptr) {
        return -1;
    }
    return static_cast<std::int32_t>(view.texture->image - data.images);
}

// Bytes of one image, wherever the file keeps them: a buffer view (GLB), a
// data: URI, or a file next to the .gltf.
bool read_image_bytes(const cgltf_image& image, const char* gltf_path, std::vector<unsigned char>& bytes,
                      std::string& error) {
    if (image.buffer_view != nullptr) {
        const cgltf_buffer_view& view = *image.buffer_view;
        if (view.buffer == nullptr || view.buffer->data == nullptr) {
            error = "image buffer view has no data";
            return false;
        }
        const auto* begin = static_cast<const unsigned char*>(view.buffer->data) + view.offset;
        bytes.assign(begin, begin + view.size);
        return true;
    }
    if (image.uri == nullptr) {
        error = "image has neither a URI nor a buffer view";
        return false;
    }
    if (std::strncmp(image.uri, "data:", 5) == 0) {
        const char* comma = std::strchr(image.uri, ',');
        if (comma == nullptr || std::strstr(image.uri, ";base64") == nullptr) {
            error = "image data URI is not base64";
            return false;
        }
        const char* base64 = comma + 1;
        const std::size_t length = std::strlen(base64);
        std::size_t decoded = (length / 4) * 3;
        if (length >= 1 && base64[length - 1] == '=') --decoded;
        if (length >= 2 && base64[length - 2] == '=') --decoded;
        cgltf_options options{};
        void* raw = nullptr;
        if (cgltf_load_buffer_base64(&options, decoded, base64, &raw) != cgltf_result_success || raw == nullptr) {
            error = "image data URI failed to decode";
            return false;
        }
        bytes.assign(static_cast<unsigned char*>(raw), static_cast<unsigned char*>(raw) + decoded);
        std::free(raw); // cgltf's default allocator is malloc
        return true;
    }
    if (gltf_path == nullptr) {
        error = std::string("image '") + image.uri + "' is an external file, but the glTF came from memory";
        return false;
    }
    std::string path(gltf_path);
    const std::size_t slash = path.find_last_of("/\\");
    path = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1));
    std::string uri(image.uri);
    cgltf_decode_uri(uri.data());
    uri.resize(std::strlen(uri.c_str()));
    path += uri;
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        error = "cannot open image '" + path + "'";
        return false;
    }
    unsigned char chunk[65536];
    std::size_t read = 0;
    while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        bytes.insert(bytes.end(), chunk, chunk + read);
    }
    std::fclose(file);
    return true;
}

struct Importer {
    const cgltf_data& data;
    const char* gltf_path;
    render::ModelData& model;
    render::MeshData& out;
    std::string& error;
    std::vector<float> scratch;

    bool import_primitive(const cgltf_mesh& mesh, cgltf_size prim_index, const math::Mat4& world) {
        const cgltf_primitive& prim = mesh.primitives[prim_index];
        if (prim.type != cgltf_primitive_type_triangles) {
            return true; // points and lines are not meshes to us
        }
        const cgltf_accessor* positions = find_attribute(prim, cgltf_attribute_type_position);
        if (positions == nullptr || positions->type != cgltf_type_vec3) {
            error = "mesh '" + std::string(mesh.name != nullptr ? mesh.name : "") + "' primitive " +
                    std::to_string(prim_index) + " has no vec3 POSITION";
            return false;
        }
        const cgltf_accessor* normals = find_attribute(prim, cgltf_attribute_type_normal);
        const cgltf_accessor* uvs = find_attribute(prim, cgltf_attribute_type_texcoord, 0);
        const cgltf_accessor* tangents = find_attribute(prim, cgltf_attribute_type_tangent);
        if (tangents != nullptr && (tangents->type != cgltf_type_vec4 || tangents->count != positions->count)) {
            tangents = nullptr;
        }
        if (normals != nullptr && (normals->type != cgltf_type_vec3 || normals->count != positions->count)) {
            normals = nullptr; // malformed: recompute rather than fail
        }
        if (uvs != nullptr && (uvs->type != cgltf_type_vec2 || uvs->count != positions->count)) {
            uvs = nullptr;
        }

        const cgltf_size vertex_count = positions->count;
        const auto vertex_base = static_cast<std::uint32_t>(out.vertices.size());
        const auto first_index = static_cast<std::uint32_t>(out.indices.size());
        const math::Mat3 normal_matrix = math::transpose(math::inverse(world.upper3x3()));

        out.vertices.resize(out.vertices.size() + vertex_count);
        scratch.resize(vertex_count * 3);
        cgltf_accessor_unpack_floats(positions, scratch.data(), vertex_count * 3);
        for (cgltf_size i = 0; i < vertex_count; ++i) {
            const math::Vec3 p{scratch[i * 3], scratch[i * 3 + 1], scratch[i * 3 + 2]};
            out.vertices[vertex_base + i].position = math::transform_point(world, p);
        }
        if (normals != nullptr) {
            cgltf_accessor_unpack_floats(normals, scratch.data(), vertex_count * 3);
            for (cgltf_size i = 0; i < vertex_count; ++i) {
                const math::Vec3 n{scratch[i * 3], scratch[i * 3 + 1], scratch[i * 3 + 2]};
                out.vertices[vertex_base + i].normal = math::normalize(normal_matrix * n);
            }
        }
        if (uvs != nullptr) {
            scratch.resize(vertex_count * 2);
            cgltf_accessor_unpack_floats(uvs, scratch.data(), vertex_count * 2);
            for (cgltf_size i = 0; i < vertex_count; ++i) {
                out.vertices[vertex_base + i].uv = {scratch[i * 2], scratch[i * 2 + 1]};
            }
        } else {
            for (cgltf_size i = 0; i < vertex_count; ++i) {
                out.vertices[vertex_base + i].uv = math::Vec2::zero();
            }
        }

        if (prim.indices != nullptr) {
            const cgltf_size index_count = prim.indices->count;
            out.indices.reserve(out.indices.size() + index_count);
            for (cgltf_size i = 0; i < index_count; ++i) {
                const cgltf_size index = cgltf_accessor_read_index(prim.indices, i);
                if (index >= vertex_count) {
                    error = "mesh '" + std::string(mesh.name != nullptr ? mesh.name : "") + "' has an index out of range";
                    return false;
                }
                out.indices.push_back(vertex_base + static_cast<std::uint32_t>(index));
            }
        } else {
            for (cgltf_size i = 0; i < vertex_count; ++i) {
                out.indices.push_back(vertex_base + static_cast<std::uint32_t>(i));
            }
        }
        const auto index_count = static_cast<std::uint32_t>(out.indices.size()) - first_index;
        if (normals == nullptr) {
            out.compute_normals(first_index, index_count);
        }
        if (tangents != nullptr) {
            // Directions transform with the model matrix itself; only normals need the inverse transpose.
            const math::Mat3 direction_matrix = world.upper3x3();
            scratch.resize(vertex_count * 4);
            cgltf_accessor_unpack_floats(tangents, scratch.data(), vertex_count * 4);
            for (cgltf_size i = 0; i < vertex_count; ++i) {
                const math::Vec3 t{scratch[i * 4], scratch[i * 4 + 1], scratch[i * 4 + 2]};
                const float w = scratch[i * 4 + 3] < 0.0f ? -1.0f : 1.0f;
                out.vertices[vertex_base + i].tangent = {math::normalize(direction_matrix * t), w};
            }
        } else {
            out.compute_tangents(first_index, index_count);
        }

        render::Submesh sub;
        sub.first_index = first_index;
        sub.index_count = index_count;
        sub.material = prim.material != nullptr ? static_cast<std::uint32_t>(prim.material - data.materials) : 0;
        out.submeshes.push_back(sub);
        return true;
    }

    bool import_node(const cgltf_node& node) {
        if (node.mesh != nullptr) {
            const math::Mat4 world = world_transform(node);
            for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
                if (!import_primitive(*node.mesh, p, world)) {
                    return false;
                }
            }
        }
        for (cgltf_size c = 0; c < node.children_count; ++c) {
            if (!import_node(*node.children[c])) {
                return false;
            }
        }
        return true;
    }

    void import_materials() {
        model.materials.reserve(data.materials_count);
        for (cgltf_size i = 0; i < data.materials_count; ++i) {
            const cgltf_material& m = data.materials[i];
            render::MaterialData material;
            if (m.has_pbr_metallic_roughness) {
                const cgltf_pbr_metallic_roughness& pbr = m.pbr_metallic_roughness;
                material.base_color_factor = {pbr.base_color_factor[0], pbr.base_color_factor[1],
                                              pbr.base_color_factor[2], pbr.base_color_factor[3]};
                material.base_color_image = image_index(data, pbr.base_color_texture);
                material.metallic_factor = pbr.metallic_factor;
                material.roughness_factor = pbr.roughness_factor;
                material.metallic_roughness_image = image_index(data, pbr.metallic_roughness_texture);
            }
            material.normal_image = image_index(data, m.normal_texture);
            material.normal_scale = m.normal_texture.scale;
            material.occlusion_image = image_index(data, m.occlusion_texture);
            material.occlusion_strength = m.occlusion_texture.scale; // cgltf keeps strength in `scale`
            material.emissive_factor = {m.emissive_factor[0], m.emissive_factor[1], m.emissive_factor[2]};
            material.emissive_image = image_index(data, m.emissive_texture);
            material.double_sided = m.double_sided;
            model.materials.push_back(material);
        }
    }

    // Decodes every image; a bad image is a warning (empty pixels), not a failure.
    void import_images() {
        model.images.resize(data.images_count);
        for (const render::MaterialData& m : model.materials) {
            for (const std::int32_t index : {m.base_color_image, m.emissive_image}) {
                if (index >= 0 && static_cast<cgltf_size>(index) < data.images_count) {
                    model.images[static_cast<std::size_t>(index)].srgb = true;
                }
            }
        }
        std::vector<unsigned char> bytes;
        for (cgltf_size i = 0; i < data.images_count; ++i) {
            TY_PROFILE_SCOPE_NAMED("decode image");
            bytes.clear();
            std::string image_error;
            if (!read_image_bytes(data.images[i], gltf_path, bytes, image_error) ||
                !decode_image(bytes.data(), bytes.size(), model.images[i], image_error)) {
                std::fprintf(stderr, "gltf: image %zu: %s\n", static_cast<std::size_t>(i), image_error.c_str());
                model.images[i].pixels.clear();
            }
        }
    }

    bool run() {
        model = render::ModelData{};
        if (data.scene != nullptr) {
            for (cgltf_size n = 0; n < data.scene->nodes_count; ++n) {
                if (!import_node(*data.scene->nodes[n])) {
                    return false;
                }
            }
        } else {
            // No default scene: take every root node.
            for (cgltf_size n = 0; n < data.nodes_count; ++n) {
                if (data.nodes[n].parent == nullptr && !import_node(data.nodes[n])) {
                    return false;
                }
            }
        }
        if (out.vertices.empty()) {
            error = "no triangle geometry found";
            return false;
        }
        out.compute_bounds();
        import_materials();
        import_images();
        return true;
    }
};

bool import_parsed(cgltf_data* data, const char* path, render::ModelData& out, std::string& error) {
    cgltf_options options{};
    const cgltf_result loaded = cgltf_load_buffers(&options, data, path);
    if (loaded != cgltf_result_success) {
        error = std::string("loading glTF buffers failed: ") + result_name(loaded);
        cgltf_free(data);
        return false;
    }
    Importer importer{*data, path, out, out.mesh, error, {}};
    const bool ok = importer.run();
    cgltf_free(data);
    return ok;
}

} // namespace

bool import_gltf_file(const char* path, render::ModelData& out, std::string& error) {
    TY_PROFILE_SCOPE_NAMED("assets::import_gltf_file");
    cgltf_options options{};
    cgltf_data* data = nullptr;
    const cgltf_result parsed = cgltf_parse_file(&options, path, &data);
    if (parsed != cgltf_result_success) {
        error = std::string("parsing '") + path + "' failed: " + result_name(parsed);
        return false;
    }
    return import_parsed(data, path, out, error);
}

bool import_gltf_memory(const void* bytes, std::size_t size, render::ModelData& out, std::string& error) {
    TY_PROFILE_SCOPE_NAMED("assets::import_gltf_memory");
    cgltf_options options{};
    cgltf_data* data = nullptr;
    const cgltf_result parsed = cgltf_parse(&options, bytes, size, &data);
    if (parsed != cgltf_result_success) {
        error = std::string("parsing glTF failed: ") + result_name(parsed);
        return false;
    }
    return import_parsed(data, nullptr, out, error);
}

} // namespace tynima::assets
