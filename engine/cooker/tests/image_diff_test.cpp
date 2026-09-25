#include <tynima/cooker/image.h>
#include <tynima/cooker/image_diff.h>
#include <tynima/platform/file.h>

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

using namespace tynima;

namespace {

// Where this machine puts temporary files. std::filesystem asks the OS —
// TMPDIR where it is set, the user's Temp on Windows — rather than assuming
// "/tmp", which on Windows names a directory on whatever drive the test
// happens to run from and usually is not there at all.
std::string temp_path(const char* name) {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) {
        dir = "/tmp";
    }
    return (dir / name).lexically_normal().generic_string();
}

render::ImageData filled(std::uint32_t width, std::uint32_t height, std::uint8_t r, std::uint8_t g,
                         std::uint8_t b) {
    render::ImageData image;
    image.width = width;
    image.height = height;
    image.srgb = true;
    image.pixels.resize(std::size_t{width} * height * 4);
    for (std::size_t i = 0; i < std::size_t{width} * height; ++i) {
        image.pixels[i * 4] = r;
        image.pixels[i * 4 + 1] = g;
        image.pixels[i * 4 + 2] = b;
        image.pixels[i * 4 + 3] = 255;
    }
    return image;
}

void paint(render::ImageData& image, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1,
           std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    for (std::uint32_t y = y0; y < y1; ++y) {
        for (std::uint32_t x = x0; x < x1; ++x) {
            std::uint8_t* p = image.pixels.data() + (std::size_t{y} * image.width + x) * 4;
            p[0] = r;
            p[1] = g;
            p[2] = b;
        }
    }
}

} // namespace

TEST_CASE("OKLab: white is lightness one, black zero, mid grey six tenths, and hues are apart") {
    float lab[3];
    const std::uint8_t white[3] = {255, 255, 255};
    cooker::srgb_to_oklab(white, lab);
    CHECK(lab[0] == doctest::Approx(1.0f).epsilon(1e-3f));
    CHECK(lab[1] == doctest::Approx(0.0f).epsilon(1e-3f));
    CHECK(lab[2] == doctest::Approx(0.0f).epsilon(1e-3f));
    const std::uint8_t black[3] = {0, 0, 0};
    cooker::srgb_to_oklab(black, lab);
    CHECK(lab[0] == doctest::Approx(0.0f));
    const std::uint8_t grey[3] = {128, 128, 128};
    cooker::srgb_to_oklab(grey, lab);
    CHECK(lab[0] == doctest::Approx(0.6f).epsilon(1e-3f));
    float red[3], green[3];
    const std::uint8_t r[3] = {255, 0, 0}, g[3] = {0, 255, 0};
    cooker::srgb_to_oklab(r, red);
    cooker::srgb_to_oklab(g, green);
    CHECK(red[1] > 0.2f);    // red is well to the a+ side
    CHECK(green[1] < -0.2f); // green well to a-
}

