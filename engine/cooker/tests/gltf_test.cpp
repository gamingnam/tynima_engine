#include "sample_gltf.h"

#include <tynima/cooker/gltf.h>
#include <tynima/cooker/image.h>
#include <tynima/core/jobs.h>

#include <doctest/doctest.h>

#include <cstring>
#include <string>

using namespace tynima;
using namespace tynima::math;
using tynima::cooker::tests::kGltf;

namespace {

// The same 2x2 PNG on its own: red, green / blue, half-transparent white.
constexpr unsigned char kPng[] = {137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 2, 0, 0, 0, 2, 8, 6, 0, 0, 0, 114, 182, 13, 36, 0, 0, 0, 19, 73, 68, 65, 84, 120, 156, 99, 248, 207, 192, 240, 31, 12, 129, 52, 8, 52, 0, 0, 73, 73, 9, 120, 40, 160, 219, 119, 0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};

} // namespace

TEST_CASE("image decode yields RGBA8 rows top to bottom") {
    render::ImageData image;
    std::string error;
    REQUIRE_MESSAGE(cooker::decode_image(kPng, sizeof(kPng), image, error), error);
    CHECK(image.width == 2);
    CHECK(image.height == 2);
    REQUIRE(image.pixels.size() == 16);
    CHECK(image.pixels[0] == 255);  // top-left red
    CHECK(image.pixels[1] == 0);
    CHECK(image.pixels[5] == 255);  // top-right green
    CHECK(image.pixels[10] == 255); // bottom-left blue
    CHECK(image.pixels[15] == 128); // bottom-right alpha
    CHECK_FALSE(image.srgb);        // the decoder never decides this

    CHECK_FALSE(cooker::decode_image("not an image", 12, image, error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(cooker::load_image_file("/nonexistent.png", image, error));
}

TEST_CASE("glTF import bakes node transforms and merges primitives") {
    render::ModelData model;
    std::string error;
    REQUIRE_MESSAGE(cooker::import_gltf_memory(kGltf, std::strlen(kGltf), model, error), error);
    const render::MeshData& mesh = model.mesh;

    REQUIRE(mesh.vertices.size() == 7); // 4 + 3
    REQUIRE(mesh.indices.size() == 9);  // 6 + 3
    REQUIRE(mesh.submeshes.size() == 2);

    // Quad: translated by (1,2,3), normals untouched by a pure translation, UVs carried over.
    CHECK(mesh.vertices[0].position == Vec3{0.0f, 1.0f, 3.0f});
    CHECK(mesh.vertices[2].position == Vec3{2.0f, 3.0f, 3.0f});
    CHECK(approx_equal(mesh.vertices[0].normal, Vec3::unit_z()));
    CHECK(mesh.vertices[2].uv == Vec2{1.0f, 1.0f});
    CHECK(mesh.submeshes[0].first_index == 0);
    CHECK(mesh.submeshes[0].index_count == 6);
    CHECK(mesh.submeshes[0].material == 1);
    CHECK(mesh.indices[3] == 0);
    CHECK(mesh.indices[5] == 3);

    // Triangle: scaled by 2, indices generated and offset past the quad's vertices,
    // normals computed from the winding (counter-clockwise seen from +z).
    CHECK(mesh.vertices[5].position == Vec3{2.0f, 0.0f, 0.0f});
    CHECK(mesh.submeshes[1].first_index == 6);
    CHECK(mesh.submeshes[1].index_count == 3);
    CHECK(mesh.submeshes[1].material == 0);
    CHECK(mesh.indices[6] == 4);
    CHECK(mesh.indices[8] == 6);
    CHECK(approx_equal(mesh.vertices[4].normal, Vec3::unit_z()));
    CHECK(mesh.vertices[4].uv == Vec2::zero());

    // Bounds span both baked primitives.
    CHECK(mesh.bounds_min == Vec3{0.0f, 0.0f, 0.0f});
    CHECK(mesh.bounds_max == Vec3{2.0f, 3.0f, 3.0f});

    // Neither primitive carries TANGENT, so frames are computed: the quad's
    // u runs along +x with right-handed frames; the triangle has no UVs and
    // still gets a unit tangent perpendicular to its normal.
    CHECK(approx_equal(mesh.vertices[0].tangent.xyz(), Vec3::unit_x()));
    CHECK(mesh.vertices[0].tangent.w == 1.0f);
    CHECK(length(mesh.vertices[4].tangent.xyz()) == doctest::Approx(1.0f));
    CHECK(dot(mesh.vertices[4].tangent.xyz(), mesh.vertices[4].normal) == doctest::Approx(0.0f));
}

TEST_CASE("glTF import carries materials and decodes their images") {
    render::ModelData model;
    std::string error;
    REQUIRE_MESSAGE(cooker::import_gltf_memory(kGltf, std::strlen(kGltf), model, error), error);

    REQUIRE(model.materials.size() == 2);
    const render::MaterialData& plain = model.materials[0];
    CHECK(plain.base_color_image == -1);
    CHECK(plain.base_color_factor == Vec4{1.0f, 1.0f, 1.0f, 1.0f}); // glTF default
    CHECK(plain.emissive_factor == Vec3{1.0f, 0.0f, 0.0f});
    CHECK(plain.double_sided);

    const render::MaterialData& textured = model.materials[1];
    CHECK(textured.base_color_image == 0);
    CHECK(textured.base_color_factor.x == doctest::Approx(0.5f));
    CHECK(textured.metallic_factor == doctest::Approx(0.2f));
    CHECK(textured.roughness_factor == doctest::Approx(0.7f));
    CHECK(textured.metallic_roughness_image == 1);
    CHECK(textured.normal_image == 1);
    CHECK(textured.normal_scale == doctest::Approx(0.8f));
    CHECK(textured.occlusion_image == 1);
    CHECK(textured.occlusion_strength == doctest::Approx(0.5f));
    CHECK(textured.emissive_image == 0);
    CHECK(textured.emissive_factor == Vec3{1.0f, 1.0f, 1.0f});
    CHECK_FALSE(textured.double_sided);

    REQUIRE(model.images.size() == 2);
    const render::ImageData& color = model.images[0];
    CHECK(color.srgb); // base color and emissive: display-encoded
    CHECK(color.width == 2);
    CHECK(color.height == 2);
    REQUIRE(color.pixels.size() == 16);
    CHECK(color.pixels[0] == 255);
    CHECK(color.pixels[10] == 255);

    const render::ImageData& data_map = model.images[1];
    CHECK_FALSE(data_map.srgb); // metallic-roughness, normal, occlusion: plain data
    REQUIRE(data_map.pixels.size() == 16);
    CHECK(data_map.pixels[0] == 128); // R: occlusion
    CHECK(data_map.pixels[1] == 64);  // G: roughness
    CHECK(data_map.pixels[2] == 255); // B: metallic
}

TEST_CASE("glTF import decodes images on the job system with the same result") {
    tynima::core::JobSystem jobs;
    render::ModelData serial;
    render::ModelData parallel;
    std::string error;
    REQUIRE(cooker::import_gltf_memory(kGltf, std::strlen(kGltf), serial, error));
    REQUIRE_MESSAGE(cooker::import_gltf_memory(kGltf, std::strlen(kGltf), parallel, error, {.jobs = &jobs}), error);
    REQUIRE(parallel.images.size() == serial.images.size());
    for (std::size_t i = 0; i < serial.images.size(); ++i) {
        CHECK(parallel.images[i].width == serial.images[i].width);
        CHECK(parallel.images[i].srgb == serial.images[i].srgb);
        CHECK(parallel.images[i].pixels == serial.images[i].pixels);
    }
}

TEST_CASE("glTF import fails with a reason, not a crash") {
    render::ModelData model;
    std::string error;
    CHECK_FALSE(cooker::import_gltf_memory("not gltf", 8, model, error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(cooker::import_gltf_file("/nonexistent/model.glb", model, error));
    CHECK_FALSE(error.empty());
    const char* empty = R"({"asset":{"version":"2.0"}})";
    CHECK_FALSE(cooker::import_gltf_memory(empty, std::strlen(empty), model, error));
    CHECK(error == "no triangle geometry found");
}
