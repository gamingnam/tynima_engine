#pragma once

#include <tynima/core/bytes.h>
#include <tynima/render/model.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// A cooked model: the file the cooker writes and the runtime loads. Every
// byte in it is what the GPU takes as it is — the vertex buffer in the one
// vertex format the pipelines read (render::Vertex), 32-bit indices in the
// order the vertex cache likes, every texture with its whole mip chain
// already built, in an 8-bit format the device samples directly. Loading is
// reading the file and a copy per buffer; no glTF is parsed, no PNG decoded,
// no normal computed, no mip generated.
//
//   header      64 bytes: "TYMD", the version, the counts, the bounds, the file's size
//   vertices    render::Vertex × vertex_count
//   indices     uint32_t × index_count
//   submeshes   render::Submesh × submesh_count
//   materials   MaterialRecord × material_count
//   images      ImageRecord × image_count
//   pixels      every image's levels in turn, level 0 first, tightly packed
//
// Little-endian, each section at a 16-byte boundary. The reader checks every
// count and offset against the file's size before it copies anything, and
// every index against the vertex count, so a truncated or edited file is a
// message rather than a GPU reading past a buffer. A file of another
// version is refused too: the cooker makes a new one from the source.
namespace tynima::assets {

inline constexpr std::uint32_t kModelBlobVersion = 1;
inline constexpr const char* kModelBlobExtension = ".tymodel";

// One texture, its levels packed one after the other from level 0 down to
// 1x1, each level width x height x 4 bytes of RGBA8, rows top to bottom.
struct CookedImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mip_count = 1;
    bool srgb = false; // sampled through an sRGB format: base color, emissive
    core::Bytes levels;
};
// The width or height of a level: halved and halved again, never below one.
[[nodiscard]] constexpr std::uint32_t mip_extent(std::uint32_t extent, std::uint32_t level) noexcept {
    const std::uint32_t halved = extent >> level;
    return halved > 0 ? halved : 1;
}
// Levels in a chain from this size down to 1x1.
[[nodiscard]] constexpr std::uint32_t full_mip_count(std::uint32_t width, std::uint32_t height) noexcept {
    std::uint32_t count = 1;
    for (std::uint32_t extent = width > height ? width : height; extent > 1; extent >>= 1) {
        ++count;
    }
    return count;
}
[[nodiscard]] std::size_t mip_level_size(const CookedImage& image, std::uint32_t level) noexcept;
[[nodiscard]] std::size_t mip_level_offset(const CookedImage& image, std::uint32_t level) noexcept;
// What `levels` must hold for the image's size and level count.
[[nodiscard]] std::size_t mip_chain_size(std::uint32_t width, std::uint32_t height,
                                         std::uint32_t mip_count) noexcept;

// Everything in a cooked file, as the cooker builds it and the reader gives it back.
struct CookedModel {
    render::MeshData mesh;
    std::vector<render::MaterialData> materials;
    std::vector<CookedImage> images;
};

// The file's bytes. false when the model does not fit the format: an image
// whose levels are not the size its dimensions say, a submesh past the
// indices, an index past the vertices.
[[nodiscard]] bool write_model_blob(const CookedModel& model, core::Bytes& out, std::string& error);
// Written to a temporary file beside `path` (named for this process) and
// renamed over it, so a reader polling the file — the runtime hot-reloading
// it — never sees half of one, and two writers at once each write a whole one.
[[nodiscard]] bool save_model_blob(const CookedModel& model, const char* path, std::string& error);

[[nodiscard]] bool read_model_blob(const void* bytes, std::size_t size, CookedModel& out, std::string& error);
[[nodiscard]] bool load_model_blob_file(const char* path, CookedModel& out, std::string& error);
// The version a file was cooked with, from its header alone; 0 when it is
// not a model blob or cannot be read. What decides whether to cook again.
[[nodiscard]] std::uint32_t model_blob_version(const char* path) noexcept;

// Uploads the mesh and every image, level by level, into a Model. false if
// the mesh fails (an image that fails is a warning: its materials fall back).
[[nodiscard]] bool upload_cooked_model(rhi::Device& device, const CookedModel& model,
                                       const render::FallbackTextures& fallbacks,
                                       render::Model& out) noexcept;

} // namespace tynima::assets