TEST_CASE("two pictures are the same when no pixel looks different, and an edge may move a pixel") {
    render::ImageData a = filled(32, 16, 60, 120, 200);
    paint(a, 8, 4, 20, 12, 220, 80, 40); // an orange block on blue
    render::ImageData b = a;
    cooker::DiffResult same = cooker::diff_images(a, b);
    CHECK(same.comparable);
    CHECK(same.differing == 0);
    CHECK(same.fraction == 0.0f);
    CHECK(same.mean == 0.0f);
    CHECK(same.max == 0.0f);

    // The block one pixel to the right: forgiven with neighbours, not without.
    render::ImageData shifted = filled(32, 16, 60, 120, 200);
    paint(shifted, 9, 4, 21, 12, 220, 80, 40);
    const cooker::DiffResult moved = cooker::diff_images(a, shifted);
    CHECK(moved.differing == 0);
    CHECK(moved.mean > 0.0f); // the average still knows the edge columns changed
    const cooker::DiffResult strict = cooker::diff_images(a, shifted, {.forgive_neighbours = false});
    CHECK(strict.differing == 16); // a column of 8 gone, a column of 8 come
    CHECK(strict.max > 0.1f);

    // A block two pixels over is a change: its four moved columns, less the
    // corners that have a like colour diagonally beside them. So is a block
    // that changed colour.
    render::ImageData far = filled(32, 16, 60, 120, 200);
    paint(far, 10, 4, 22, 12, 220, 80, 40);
    CHECK(cooker::diff_images(a, far).differing == 28);
    render::ImageData recolored = a;
    paint(recolored, 8, 4, 20, 12, 220, 120, 40);
    render::ImageData heatmap;
    const cooker::DiffResult color = cooker::diff_images(a, recolored, {}, &heatmap);
    CHECK(color.differing == 12 * 8);
    CHECK(color.fraction == doctest::Approx(96.0f / 512.0f));
    CHECK(color.max > 0.05f);
    REQUIRE(heatmap.width == 32);
    REQUIRE(heatmap.pixels.size() == 32 * 16 * 4);
    const std::uint8_t* inside = heatmap.pixels.data() + (std::size_t{6} * 32 + 10) * 4;
    CHECK(inside[0] >= 128); // red where it differs
    CHECK(inside[1] == 0);
    const std::uint8_t* outside = heatmap.pixels.data() + (std::size_t{1} * 32 + 1) * 4;
    CHECK(outside[2] == static_cast<std::uint8_t>(200 * 0.3f)); // dimmed elsewhere
    CHECK(outside[3] == 255);

    // Just under the threshold is the same; a lot of just-under is still
    // caught by the mean, which is the caller's other number.
    render::ImageData tinted = a;
    for (std::size_t i = 0; i < tinted.pixels.size(); i += 4) {
        tinted.pixels[i] = static_cast<std::uint8_t>(tinted.pixels[i] + 2);
    }
    const cooker::DiffResult tint = cooker::diff_images(a, tinted);
    CHECK(tint.differing == 0);
    CHECK(tint.mean > 0.0f);
    CHECK(tint.mean < 0.02f);

    // Different sizes, or nothing to compare, is not comparable.
    CHECK_FALSE(cooker::diff_images(a, filled(16, 16, 0, 0, 0)).comparable);
    CHECK_FALSE(cooker::diff_images(render::ImageData{}, render::ImageData{}).comparable);

    // The strip: golden, render, heatmap side by side.
    const render::ImageData strip = cooker::diff_strip(a, recolored, heatmap);
    CHECK(strip.width == 96);
    CHECK(strip.height == 16);
    CHECK(strip.pixels[(std::size_t{6} * 96 + 32 + 10) * 4 + 1] ==
          120); // the render's changed green, middle panel
    CHECK(strip.pixels[(std::size_t{6} * 96 + 64 + 10) * 4] >= 128); // the heatmap's red, right panel
}

TEST_CASE("a picture saved as a PNG loads back as the same pixels, and is the same file twice") {
    render::ImageData image = filled(7, 5, 10, 200, 30);
    paint(image, 2, 1, 5, 4, 250, 5, 90);
    image.pixels[3] = 128; // some alpha too
    const std::string path = temp_path("tynima_png_test.png");
    std::string error;
    REQUIRE_MESSAGE(cooker::save_png(path.c_str(), image, error), error);
    render::ImageData loaded;
    REQUIRE_MESSAGE(cooker::load_image_file(path.c_str(), loaded, error), error);
    CHECK(loaded.width == 7);
    CHECK(loaded.height == 5);
    CHECK(loaded.pixels == image.pixels);

    core::Bytes first, second;
    REQUIRE(platform::read_file(path.c_str(), first));
    REQUIRE(cooker::save_png(path.c_str(), image, error));
    REQUIRE(platform::read_file(path.c_str(), second));
    CHECK(first == second);

    CHECK_FALSE(cooker::save_png(path.c_str(), render::ImageData{}, error));
    CHECK(error.find("no pixels") != std::string::npos);
    // Inside a file rather than a directory: the one place no platform will
    // open a file for writing. "/no/such/dir" is only unwritable where the
    // root is, which on Windows it is not.
    CHECK_FALSE(cooker::save_png((path + "/x.png").c_str(), image, error));
    CHECK(error.find("cannot write") != std::string::npos);
    (void)platform::remove_file(path.c_str());
}
