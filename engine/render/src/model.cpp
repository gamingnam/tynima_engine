#include <tynima/render/model.h>

#include <tynima/core/profile.h>

namespace tynima::render {

bool upload_model(rhi::Device& device, const ModelData& data, rhi::Texture& fallback_white, Model& out) noexcept {
    TY_PROFILE_SCOPE_NAMED("render::upload_model");
    destroy_model(device, out);
    if (!upload_mesh(device, data.mesh, out.mesh)) {
        return false;
    }

    out.textures.reserve(data.images.size());
    for (const ImageData& image : data.images) {
        rhi::Texture* texture = nullptr;
        if (!image.pixels.empty()) {
            texture = device.create_texture_with_data(
                image.srgb ? rhi::TextureFormat::Rgba8Srgb : rhi::TextureFormat::Rgba8Unorm, image.width, image.height,
                image.pixels.data(), static_cast<std::uint32_t>(image.pixels.size()), true);
        }
        out.textures.push_back(texture);
    }

    auto texture_or_white = [&](std::int32_t index) -> rhi::Texture* {
        if (index >= 0 && static_cast<std::size_t>(index) < out.textures.size() && out.textures[index] != nullptr) {
            return out.textures[index];
        }
        return &fallback_white;
    };

    out.materials.reserve(data.materials.size() + 1);
    for (const MaterialData& m : data.materials) {
        Material material;
        material.base_color_factor = m.base_color_factor;
        material.base_color = texture_or_white(m.base_color_image);
        material.metallic_factor = m.metallic_factor;
        material.roughness_factor = m.roughness_factor;
        material.metallic_roughness = texture_or_white(m.metallic_roughness_image);
        material.normal = m.normal_image >= 0 ? texture_or_white(m.normal_image) : nullptr; // white is not a normal
        material.normal_scale = m.normal_scale;
        material.occlusion = texture_or_white(m.occlusion_image);
        material.occlusion_strength = m.occlusion_strength;
        material.emissive_factor = m.emissive_factor;
        material.emissive = texture_or_white(m.emissive_image);
        material.double_sided = m.double_sided;
        out.materials.push_back(material);
    }
    if (out.materials.empty()) {
        Material material;
        material.base_color = material.metallic_roughness = material.occlusion = material.emissive = &fallback_white;
        material.metallic_factor = 0.0f; // an untextured, unspecified material reads as matte dielectric
        material.roughness_factor = 0.8f;
        out.materials.push_back(material);
    }
    // A submesh may name a material the file did not define; clamp rather than crash.
    for (Submesh& sub : out.mesh.submeshes) {
        if (sub.material >= out.materials.size()) {
            sub.material = 0;
        }
    }
    return true;
}

void destroy_model(rhi::Device& device, Model& model) noexcept {
    destroy_mesh(device, model.mesh);
    for (rhi::Texture* texture : model.textures) {
        device.destroy_texture(texture);
    }
    model = Model{};
}

rhi::Texture* create_white_texture(rhi::Device& device) noexcept {
    const std::uint8_t white[4] = {255, 255, 255, 255};
    return device.create_texture_with_data(rhi::TextureFormat::Rgba8Unorm, 1, 1, white, sizeof(white), false);
}

} // namespace tynima::render
