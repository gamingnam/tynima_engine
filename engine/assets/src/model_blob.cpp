#include <tynima/assets/model_blob.h>

#include <tynima/core/log.h>
#include <tynima/core/profile.h>
#include <tynima/platform/file.h>
#include <tynima/platform/platform.h>

#include <cstdio>
#include <cstring>

namespace tynima::assets {

namespace {

// ---- the file's records: plain data, laid out by hand, never padded by the compiler

constexpr char kMagic[4] = {'T', 'Y', 'M', 'D'};
constexpr std::size_t kAlignment = 16;

struct Header {
    char magic[4];
    std::uint32_t version;
    std::uint32_t vertex_count;
    std::uint32_t index_count;
    std::uint32_t submesh_count;
    std::uint32_t material_count;
    std::uint32_t image_count;
    std::uint32_t flags; // none yet: room for a compressed-texture format, a different vertex layout
    float bounds_min[3];
    float bounds_max[3];
    std::uint64_t file_size; // the whole file: a truncated one is caught before any offset is trusted
};
static_assert(sizeof(Header) == 64);

struct MaterialRecord {
    float base_color_factor[4];
    float metallic_factor;
    float roughness_factor;
    float normal_scale;
    float occlusion_strength;
    float emissive_factor[3];
    std::int32_t base_color_image;
    std::int32_t metallic_roughness_image;
    std::int32_t normal_image;
    std::int32_t occlusion_image;
    std::int32_t emissive_image;
    std::uint32_t flags; // bit 0: double sided
};
static_assert(sizeof(MaterialRecord) == 68);

enum ImageFormat : std::uint32_t { kRgba8Unorm = 0, kRgba8Srgb = 1 };

struct ImageRecord {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t mip_count;
    std::uint32_t format;
    std::uint64_t offset; // into the pixel section
    std::uint64_t size;   // every level
};
static_assert(sizeof(ImageRecord) == 32);

static_assert(sizeof(render::Vertex) == 48);
static_assert(sizeof(render::Submesh) == 12);

[[nodiscard]] constexpr std::size_t align_up(std::size_t value) noexcept {
    return (value + kAlignment - 1) & ~(kAlignment - 1);
}

// Where each section starts, from the counts alone; `end` is the file's size.
struct Layout {
    std::size_t vertices, indices, submeshes, materials, images, pixels, end;
};

[[nodiscard]] Layout layout_for(const Header& header, std::uint64_t pixel_bytes) noexcept {
    Layout l{};
    l.vertices = align_up(sizeof(Header));
    l.indices = align_up(l.vertices + std::size_t{header.vertex_count} * sizeof(render::Vertex));
    l.submeshes = align_up(l.indices + std::size_t{header.index_count} * sizeof(std::uint32_t));
    l.materials = align_up(l.submeshes + std::size_t{header.submesh_count} * sizeof(render::Submesh));
    l.images = align_up(l.materials + std::size_t{header.material_count} * sizeof(MaterialRecord));
    l.pixels = align_up(l.images + std::size_t{header.image_count} * sizeof(ImageRecord));
    l.end = l.pixels + static_cast<std::size_t>(pixel_bytes);
    return l;
}

// The image's levels must be exactly what its dimensions and level count say.
bool check_image(const CookedImage& image, std::size_t index, std::string& error) {
    if (image.width == 0 || image.height == 0) {
        error = "image " + std::to_string(index) + " has no size";
        return false;
    }
    if (image.mip_count == 0 || image.mip_count > full_mip_count(image.width, image.height)) {
        error = "image " + std::to_string(index) + " has " + std::to_string(image.mip_count) +
                " mip levels, which its size cannot hold";
        return false;
    }
    if (image.levels.size() != mip_chain_size(image.width, image.height, image.mip_count)) {
        error = "image " + std::to_string(index) + " holds " + std::to_string(image.levels.size()) +
                " bytes of levels, not the " +
                std::to_string(mip_chain_size(image.width, image.height, image.mip_count)) +
                " its size and levels say";
        return false;
    }
    return true;
}

bool check_mesh(const render::MeshData& mesh, std::string& error) {
    for (std::size_t s = 0; s < mesh.submeshes.size(); ++s) {
        const render::Submesh& sub = mesh.submeshes[s];
        if (std::uint64_t{sub.first_index} + sub.index_count > mesh.indices.size()) {
            error = "submesh " + std::to_string(s) + " reaches past the indices";
            return false;
        }
    }
    for (std::size_t i = 0; i < mesh.indices.size(); ++i) {
        if (mesh.indices[i] >= mesh.vertices.size()) {
            error = "index " + std::to_string(i) + " is " + std::to_string(mesh.indices[i]) + ", past the " +
                    std::to_string(mesh.vertices.size()) + " vertices";
            return false;
        }
    }
    return true;
}

template <typename T> void put(core::Bytes& out, std::size_t offset, const T* items, std::size_t count) {
    if (count > 0) {
        std::memcpy(out.data() + offset, items, count * sizeof(T));
    }
}

template <typename T> void get(const std::uint8_t* bytes, std::size_t offset, T* items, std::size_t count) {
    if (count > 0) {
        std::memcpy(items, bytes + offset, count * sizeof(T));
    }
}

} // namespace

std::size_t mip_chain_size(std::uint32_t width, std::uint32_t height, std::uint32_t mip_count) noexcept {
    std::size_t total = 0;
    for (std::uint32_t level = 0; level < mip_count; ++level) {
        total += std::size_t{mip_extent(width, level)} * mip_extent(height, level) * 4;
    }
    return total;
}

std::size_t mip_level_size(const CookedImage& image, std::uint32_t level) noexcept {
    return std::size_t{mip_extent(image.width, level)} * mip_extent(image.height, level) * 4;
}

std::size_t mip_level_offset(const CookedImage& image, std::uint32_t level) noexcept {
    return mip_chain_size(image.width, image.height, level);
}

bool write_model_blob(const CookedModel& model, core::Bytes& out, std::string& error) {
    TY_PROFILE_SCOPE_NAMED("assets::write_model_blob");
    out.clear();
    if (!check_mesh(model.mesh, error)) {
        return false;
    }
    if (model.mesh.vertices.size() > 0xFFFFFFFFu || model.mesh.indices.size() > 0xFFFFFFFFu) {
        error = "more vertices or indices than a 32-bit count holds";
        return false;
    }
    std::uint64_t pixel_bytes = 0;
    for (std::size_t i = 0; i < model.images.size(); ++i) {
        if (!check_image(model.images[i], i, error)) {
            return false;
        }
        pixel_bytes = align_up(static_cast<std::size_t>(pixel_bytes)) + model.images[i].levels.size();
    }

    Header header{};
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.version = kModelBlobVersion;
    header.vertex_count = static_cast<std::uint32_t>(model.mesh.vertices.size());
    header.index_count = static_cast<std::uint32_t>(model.mesh.indices.size());
    header.submesh_count = static_cast<std::uint32_t>(model.mesh.submeshes.size());
    header.material_count = static_cast<std::uint32_t>(model.materials.size());
    header.image_count = static_cast<std::uint32_t>(model.images.size());
    header.flags = 0;
    std::memcpy(header.bounds_min, &model.mesh.bounds_min, sizeof(header.bounds_min));
    std::memcpy(header.bounds_max, &model.mesh.bounds_max, sizeof(header.bounds_max));
    const Layout l = layout_for(header, pixel_bytes);
    header.file_size = l.end;

    out.resize(l.end);
    std::memset(out.data(), 0,
                out.size()); // the padding between sections is zero, so the file is the same twice
    put(out, 0, &header, 1);
    put(out, l.vertices, model.mesh.vertices.data(), model.mesh.vertices.size());
    put(out, l.indices, model.mesh.indices.data(), model.mesh.indices.size());
    put(out, l.submeshes, model.mesh.submeshes.data(), model.mesh.submeshes.size());
    for (std::size_t i = 0; i < model.materials.size(); ++i) {
        const render::MaterialData& m = model.materials[i];
        MaterialRecord record{};
        std::memcpy(record.base_color_factor, &m.base_color_factor, sizeof(record.base_color_factor));
        record.metallic_factor = m.metallic_factor;
        record.roughness_factor = m.roughness_factor;
        record.normal_scale = m.normal_scale;
        record.occlusion_strength = m.occlusion_strength;
        std::memcpy(record.emissive_factor, &m.emissive_factor, sizeof(record.emissive_factor));
        record.base_color_image = m.base_color_image;
        record.metallic_roughness_image = m.metallic_roughness_image;
        record.normal_image = m.normal_image;
        record.occlusion_image = m.occlusion_image;
        record.emissive_image = m.emissive_image;
        record.flags = m.double_sided ? 1u : 0u;
        put(out, l.materials + i * sizeof(MaterialRecord), &record, 1);
    }
    std::size_t pixel_offset = 0;
    for (std::size_t i = 0; i < model.images.size(); ++i) {
        const CookedImage& image = model.images[i];
        pixel_offset = align_up(pixel_offset);
        ImageRecord record{};
        record.width = image.width;
        record.height = image.height;
        record.mip_count = image.mip_count;
        record.format = image.srgb ? kRgba8Srgb : kRgba8Unorm;
        record.offset = pixel_offset;
        record.size = image.levels.size();
        put(out, l.images + i * sizeof(ImageRecord), &record, 1);
        put(out, l.pixels + pixel_offset, image.levels.data(), image.levels.size());
        pixel_offset += image.levels.size();
    }
    return true;
}

bool save_model_blob(const CookedModel& model, const char* path, std::string& error) {
    core::Bytes bytes;
    if (!write_model_blob(model, bytes, error)) {
        return false;
    }
    // Named for this process: two cooking the same model at once (a test
    // suite's runs) each rename their own whole file over the path.
    const std::string temporary = std::string(path) + "." + std::to_string(platform::process_id()) + ".tmp";
    if (!platform::write_file(temporary.c_str(), bytes.data(), bytes.size())) {
        error = "cannot write '" + temporary + "'";
        return false;
    }
    if (!platform::rename_file(temporary.c_str(), path)) {
        error = "cannot move '" + temporary + "' over '" + path + "'";
        (void)platform::remove_file(temporary.c_str());
        return false;
    }
    return true;
}

bool read_model_blob(const void* data, std::size_t size, CookedModel& out, std::string& error) {
    TY_PROFILE_SCOPE_NAMED("assets::read_model_blob");
    out = CookedModel{};
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    Header header{};
    if (bytes == nullptr || size < sizeof(Header)) {
        error = "too short to be a model blob";
        return false;
    }
    get(bytes, 0, &header, 1);
    if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0) {
        error = "not a model blob";
        return false;
    }
    if (header.version != kModelBlobVersion) {
        error = "cooked with model blob version " + std::to_string(header.version) +
                ", and this engine reads " + std::to_string(kModelBlobVersion) + ": cook it again";
        return false;
    }
    if (header.file_size != size) {
        error = "the header says " + std::to_string(header.file_size) + " bytes and the file has " +
                std::to_string(size) + ": truncated or edited";
        return false;
    }
    // Every count against the size: the sections must end inside the file,
    // and what is left after them is the pixels.
    const Layout sections = layout_for(header, 0);
    if (sections.pixels > size) {
        error = "the counts in the header do not fit the file";
        return false;
    }
    const std::uint64_t pixel_bytes = size - sections.pixels;
    const Layout l = layout_for(header, pixel_bytes);

