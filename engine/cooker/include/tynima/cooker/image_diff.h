#pragma once

#include <tynima/render/model.h>

#include <cstdint>

// Whether two pictures look the same: the comparison behind the golden
// image tests (tools/golden). Not whether the bytes are equal — a GPU's
// rasteriser puts an edge half a pixel off another's, its texture units
// filter a little differently, its shading rounds elsewhere — but whether
// a person would see a difference. Each pixel of both images is taken to
// OKLab, a colour space in which the distance between two colours is
// close to how different they look, and the distance between the two is
// the pixel's difference; a pixel whose distance is under the threshold
// (0.02 is about the least a person notices) is the same. An edge that
// moved a pixel is forgiven: a pixel also counts as the same when any
// pixel within one of it in the other image is close enough, which a
// whole region of wrong colour never is. What comes out is how many
// pixels differ and by how much on average and at most; the caller
// decides what fraction it tolerates.
namespace tynima::cooker {

struct DiffOptions {
    float threshold = 0.02f;        // OKLab distance a pixel may differ by and still be the same
    bool forgive_neighbours = true; // a match within one pixel counts: edges that moved
};

struct DiffResult {
    bool comparable = false; // the same size, with pixels; false is a difference in itself
    std::uint32_t differing = 0;
    float fraction = 0.0f; // differing / all
    float mean = 0.0f;     // the average distance, over every pixel
    float max = 0.0f;
};

// Both images RGBA8, display-encoded (sRGB), as a PNG holds them. When
// `heatmap` is given it becomes an image of the same size showing where
// they differ: the second image dimmed, with each differing pixel painted
// the brighter red the further apart it is.
[[nodiscard]] DiffResult diff_images(const render::ImageData& expected, const render::ImageData& actual,
                                     const DiffOptions& options = {}, render::ImageData* heatmap = nullptr);

// The expected image, the actual one and the heatmap side by side in one
// picture, for a look at a failure.
[[nodiscard]] render::ImageData diff_strip(const render::ImageData& expected, const render::ImageData& actual,
                                           const render::ImageData& heatmap);

// sRGB bytes to OKLab: L, a, b. The distance between two is their difference.
void srgb_to_oklab(const std::uint8_t rgb[3], float lab[3]) noexcept;

} // namespace tynima::cooker
