#include <tynima/render/shapes.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

using namespace tynima;
using namespace tynima::math;

TEST_CASE("a box mesh is six hard-edged quads around the origin") {
    const render::MeshData box = render::box_mesh(Vec3{1.0f, 2.0f, 3.0f});
    REQUIRE(box.vertices.size() == 24);
    REQUIRE(box.indices.size() == 36);
    REQUIRE(box.submeshes.size() == 1);
    CHECK(box.submeshes[0].index_count == 36);
    CHECK(box.bounds_min == Vec3{-1.0f, -2.0f, -3.0f});
    CHECK(box.bounds_max == Vec3{1.0f, 2.0f, 3.0f});
    for (const render::Vertex& v : box.vertices) {
        // Every normal is a signed axis, the vertex lies on that face, and the
        // tangent runs along the face.
        CHECK(length(v.normal) == doctest::Approx(1.0f));
        CHECK(std::abs(dot(v.position, v.normal)) ==
              doctest::Approx(std::abs(dot(Vec3{1.0f, 2.0f, 3.0f}, v.normal))));
        CHECK(dot(v.normal, v.tangent.xyz()) == doctest::Approx(0.0f));
        CHECK(v.tangent.w == 1.0f);
    }
    for (const std::uint32_t index : box.indices) {
        CHECK(index < 24);
    }
    // Counter-clockwise seen from outside: each triangle's winding agrees with its normal.
    for (std::size_t i = 0; i < box.indices.size(); i += 3) {
        const render::Vertex& a = box.vertices[box.indices[i]];
        const render::Vertex& b = box.vertices[box.indices[i + 1]];
        const render::Vertex& c = box.vertices[box.indices[i + 2]];
        CHECK(dot(cross(b.position - a.position, c.position - a.position), a.normal) > 0.0f);
    }
}

TEST_CASE("a capsule mesh is round, closed, and a sphere when it has no middle") {
    const render::MeshData capsule = render::capsule_mesh(0.5f, 1.0f);
    REQUIRE(capsule.submeshes.size() == 1);
    CHECK(capsule.bounds_min == Vec3{-0.5f, -1.5f, -0.5f});
    CHECK(capsule.bounds_max == Vec3{0.5f, 1.5f, 0.5f});
    for (const render::Vertex& v : capsule.vertices) {
        CHECK(length(v.normal) == doctest::Approx(1.0f));
        CHECK(std::abs(dot(v.normal, v.tangent.xyz())) < 1e-4f);
        // Every vertex is `radius` from the axis segment between the two cap centres.
        const float y = std::clamp(v.position.y, -1.0f, 1.0f);
        CHECK(length(v.position - Vec3{0.0f, y, 0.0f}) == doctest::Approx(0.5f).epsilon(1e-3f));
        CHECK(v.uv.x >= 0.0f);
        CHECK(v.uv.x <= 1.0f);
    }
    for (std::size_t i = 0; i < capsule.indices.size(); i += 3) {
        const render::Vertex& a = capsule.vertices[capsule.indices[i]];
        const render::Vertex& b = capsule.vertices[capsule.indices[i + 1]];
        const render::Vertex& c = capsule.vertices[capsule.indices[i + 2]];
        REQUIRE(capsule.indices[i + 2] < capsule.vertices.size());
        CHECK(dot(cross(b.position - a.position, c.position - a.position), a.normal) >= -1e-6f);
    }
    const render::MeshData sphere = render::capsule_mesh(2.0f, 0.0f);
    CHECK(sphere.bounds_min == Vec3{-2.0f});
    CHECK(sphere.bounds_max == Vec3{2.0f});
    for (const render::Vertex& v : sphere.vertices) {
        CHECK(length(v.position) == doctest::Approx(2.0f).epsilon(1e-3f));
    }

    const render::ModelData model =
        render::plain_model(render::box_mesh(Vec3{1.0f}), Vec4{0.2f, 0.4f, 0.6f, 1.0f}, 0.7f);
    CHECK(model.mesh.vertices.size() == 24);
    REQUIRE(model.materials.size() == 1);
    CHECK(model.materials[0].base_color_factor == Vec4{0.2f, 0.4f, 0.6f, 1.0f});
    CHECK(model.materials[0].roughness_factor == 0.7f);
    CHECK(model.materials[0].metallic_factor == 0.0f);
    CHECK(model.images.empty());
}