    out.mesh.vertices.resize(header.vertex_count);
    out.mesh.indices.resize(header.index_count);
    out.mesh.submeshes.resize(header.submesh_count);
    get(bytes, l.vertices, out.mesh.vertices.data(), out.mesh.vertices.size());
    get(bytes, l.indices, out.mesh.indices.data(), out.mesh.indices.size());
    get(bytes, l.submeshes, out.mesh.submeshes.data(), out.mesh.submeshes.size());
    std::memcpy(&out.mesh.bounds_min, header.bounds_min, sizeof(header.bounds_min));
    std::memcpy(&out.mesh.bounds_max, header.bounds_max, sizeof(header.bounds_max));
    if (!check_mesh(out.mesh, error)) {
        out = CookedModel{};
        return false;
    }

    out.materials.resize(header.material_count);
    for (std::size_t i = 0; i < out.materials.size(); ++i) {
        MaterialRecord record{};
        get(bytes, l.materials + i * sizeof(MaterialRecord), &record, 1);
        render::MaterialData& m = out.materials[i];
        std::memcpy(&m.base_color_factor, record.base_color_factor, sizeof(record.base_color_factor));
        m.metallic_factor = record.metallic_factor;
        m.roughness_factor = record.roughness_factor;
        m.normal_scale = record.normal_scale;
        m.occlusion_strength = record.occlusion_strength;
        std::memcpy(&m.emissive_factor, record.emissive_factor, sizeof(record.emissive_factor));
        // An image index past the images is a missing map, not a crash later.
        const auto image_or_none = [&](std::int32_t index) {
            return index >= 0 && static_cast<std::uint32_t>(index) < header.image_count ? index : -1;
        };
        m.base_color_image = image_or_none(record.base_color_image);
        m.metallic_roughness_image = image_or_none(record.metallic_roughness_image);
        m.normal_image = image_or_none(record.normal_image);
        m.occlusion_image = image_or_none(record.occlusion_image);
        m.emissive_image = image_or_none(record.emissive_image);
        m.double_sided = (record.flags & 1u) != 0;
    }

