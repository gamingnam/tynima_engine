#include <tynima/cooker/image_diff.h>

#include <tynima/cooker/mipmaps.h>
#include <tynima/core/profile.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace tynima::cooker {

namespace {

float distance(const float* a, const float* b) noexcept {
    const float dl = a[0] - b[0];
    const float da = a[1] - b[1];
    const float db = a[2] - b[2];
    return std::sqrt(dl * dl + da * da + db * db);
}

// Every pixel of an image in OKLab, three floats each.
std::vector<float> to_oklab(const render::ImageData& image) {
    std::vector<float> lab(std::size_t{image.width} * image.height * 3);
    for (std::size_t i = 0; i < std::size_t{image.width} * image.height; ++i) {
        srgb_to_oklab(image.pixels.data() + i * 4, lab.data() + i * 3);
    }
    return lab;
}

// The nearest colour to `color` within one pixel of (x, y) in `lab`.
float nearest_within_one(const float* color, const std::vector<float>& lab, std::uint32_t width,
                         std::uint32_t height, std::uint32_t x, std::uint32_t y) noexcept {
    float best = 3.402823466e+38f;
    const std::uint32_t x0 = x > 0 ? x - 1 : 0, x1 = std::min(x + 1, width - 1);
    const std::uint32_t y0 = y > 0 ? y - 1 : 0, y1 = std::min(y + 1, height - 1);
    for (std::uint32_t yy = y0; yy <= y1; ++yy) {
        for (std::uint32_t xx = x0; xx <= x1; ++xx) {
            best = std::min(best, distance(color, lab.data() + (std::size_t{yy} * width + xx) * 3));
        }
    }
    return best;
}

} // namespace

void srgb_to_oklab(const std::uint8_t rgb[3], float lab[3]) noexcept {
    // Björn Ottosson's OKLab: linear sRGB to a cone response, a cube root
    // to make it perceptual, then to lightness and two opponent axes.
    const float r = srgb_to_linear(rgb[0]);
    const float g = srgb_to_linear(rgb[1]);
    const float b = srgb_to_linear(rgb[2]);
    const float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    const float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    const float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    lab[0] = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    lab[1] = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    lab[2] = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

DiffResult diff_images(const render::ImageData& expected, const render::ImageData& actual,
                       const DiffOptions& options, render::ImageData* heatmap) {
    TY_PROFILE_SCOPE_NAMED("cooker::diff_images");
    DiffResult result;
    const std::size_t count = std::size_t{expected.width} * expected.height;
    if (count == 0 || expected.width != actual.width || expected.height != actual.height ||
        expected.pixels.size() < count * 4 || actual.pixels.size() < count * 4) {
        return result;
    }
    result.comparable = true;
    const std::vector<float> lab_expected = to_oklab(expected);
    const std::vector<float> lab_actual = to_oklab(actual);
    if (heatmap != nullptr) {
        heatmap->width = actual.width;
        heatmap->height = actual.height;
        heatmap->srgb = true;
        heatmap->pixels.assign(count * 4, 255);
    }
    double sum = 0.0;
    for (std::uint32_t y = 0; y < expected.height; ++y) {
        for (std::uint32_t x = 0; x < expected.width; ++x) {
            const std::size_t i = std::size_t{y} * expected.width + x;
            const float* e = lab_expected.data() + i * 3;
            const float* a = lab_actual.data() + i * 3;
            const float direct = distance(e, a);
            sum += static_cast<double>(direct);
            float effective = direct;
            if (effective > options.threshold && options.forgive_neighbours) {
                // The same picture with an edge a pixel over: each pixel has
                // its colour close by in the other image. A wrong region has not.
                effective =
                    std::max(nearest_within_one(e, lab_actual, expected.width, expected.height, x, y),
                             nearest_within_one(a, lab_expected, expected.width, expected.height, x, y));
            }
            result.max = std::max(result.max, effective);
            const bool differs = effective > options.threshold;
            result.differing += differs ? 1u : 0u;
            if (heatmap != nullptr) {
                std::uint8_t* out = heatmap->pixels.data() + i * 4;
                const std::uint8_t* in = actual.pixels.data() + i * 4;
                if (differs) {
                    // Red, the brighter the further apart: full red at four thresholds.
                    const float over =
                        std::min(1.0f, (effective - options.threshold) / (3.0f * options.threshold));
                    out[0] = static_cast<std::uint8_t>(128.0f + 127.0f * over);
                    out[1] = 0;
                    out[2] = 0;
                } else {
                    out[0] = static_cast<std::uint8_t>(in[0] * 0.3f);
                    out[1] = static_cast<std::uint8_t>(in[1] * 0.3f);
                    out[2] = static_cast<std::uint8_t>(in[2] * 0.3f);
                }
                out[3] = 255;
            }
        }
    }
    result.fraction = static_cast<float>(result.differing) / static_cast<float>(count);
    result.mean = static_cast<float>(sum / static_cast<double>(count));
    return result;
}

render::ImageData diff_strip(const render::ImageData& expected, const render::ImageData& actual,
                             const render::ImageData& heatmap) {
    render::ImageData strip;
    const render::ImageData* panels[3] = {&expected, &actual, &heatmap};
    std::uint32_t width = 0, height = 0;
    for (const render::ImageData* panel : panels) {
        width += panel->width;
        height = std::max(height, panel->height);
    }
    if (width == 0 || height == 0) {
        return strip;
    }
    strip.width = width;
    strip.height = height;
    strip.srgb = true;
    strip.pixels.assign(std::size_t{width} * height * 4, 0);
    std::uint32_t x_offset = 0;
    for (const render::ImageData* panel : panels) {
        for (std::uint32_t y = 0; y < panel->height; ++y) {
            if (panel->pixels.size() >= (std::size_t{y} + 1) * panel->width * 4) {
                std::copy_n(panel->pixels.data() + std::size_t{y} * panel->width * 4,
                            std::size_t{panel->width} * 4,
                            strip.pixels.data() + (std::size_t{y} * width + x_offset) * 4);
            }
        }
        x_offset += panel->width;
    }
    return strip;
}

} // namespace tynima::cooker
