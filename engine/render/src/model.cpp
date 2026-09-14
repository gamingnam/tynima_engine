#include <tynima/render/model.h>

#include <tynima/core/profile.h>

namespace tynima::render {

bool upload_model(rhi::Device& device, const ModelData& data, const FallbackTextures& fallbacks, Model& out) noexcept {
    TY_PROFILE_SCOPE_NAMED("render::upload_model");
    destroy_model(device, out);
    if (!upload_mesh(device, data.mesh, out.mesh)) {
        return false;
    }

    out.textures.reserve(data.images.size());
    for (const ImageData& image : data.images) {
        rhi::TextureHandle texture;
        if (!image.pixels.empty()) {
            texture = device.create_texture_with_data(
                image.srgb ? rhi::TextureFormat::Rgba8Srgb : rhi::TextureFormat::Rgba8Unorm, image.width, image.height,
                image.pixels.data(), static_cast<std::uint32_t>(image.pixels.size()), true);
        }
        out.textures.push_back(texture);
    }

    auto texture_or = [&](std::int32_t index, rhi::TextureHandle fallback) -> rhi::TextureHandle {
        if (index >= 0 && static_cast<std::size_t>(index) < out.textures.size() && out.textures[index]) {
            return out.textures[index];
        }
        return fallback;
    };

    out.materials.reserve(data.materials.size() + 1);
    for (const MaterialData& m : data.materials) {
        Material material;
        material.base_color_factor = m.base_color_factor;
        material.base_color = texture_or(m.base_color_image, fallbacks.white);
        material.metallic_factor = m.metallic_factor;
        material.roughness_factor = m.roughness_factor;
        material.metallic_roughness = texture_or(m.metallic_roughness_image, fallbacks.white);
        material.normal = texture_or(m.normal_image, fallbacks.flat_normal);
        material.normal_scale = m.normal_scale;
        material.occlusion = texture_or(m.occlusion_image, fallbacks.white);
        material.occlusion_strength = m.occlusion_strength;
        material.emissive_factor = m.emissive_factor;
        material.emissive = texture_or(m.emissive_image, fallbacks.white);
        material.double_sided = m.double_sided;
        out.materials.push_back(material);
    }
    if (out.materials.empty()) {
        Material material;
        material.base_color = material.metallic_roughness = material.occlusion = material.emissive = fallbacks.white;
        material.normal = fallbacks.flat_normal;
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
    for (const rhi::TextureHandle texture : model.textures) {
        device.destroy_texture(texture);
    }
    model = Model{};
}

bool create_fallback_textures(rhi::Device& device, FallbackTextures& out) noexcept {
    const std::uint8_t white[4] = {255, 255, 255, 255};
    const std::uint8_t flat[4] = {128, 128, 255, 255};
    out.white = device.create_texture_with_data(rhi::TextureFormat::Rgba8Unorm, 1, 1, white, sizeof(white), false);
    out.flat_normal = device.create_texture_with_data(rhi::TextureFormat::Rgba8Unorm, 1, 1, flat, sizeof(flat), false);
    if (!out.white || !out.flat_normal) {
        destroy_fallback_textures(device, out);
        return false;
    }
    return true;
}

void destroy_fallback_textures(rhi::Device& device, FallbackTextures& textures) noexcept {
    device.destroy_texture(textures.white);
    device.destroy_texture(textures.flat_normal);
    textures = FallbackTextures{};
}

} // namespace tynima::render
