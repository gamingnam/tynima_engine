#include "sample_gltf.h"

#include <tynima/cooker/cook.h>
#include <tynima/cooker/gltf.h>
#include <tynima/cooker/mipmaps.h>
#include <tynima/platform/file.h>
#include <tynima/platform/time.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>


using namespace tynima;
using namespace tynima::math;
using tynima::cooker::tests::kGltf;

namespace {

// Where this machine puts temporary files. std::filesystem asks the OS —
// TMPDIR where it is set, the user's Temp on Windows — rather than assuming
// "/tmp", which on Windows names a directory on whatever drive the test
// happens to run from and usually is not there at all.
std::string temp_dir() {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) {
        dir = "/tmp";
    }
    // No trailing separator, so a caller may append one of its own.
    std::string out = dir.lexically_normal().generic_string();
    while (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

// The triangles of a mesh as position triples, sorted: what must survive any
// reordering of vertices and indices.
using Triangle = std::array<float, 9>;
std::vector<Triangle> triangles_of(const render::MeshData& mesh, const render::Submesh& sub) {
    std::vector<Triangle> out;
    for (std::uint32_t i = sub.first_index; i + 2 < sub.first_index + sub.index_count; i += 3) {
        Triangle t{};
        // Rotate each triangle so its lowest vertex comes first: the same
        // triangle, whichever of its three starts it in the index list.
        std::array<Vec3, 3> p{mesh.vertices[mesh.indices[i]].position,
                              mesh.vertices[mesh.indices[i + 1]].position,
                              mesh.vertices[mesh.indices[i + 2]].position};
        const auto less = [](const Vec3& a, const Vec3& b) {
            return a.x != b.x ? a.x < b.x : a.y != b.y ? a.y < b.y : a.z < b.z;
        };
        std::size_t first = 0;
        for (std::size_t k = 1; k < 3; ++k) {
            if (less(p[k], p[first])) {
                first = k;
            }
        }
        for (std::size_t k = 0; k < 3; ++k) {
            const Vec3& v = p[(first + k) % 3];
            t[k * 3] = v.x;
            t[k * 3 + 1] = v.y;
            t[k * 3 + 2] = v.z;
        }
        out.push_back(t);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void touch_later(const std::string& path, const char* text) {
    const std::uint64_t before = platform::file_write_time(path.c_str());
    for (int attempt = 0; attempt < 1000; ++attempt) {
        REQUIRE(platform::write_file(path.c_str(), text, std::strlen(text)));
        if (platform::file_write_time(path.c_str()) > before) {
            return;
        }
        platform::sleep_ns(1'000'000);
    }
    FAIL("the file's write time never moved");
}

} // namespace

TEST_CASE("sRGB bytes round-trip through the mip filter's table, and average in linear light") {
    for (int b = 0; b < 256; ++b) {
        CHECK(cooker::linear_to_srgb(cooker::srgb_to_linear(static_cast<std::uint8_t>(b))) == b);
    }
    CHECK(cooker::srgb_to_linear(0) == 0.0f);
    CHECK(cooker::srgb_to_linear(255) == 1.0f);
    CHECK(cooker::srgb_to_linear(128) == doctest::Approx(0.2158605f));
    CHECK(cooker::linear_to_srgb(-1.0f) == 0);
    CHECK(cooker::linear_to_srgb(2.0f) == 255);
    CHECK(cooker::linear_to_srgb(0.5f) == 188); // the byte nearest linear 0.5, not 128

    // Black and white, side by side: half in linear light is 188 in sRGB, and
    // 128 as plain data. Alpha averages plainly either way.
    const std::uint8_t pair[8] = {0, 0, 0, 255, 255, 255, 255, 0};
    std::uint8_t out[4];
    cooker::downsample_2x2(pair, 2, 1, true, out);
    CHECK(out[0] == 188);
    CHECK(out[1] == 188);
    CHECK(out[2] == 188);
    CHECK(out[3] == 128);
    cooker::downsample_2x2(pair, 2, 1, false, out);
    CHECK(out[0] == 128);
    CHECK(out[3] == 128);
    // An odd width loses its last column, as on the GPU: 3x1 of 10, 20, 30 becomes 15.
    const std::uint8_t three[12] = {10, 10, 10, 10, 20, 20, 20, 20, 30, 30, 30, 30};
    std::uint8_t two[4];
    cooker::downsample_2x2(three, 3, 1, false, two);
    CHECK(two[0] == 15);
}

TEST_CASE("a mip chain runs down to one texel, each level the box filter of the one above") {
    render::ImageData image;
    image.width = 4;
    image.height = 2;
    image.srgb = false;
    for (std::uint32_t i = 0; i < 8; ++i) {
        const auto v = static_cast<std::uint8_t>(i * 30);
        image.pixels.insert(image.pixels.end(), {v, v, v, 255});
    }
    assets::CookedImage cooked;
    cooker::build_mip_chain(image, true, cooked);
    CHECK(cooked.width == 4);
    CHECK(cooked.height == 2);
    CHECK(cooked.mip_count == 3);
    CHECK_FALSE(cooked.srgb);
    REQUIRE(cooked.levels.size() == assets::mip_chain_size(4, 2, 3));
    CHECK(std::equal(image.pixels.begin(), image.pixels.end(), cooked.levels.begin()));
    // Level 1 is 2x1: the left 2x2 block averages (0, 30, 120, 150) = 75; the right (60, 90, 180, 210) = 135.
    const std::uint8_t* level1 = cooked.levels.data() + assets::mip_level_offset(cooked, 1);
    CHECK(level1[0] == 75);
    CHECK(level1[4] == 135);
    // Level 2 is 1x1: (75 + 135) / 2 = 105.
    const std::uint8_t* level2 = cooked.levels.data() + assets::mip_level_offset(cooked, 2);
    CHECK(level2[0] == 105);
    CHECK(level2[3] == 255);

    cooker::build_mip_chain(image, false, cooked);
    CHECK(cooked.mip_count == 1);
    CHECK(cooked.levels.size() == 32);

    render::ImageData empty;
    cooker::build_mip_chain(empty, true, cooked);
    CHECK(cooked.levels.empty());
}

TEST_CASE("cooking a model keeps its triangles, materials and images, in a GPU-friendlier order") {
    render::ModelData data;
    std::string error;
    REQUIRE_MESSAGE(cooker::import_gltf_memory(kGltf, std::strlen(kGltf), data, error), error);
    // Two identical vertices, so there is something to weld.
    data.mesh.vertices.push_back(data.mesh.vertices[4]);
    data.mesh.vertices.push_back(data.mesh.vertices[5]);
    data.mesh.vertices.push_back(data.mesh.vertices[6]);
    const auto base = static_cast<std::uint32_t>(data.mesh.vertices.size()) - 3;
    data.mesh.indices.insert(data.mesh.indices.end(), {base, base + 1, base + 2});
    data.mesh.submeshes[1].index_count += 3; // the triangle twice
    data.mesh.vertices.push_back(data.mesh.vertices[0]);
    data.mesh.vertices.back().position = Vec3{100.0f, 100.0f, 100.0f}; // used by nothing
    data.mesh.compute_bounds();
    CHECK(data.mesh.bounds_max.x == 100.0f);

    assets::CookedModel cooked;
    cooker::cook_model_data(data, {}, cooked);
    REQUIRE(cooked.mesh.submeshes.size() == 2);
    CHECK(cooked.mesh.submeshes[0].index_count == 6);
    CHECK(cooked.mesh.submeshes[1].index_count == 6);
    CHECK(cooked.mesh.indices.size() == 12);
    CHECK(cooked.mesh.vertices.size() == 7); // the copies welded, the stray dropped
    CHECK(cooked.mesh.bounds_max == Vec3{2.0f, 3.0f, 3.0f});
    for (std::size_t s = 0; s < 2; ++s) {
        CHECK(triangles_of(cooked.mesh, cooked.mesh.submeshes[s]) ==
              triangles_of(data.mesh, data.mesh.submeshes[s]));
    }
    for (const std::uint32_t index : cooked.mesh.indices) {
        CHECK(index < cooked.mesh.vertices.size());
    }
    REQUIRE(cooked.materials.size() == 2);
    CHECK(cooked.materials[1].base_color_image == 0);
    CHECK(cooked.materials[1].normal_image == 1);
    REQUIRE(cooked.images.size() == 2);
    CHECK(cooked.images[0].srgb);
    CHECK(cooked.images[0].mip_count == 2); // 2x2 then 1x1
    CHECK(cooked.images[0].levels.size() == 20);
    CHECK_FALSE(cooked.images[1].srgb);

    // Without the ordering, the vertices are the source's.
    cooker::cook_model_data(data, {.optimize_mesh = false, .mipmaps = false}, cooked);
    CHECK(cooked.mesh.vertices.size() == data.mesh.vertices.size());
    CHECK(cooked.mesh.indices == data.mesh.indices);
    CHECK(cooked.images[0].mip_count == 1);

    // An image that never decoded is left out, and the materials know.
    data.images[1].pixels.clear();
    cooker::cook_model_data(data, {}, cooked);
    REQUIRE(cooked.images.size() == 1);
    CHECK(cooked.materials[1].base_color_image == 0);
    CHECK(cooked.materials[1].normal_image == -1);
    CHECK(cooked.materials[1].metallic_roughness_image == -1);
}

TEST_CASE("a source file cooks to a blob beside it, and cooks again only when it is newer") {
    const std::string dir = temp_dir() + "/tynima_cook_test";
    REQUIRE(platform::make_directories(dir.c_str()));
    const std::string source = dir + "/sample.gltf";
    REQUIRE(platform::write_file(source.c_str(), kGltf, std::strlen(kGltf)));
    CHECK(cooker::is_model_source(source.c_str()));
    CHECK(cooker::is_model_source("Model.GLB"));
    CHECK_FALSE(cooker::is_model_source("model.tymodel"));
    CHECK(cooker::is_cooked_model("model.tymodel"));
    CHECK(cooker::cooked_path_for("assets/models/bottle.glb") == "assets/models/.cooked/bottle.tymodel");
    CHECK(cooker::cooked_path_for("bottle.glb") == ".cooked/bottle.tymodel");
    CHECK(cooker::cooked_path_for("assets/bottle.glb", "out") == "out/bottle.tymodel");
    CHECK(cooker::cooked_path_for("assets/bottle.glb", "out/") == "out/bottle.tymodel");

    std::string cooked, error;
    bool cooked_now = false;
    REQUIRE_MESSAGE(cooker::ensure_cooked(source.c_str(), nullptr, {}, cooked, error, &cooked_now), error);
    CHECK(cooked_now);
    CHECK(cooked == dir + "/.cooked/sample.tymodel");
    CHECK(cooker::cooked_is_current(source.c_str(), cooked.c_str()));
    assets::CookedModel model;
    REQUIRE_MESSAGE(assets::load_model_blob_file(cooked.c_str(), model, error), error);
    CHECK(model.mesh.indices.size() == 9);
    CHECK(model.images.size() == 2);

    // Current: nothing happens. Touched: cooked again. Deleted: cooked again.
    REQUIRE(cooker::ensure_cooked(source.c_str(), nullptr, {}, cooked, error, &cooked_now));
    CHECK_FALSE(cooked_now);
    touch_later(source, kGltf);
    CHECK_FALSE(cooker::cooked_is_current(source.c_str(), cooked.c_str()));
    REQUIRE(cooker::ensure_cooked(source.c_str(), nullptr, {}, cooked, error, &cooked_now));
    CHECK(cooked_now);
    REQUIRE(platform::remove_file(cooked.c_str()));
    REQUIRE(cooker::ensure_cooked(source.c_str(), nullptr, {}, cooked, error, &cooked_now));
    CHECK(cooked_now);

    // Somewhere else, by name; and the failures.
    const std::string elsewhere = dir + "/out/deeper";
    REQUIRE(cooker::cook_model_file(source.c_str(), (elsewhere + "/sample.tymodel").c_str(), {}, error));
    CHECK(assets::model_blob_version((elsewhere + "/sample.tymodel").c_str()) == assets::kModelBlobVersion);
    CHECK_FALSE(
        cooker::cook_model_file((dir + "/missing.glb").c_str(), (dir + "/x.tymodel").c_str(), {}, error));
    CHECK_FALSE(error.empty());
    // Inside a file rather than a directory: the one place no platform can
    // make a directory. "/no/such/dir" is only unwritable where the root
    // is — on Windows it names a directory on the current drive, and the
    // cooker would cheerfully make it.
    CHECK_FALSE(cooker::cook_model_file(source.c_str(), (source + "/x.tymodel").c_str(), {}, error));
    CHECK(error.find("cannot make") != std::string::npos);
    CHECK_FALSE(cooker::cook_model_file(cooked.c_str(), (dir + "/x.tymodel").c_str(), {}, error));
    CHECK(error.find("not a model the cooker reads") != std::string::npos);
    CHECK_FALSE(cooker::ensure_cooked((dir + "/missing.glb").c_str(), nullptr, {}, cooked, error));
    CHECK(error.find("cannot read") != std::string::npos);

    (void)platform::remove_file((elsewhere + "/sample.tymodel").c_str());
    (void)platform::remove_file((dir + "/.cooked/sample.tymodel").c_str());
    (void)platform::remove_file(source.c_str());
}
