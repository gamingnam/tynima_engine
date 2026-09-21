#include <tynima/render/shapes.h>

#include <cmath>
#include <initializer_list>
#include <utility>

namespace tynima::render {

using namespace tynima::math;

MeshData box_mesh(const Vec3& half_extents) {
    MeshData mesh;
    const Vec3 h = half_extents;
    struct Face {
        Vec3 normal, u_axis, v_axis;
    };
    const Face faces[6] = {
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},  // top
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},  // bottom
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},  // +x
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},  // -x
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},   // +z
        {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}, // -z
    };
    for (const Face& face : faces) {
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        // Each axis is a signed unit vector: scaling it component-wise by the
        // half extents gives the face centre and its two half-edges.
        const Vec3 origin = face.normal * h;
        const Vec3 du = face.u_axis * h;
        const Vec3 dv = face.v_axis * h;
        const float tile = 2.0f; // UV repeats per metre, so a big floor still shows texture
        const Vec2 uv_scale{length(du) * tile, length(dv) * tile};
        for (int corner = 0; corner < 4; ++corner) {
            const float su = (corner == 1 || corner == 2) ? 1.0f : -1.0f;
            const float sv = (corner >= 2) ? 1.0f : -1.0f;
            const Vec2 uv{(su + 1.0f) * 0.5f * uv_scale.x, (1.0f - sv) * 0.5f * uv_scale.y};
            mesh.vertices.push_back({.position = origin + du * su + dv * sv,
                                     .normal = face.normal,
                                     .uv = uv,
                                     .tangent = Vec4{face.u_axis, 1.0f}});
        }
        for (const std::uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) {
            mesh.indices.push_back(base + i);
        }
    }
    mesh.submeshes.push_back(
        {.first_index = 0, .index_count = static_cast<std::uint32_t>(mesh.indices.size()), .material = 0});
    mesh.compute_bounds();
    return mesh;
}

MeshData capsule_mesh(float radius, float half_height) {
    MeshData mesh;
    constexpr int kSegments = 24;
    constexpr int kRings = 6; // per cap
    // Rows of vertices from the bottom pole to the top pole: each cap has
    // kRings + 1 rows, the equator rows of the two caps being the cylinder.
    for (int cap = 0; cap < 2; ++cap) {
        for (int ring = 0; ring <= kRings; ++ring) {
            // Latitude from the pole (-90 degrees) to the equator for the
            // bottom cap, the equator to the pole for the top one.
            const float t = static_cast<float>(ring) / static_cast<float>(kRings);
            const float latitude = cap == 0 ? -kPi * 0.5f * (1.0f - t) : kPi * 0.5f * t;
            const float y = std::sin(latitude);
            const float r = std::cos(latitude);
            const float offset = cap == 0 ? -half_height : half_height;
            for (int seg = 0; seg <= kSegments; ++seg) {
                const float u = static_cast<float>(seg) / static_cast<float>(kSegments);
                const float longitude = u * kTwoPi;
                const Vec3 normal{r * std::cos(longitude), y, r * std::sin(longitude)};
                const float height = offset + y * radius; // -half_height - radius .. half_height + radius
                const float v = 1.0f - (height + half_height + radius) / (2.0f * (half_height + radius));
                const Vec4 tangent{-std::sin(longitude), 0.0f, std::cos(longitude), 1.0f};
                mesh.vertices.push_back({.position = normal * radius + Vec3{0.0f, offset, 0.0f},
                                         .normal = normal,
                                         .uv = Vec2{u, v},
                                         .tangent = tangent});
            }
        }
    }
    const auto rows = static_cast<std::uint32_t>(2 * (kRings + 1));
    const auto columns = static_cast<std::uint32_t>(kSegments + 1);
    for (std::uint32_t row = 0; row + 1 < rows; ++row) {
        for (std::uint32_t col = 0; col + 1 < columns; ++col) {
            const std::uint32_t a = row * columns + col;
            const std::uint32_t b = a + columns;
            // Counter-clockwise seen from outside (longitude runs clockwise seen from above).
            for (const std::uint32_t i : {a, b, a + 1, a + 1, b, b + 1}) {
                mesh.indices.push_back(i);
            }
        }
    }
    mesh.submeshes.push_back(
        {.first_index = 0, .index_count = static_cast<std::uint32_t>(mesh.indices.size()), .material = 0});
    mesh.compute_bounds();
    return mesh;
}

ModelData plain_model(MeshData mesh, const Vec4& color, float roughness) {
    ModelData model;
    model.mesh = std::move(mesh);
    MaterialData material;
    material.base_color_factor = color;
    material.metallic_factor = 0.0f;
    material.roughness_factor = roughness;
    model.materials.push_back(material);
    return model;
}

} // namespace tynima::render
