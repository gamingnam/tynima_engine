#include <tynima/assets/model_blob.h>
#include <tynima/platform/file.h>
#include <tynima/rhi/device.h>

#include <doctest/doctest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <string>

using namespace tynima;
using namespace tynima::math;

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

// A model with a little of everything: two submeshes over six vertices, two
// materials, and two images — a 4x4 sRGB one with its three levels and a
// 2x1 data one with two.
assets::CookedModel make_model() {
    assets::CookedModel model;
    for (std::uint32_t i = 0; i < 6; ++i) {
        render::Vertex v;
        v.position = Vec3{static_cast<float>(i), 0.5f * static_cast<float>(i), -1.0f};
        v.normal = Vec3::unit_y();
        v.uv = Vec2{0.25f * static_cast<float>(i), 1.0f};
        v.tangent = Vec4{1.0f, 0.0f, 0.0f, -1.0f};
        model.mesh.vertices.push_back(v);
    }
    model.mesh.indices = {0, 1, 2, 2, 1, 3, 3, 4, 5};
    model.mesh.submeshes = {{.first_index = 0, .index_count = 6, .material = 1},
                            {.first_index = 6, .index_count = 3, .material = 0}};
    model.mesh.compute_bounds();

    render::MaterialData plain;
    plain.emissive_factor = Vec3{1.0f, 0.5f, 0.25f};
    plain.double_sided = true;
    render::MaterialData textured;
    textured.base_color_factor = Vec4{0.5f, 1.0f, 1.0f, 1.0f};
    textured.base_color_image = 0;
    textured.metallic_factor = 0.2f;
    textured.roughness_factor = 0.7f;
    textured.metallic_roughness_image = 1;
    textured.normal_image = 1;
    textured.normal_scale = 0.8f;
    textured.occlusion_image = 1;
    textured.occlusion_strength = 0.5f;
    textured.emissive_image = 0;
    model.materials = {plain, textured};

    assets::CookedImage color;
    color.width = color.height = 4;
    color.mip_count = 3;
    color.srgb = true;
    color.levels.resize(assets::mip_chain_size(4, 4, 3));
    for (std::size_t i = 0; i < color.levels.size(); ++i) {
        color.levels[i] = static_cast<std::uint8_t>(i * 7);
    }
    assets::CookedImage data;
    data.width = 2;
    data.height = 1;
    data.mip_count = 2;
    const std::uint8_t data_levels[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    data.levels.assign(data_levels, sizeof data_levels);
    model.images = {color, data};
    return model;
}

} // namespace

TEST_CASE("mip arithmetic: extents halve to one, offsets and sizes follow") {
    CHECK(assets::full_mip_count(1, 1) == 1);
    CHECK(assets::full_mip_count(4, 4) == 3);
    CHECK(assets::full_mip_count(256, 16) == 9);
    CHECK(assets::full_mip_count(5, 3) == 3); // 5x3, 2x1, 1x1
    for (const std::uint32_t extent : {1u, 2u, 3u, 7u, 64u, 1000u, 2048u}) {
        CHECK(assets::full_mip_count(extent, 1) == rhi::mip_level_count(extent, 1)); // the device's own count
        CHECK(assets::full_mip_count(4, extent) == rhi::mip_level_count(4, extent));
    }
    CHECK(assets::mip_extent(5, 1) == 2);
    CHECK(assets::mip_extent(5, 2) == 1);
    CHECK(assets::mip_extent(5, 9) == 1);
    CHECK(assets::mip_chain_size(4, 4, 3) == (16 + 4 + 1) * 4);
    assets::CookedImage image;
    image.width = 5;
    image.height = 3;
    image.mip_count = 3;
    CHECK(assets::mip_level_size(image, 0) == 60);
    CHECK(assets::mip_level_size(image, 1) == 8);
    CHECK(assets::mip_level_size(image, 2) == 4);
    CHECK(assets::mip_level_offset(image, 0) == 0);
    CHECK(assets::mip_level_offset(image, 1) == 60);
    CHECK(assets::mip_level_offset(image, 2) == 68);
    CHECK(assets::mip_chain_size(5, 3, 3) == 72);
}