    out.images.resize(header.image_count);
    for (std::size_t i = 0; i < out.images.size(); ++i) {
        ImageRecord record{};
        get(bytes, l.images + i * sizeof(ImageRecord), &record, 1);
        CookedImage& image = out.images[i];
        image.width = record.width;
        image.height = record.height;
        image.mip_count = record.mip_count;
        image.srgb = record.format == kRgba8Srgb;
        if (record.format != kRgba8Unorm && record.format != kRgba8Srgb) {
            error = "image " + std::to_string(i) + " is in a format this engine does not know";
            out = CookedModel{};
            return false;
        }
        if (record.offset > pixel_bytes || record.size > pixel_bytes - record.offset) {
            error = "image " + std::to_string(i) + " reaches past the pixels";
            out = CookedModel{};
            return false;
        }
        image.levels.assign(bytes + l.pixels + static_cast<std::size_t>(record.offset),
                            static_cast<std::size_t>(record.size));
        if (!check_image(image, i, error)) {
            out = CookedModel{};
            return false;
        }
    }
    return true;
}

bool load_model_blob_file(const char* path, CookedModel& out, std::string& error) {
    TY_PROFILE_SCOPE_NAMED("assets::load_model_blob_file");
    core::Bytes bytes;
    if (!platform::read_file(path, bytes)) {
        error = std::string("cannot read '") + path + "'";
        return false;
    }
    return read_model_blob(bytes.data(), bytes.size(), out, error);
}

