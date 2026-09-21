#pragma once

#include <tynima/assets/model_blob.h>
#include <tynima/render/model.h>

#include <cstdint>

// Mip chains built offline, so a texture is uploaded with every level in
// hand and the GPU never runs a mip pass at load. Each level is the one
// above it averaged 2x2 (a box filter: the right answer for minification
// by exactly half, and what the GPU's own generator does). An odd
// dimension loses its last row or column, as it would there; a dimension
// already at one stays. Display-encoded images are
// averaged in linear light — averaging sRGB bytes darkens every level a
// little and a distant texture visibly — through a table, so the result is
// the same bytes on every platform, whatever its libm.
namespace tynima::cooker {

// Level 0 is the image; every further level down to 1x1 when `mipmaps`,
// else that one level. `out.srgb` follows the image.
void build_mip_chain(const render::ImageData& image, bool mipmaps, assets::CookedImage& out);

// One level from the one above it: `width` x `height` are the source's.
void downsample_2x2(const std::uint8_t* source, std::uint32_t width, std::uint32_t height, bool srgb,
                    std::uint8_t* out) noexcept;

// The byte a linear value encodes to, and back, as the mip filter uses them.
[[nodiscard]] float srgb_to_linear(std::uint8_t encoded) noexcept;
[[nodiscard]] std::uint8_t linear_to_srgb(float linear) noexcept;

} // namespace tynima::cooker
