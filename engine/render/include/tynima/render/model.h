#pragma once

#include <tynima/core/math.h>
#include <tynima/render/mesh.h>
#include <tynima/rhi/device.h>

#include <cstdint>
#include <vector>

namespace tynima::render {

// Decoded pixels: RGBA8, rows top to bottom, which is also the GPU's texture
// origin, so nothing is flipped between an image file and a sampled texel.
// `srgb` says whether the bytes are display-encoded (base color, emissive)
// or plain data (normals, roughness); the GPU format follows from it.
struct ImageData {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
    bool srgb = false;
};

// glTF's metallic-roughness material. Image indices point into
// ModelData::images; -1 means "none, use the factor".
struct MaterialData {
    math::Vec4 base_color_factor{1.0f, 1.0f, 1.0f, 1.0f};
    std::int32_t base_color_image = -1;
    float metallic_factor = 1.0f;
    float roughness_factor = 1.0f;
    std::int32_t metallic_roughness_image = -1;
    std::int32_t normal_image = -1;
    float normal_scale = 1.0f;
    std::int32_t occlusion_image = -1;
    float occlusion_strength = 1.0f;
    math::Vec3 emissive_factor{0.0f};
    std::int32_t emissive_image = -1;
    bool double_sided = false;
};

// Everything an importer produces for one model file.
struct ModelData {
    MeshData mesh;
    std::vector<MaterialData> materials;
    std::vector<ImageData> images;
};

// GPU-side material. Textures are borrowed: from Model::textures, or the 1x1
// white fallback when the material has none — white multiplies out to the
// factor, which is what glTF specifies for a missing map. Channel layout is
// glTF's: metallic in B and roughness in G of metallic_roughness, occlusion
// in R of occlusion, emissive RGB (sRGB), normal XYZ in tangent space.
struct Material {
    math::Vec4 base_color_factor{1.0f, 1.0f, 1.0f, 1.0f};
    rhi::Texture* base_color = nullptr;
    float metallic_factor = 1.0f;
    float roughness_factor = 1.0f;
    rhi::Texture* metallic_roughness = nullptr;
    rhi::Texture* normal = nullptr;
    float normal_scale = 1.0f;
    rhi::Texture* occlusion = nullptr;
    float occlusion_strength = 1.0f;
    math::Vec3 emissive_factor{0.0f};
    rhi::Texture* emissive = nullptr;
    bool double_sided = false;
};

struct Model {
    Mesh mesh;
    std::vector<Material> materials;    // indexed by Submesh::material
    std::vector<rhi::Texture*> textures; // owned, one per ModelData::images entry (nullptr if it failed)
};

// Uploads the mesh and every image; materials without a base color map point
// at `fallback_white`. false if the mesh fails (a failed image is a warning:
// its slot stays nullptr and the material falls back).
[[nodiscard]] bool upload_model(rhi::Device& device, const ModelData& data, rhi::Texture& fallback_white,
                                Model& out) noexcept;
void destroy_model(rhi::Device& device, Model& model) noexcept;

// 1x1 opaque white, so an untextured material samples 1.0 and shows its factor.
[[nodiscard]] rhi::Texture* create_white_texture(rhi::Device& device) noexcept;

} // namespace tynima::render