TEST_CASE("a cooked model is written to bytes and read back the same, its sections aligned") {
    const assets::CookedModel model = make_model();
    core::Bytes bytes;
    std::string error;
    REQUIRE_MESSAGE(assets::write_model_blob(model, bytes, error), error);
    CHECK(std::memcmp(bytes.data(), "TYMD", 4) == 0);
    // The header is 64 bytes and the vertices follow it directly: the first
    // vertex's position is the first thing after the header.
    float x = 0.0f;
    std::memcpy(&x, bytes.data() + 64, sizeof x);
    CHECK(x == 0.0f);
    std::memcpy(&x, bytes.data() + 64 + 48, sizeof x);
    CHECK(x == 1.0f);

    assets::CookedModel loaded;
    REQUIRE_MESSAGE(assets::read_model_blob(bytes.data(), bytes.size(), loaded, error), error);
    REQUIRE(loaded.mesh.vertices.size() == 6);
    CHECK(std::memcmp(loaded.mesh.vertices.data(), model.mesh.vertices.data(), 6 * sizeof(render::Vertex)) ==
          0);
    CHECK(loaded.mesh.indices == model.mesh.indices);
    REQUIRE(loaded.mesh.submeshes.size() == 2);
    CHECK(loaded.mesh.submeshes[1].first_index == 6);
    CHECK(loaded.mesh.submeshes[0].material == 1);
    CHECK(loaded.mesh.bounds_min == model.mesh.bounds_min);
    CHECK(loaded.mesh.bounds_max == Vec3{5.0f, 2.5f, -1.0f});
    REQUIRE(loaded.materials.size() == 2);
    CHECK(loaded.materials[0].double_sided);
    CHECK(loaded.materials[0].emissive_factor == Vec3{1.0f, 0.5f, 0.25f});
    CHECK(loaded.materials[0].base_color_image == -1);
    CHECK_FALSE(loaded.materials[1].double_sided);
    CHECK(loaded.materials[1].base_color_factor == Vec4{0.5f, 1.0f, 1.0f, 1.0f});
    CHECK(loaded.materials[1].metallic_factor == 0.2f);
    CHECK(loaded.materials[1].roughness_factor == 0.7f);
    CHECK(loaded.materials[1].normal_scale == 0.8f);
    CHECK(loaded.materials[1].occlusion_strength == 0.5f);
    CHECK(loaded.materials[1].metallic_roughness_image == 1);
    CHECK(loaded.materials[1].emissive_image == 0);
    REQUIRE(loaded.images.size() == 2);
    CHECK(loaded.images[0].srgb);
    CHECK(loaded.images[0].mip_count == 3);
    CHECK(loaded.images[0].levels == model.images[0].levels);
    CHECK_FALSE(loaded.images[1].srgb);
    CHECK(loaded.images[1].width == 2);
    CHECK(loaded.images[1].levels == model.images[1].levels);

    // Written again from what was read: the same bytes.
    core::Bytes again;
    REQUIRE(assets::write_model_blob(loaded, again, error));
    CHECK(again == bytes);
}

TEST_CASE("a blob that is short, edited or of another version is refused with a reason") {
    core::Bytes bytes;
    std::string error;
    REQUIRE(assets::write_model_blob(make_model(), bytes, error));
    assets::CookedModel out;
    const auto refused = [&](const core::Bytes& file, const char* reason) {
        std::string why;
        CHECK_FALSE(assets::read_model_blob(file.data(), file.size(), out, why));
        CHECK_MESSAGE(why.find(reason) != std::string::npos, why);
        CHECK(out.mesh.vertices.empty()); // nothing half-loaded
    };
    refused(core::Bytes(bytes.data(), 20), "too short");
    refused(core::Bytes(bytes.data(), bytes.size() - 16), "truncated");
    core::Bytes wrong = bytes;
    wrong[0] = 'X';
    refused(wrong, "not a model blob");
    wrong = bytes;
    wrong[4] = 99; // the version
    refused(wrong, "version 99");
    wrong = bytes;
    wrong[8] = 200; // vertex_count: the sections no longer fit
    refused(wrong, "do not fit");
    wrong = bytes;
    wrong[64 + 6 * 48] = 77; // the first index, past six vertices
    refused(wrong, "index 0 is 77");
    // A model that does not fit the format never becomes bytes at all.
    assets::CookedModel bad = make_model();
    bad.images[1].levels.resize(bad.images[1].levels.size() - 1);
    CHECK_FALSE(assets::write_model_blob(bad, bytes, error));
    CHECK(error.find("image 1") != std::string::npos);
    bad = make_model();
    bad.mesh.submeshes[1].index_count = 30;
    CHECK_FALSE(assets::write_model_blob(bad, bytes, error));
    CHECK(error.find("submesh 1") != std::string::npos);
    bad = make_model();
    bad.images[0].mip_count = 4;
    CHECK_FALSE(assets::write_model_blob(bad, bytes, error));
    CHECK(error.find("mip levels") != std::string::npos);
}

TEST_CASE("a blob is saved whole to disk, read back, and its version read from the header alone") {
    const std::string path = temp_path("tynima_blob_test.tymodel");
    std::string error;
    REQUIRE_MESSAGE(assets::save_model_blob(make_model(), path.c_str(), error), error);
    CHECK(platform::file_write_time((path + "." + std::to_string(platform::process_id()) + ".tmp").c_str()) ==
          0); // renamed away
    CHECK(assets::model_blob_version(path.c_str()) == assets::kModelBlobVersion);
    assets::CookedModel loaded;
    REQUIRE_MESSAGE(assets::load_model_blob_file(path.c_str(), loaded, error), error);
    CHECK(loaded.mesh.indices.size() == 9);
    CHECK(loaded.images.size() == 2);
    CHECK_FALSE(assets::load_model_blob_file("/no/such/model.tymodel", loaded, error));
    CHECK(error.find("cannot read") != std::string::npos);
    CHECK(assets::model_blob_version("/no/such/model.tymodel") == 0);
    REQUIRE(platform::write_file(path.c_str(), "hello", 5));
    CHECK(assets::model_blob_version(path.c_str()) == 0);
    CHECK_FALSE(assets::save_model_blob(make_model(), "/no/such/dir/model.tymodel", error));
    CHECK(error.find("cannot write") != std::string::npos);
    (void)platform::remove_file(path.c_str());
}
