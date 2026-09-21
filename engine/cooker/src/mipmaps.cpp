#include <tynima/cooker/mipmaps.h>

#include <tynima/core/profile.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tynima::cooker {

namespace {

// sRGB's transfer function, once per byte. The table is the only place a
// pow is evaluated; decoding reads it and encoding searches it, so the
// bytes a mip chain comes out as do not depend on how a platform's libm
// rounds a pow — one ulp there could otherwise tip a byte over.
struct SrgbTable {
    float linear[256];
    SrgbTable() noexcept {
        for (int i = 0; i < 256; ++i) {
            const float c = static_cast<float>(i) / 255.0f;
            linear[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
    }
};

const SrgbTable& table() noexcept {
    static const SrgbTable instance;
    return instance;
}

} // namespace

float srgb_to_linear(std::uint8_t encoded) noexcept {
    return table().linear[encoded];
}

std::uint8_t linear_to_srgb(float linear) noexcept {
    const float* begin = table().linear;
    const float* end = begin + 256;
    // The first byte whose linear value is not below `linear`, then whichever
    // of it and the byte before is nearer.
    const float* at = std::lower_bound(begin, end, linear);
    if (at == end) {
        return 255;
    }
    if (at == begin) {
        return 0;
    }
    const float above = *at - linear;
    const float below = linear - *(at - 1);
    return static_cast<std::uint8_t>((at - begin) - (below < above ? 1 : 0));
}

void downsample_2x2(const std::uint8_t* source, std::uint32_t width, std::uint32_t height, bool srgb,
                    std::uint8_t* out) noexcept {
    const std::uint32_t out_width = width > 1 ? width / 2 : 1;
    const std::uint32_t out_height = height > 1 ? height / 2 : 1;
    for (std::uint32_t y = 0; y < out_height; ++y) {
        const std::uint32_t y0 = std::min(2 * y, height - 1);
        const std::uint32_t y1 = std::min(2 * y + 1, height - 1);
        for (std::uint32_t x = 0; x < out_width; ++x) {
            const std::uint32_t x0 = std::min(2 * x, width - 1);
            const std::uint32_t x1 = std::min(2 * x + 1, width - 1);
            const std::uint8_t* texels[4] = {
                source + (std::size_t{y0} * width + x0) * 4, source + (std::size_t{y0} * width + x1) * 4,
                source + (std::size_t{y1} * width + x0) * 4, source + (std::size_t{y1} * width + x1) * 4};
            std::uint8_t* texel = out + (std::size_t{y} * out_width + x) * 4;
            for (int channel = 0; channel < 3; ++channel) {
                if (srgb) {
                    float sum = 0.0f;
                    for (const std::uint8_t* t : texels) {
                        sum += srgb_to_linear(t[channel]);
                    }
                    texel[channel] = linear_to_srgb(sum * 0.25f);
                } else {
                    const unsigned sum =
                        texels[0][channel] + texels[1][channel] + texels[2][channel] + texels[3][channel];
                    texel[channel] = static_cast<std::uint8_t>((sum + 2) / 4);
                }
            }
            // Alpha is coverage: linear in either kind of image.
            const unsigned alpha = texels[0][3] + texels[1][3] + texels[2][3] + texels[3][3];
            texel[3] = static_cast<std::uint8_t>((alpha + 2) / 4);
        }
    }
}

void build_mip_chain(const render::ImageData& image, bool mipmaps, assets::CookedImage& out) {
    TY_PROFILE_SCOPE_NAMED("cooker::build_mip_chain");
    out = assets::CookedImage{};
    out.width = image.width;
    out.height = image.height;
    out.srgb = image.srgb;
    out.mip_count = mipmaps ? assets::full_mip_count(image.width, image.height) : 1;
    if (image.width == 0 || image.height == 0 ||
        image.pixels.size() < std::size_t{image.width} * image.height * 4) {
        out = assets::CookedImage{};
        return;
    }
    out.levels.resize(assets::mip_chain_size(out.width, out.height, out.mip_count));
    std::memcpy(out.levels.data(), image.pixels.data(), assets::mip_level_size(out, 0));
    for (std::uint32_t level = 1; level < out.mip_count; ++level) {
        downsample_2x2(out.levels.data() + assets::mip_level_offset(out, level - 1),
                       assets::mip_extent(out.width, level - 1), assets::mip_extent(out.height, level - 1),
                       out.srgb, out.levels.data() + assets::mip_level_offset(out, level));
    }
}

} // namespace tynima::cooker