std::uint32_t model_blob_version(const char* path) noexcept {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return 0;
    }
    Header header{};
    const bool read = std::fread(&header, sizeof(Header), 1, file) == 1;
    std::fclose(file);
    return read && std::memcmp(header.magic, kMagic, sizeof(kMagic)) == 0 ? header.version : 0;
}

bool upload_cooked_model(rhi::Device& device, const CookedModel& model,
                         const render::FallbackTextures& fallbacks, render::Model& out) noexcept {
    TY_PROFILE_SCOPE_NAMED("assets::upload_cooked_model");
    render::destroy_model(device, out);
    if (!render::upload_mesh(device, model.mesh, out.mesh)) {
        return false;
    }
    out.textures.reserve(model.images.size());
    for (std::size_t i = 0; i < model.images.size(); ++i) {
        const CookedImage& image = model.images[i];
        // Every level is in the file: a sampled texture, no render-target
        // usage for a mip pass the GPU never runs.
        rhi::TextureHandle texture = device.create_texture(
            {.format = image.srgb ? rhi::TextureFormat::Rgba8Srgb : rhi::TextureFormat::Rgba8Unorm,
             .width = image.width,
             .height = image.height,
             .mip_levels = image.mip_count,
             .usage = rhi::TextureUsage::Sampled});
        for (std::uint32_t level = 0; texture && level < image.mip_count; ++level) {
            if (!device.upload_texture(texture, image.levels.data() + mip_level_offset(image, level),
                                       static_cast<std::uint32_t>(mip_level_size(image, level)), level)) {
                device.destroy_texture(texture);
                texture = {};
            }
        }
        if (!texture) {
            TY_LOG_WARN("model", "image %zu (%ux%u) failed to upload: %s", i, image.width, image.height,
                        platform::last_error());
        }
        out.textures.push_back(texture);
    }
    render::resolve_materials(model.materials.data(), static_cast<std::uint32_t>(model.materials.size()),
                              fallbacks, out);
    return true;
}

} // namespace tynima::assets
