#include <tynima/render/mesh.h>

#include <tynima/core/profile.h>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <vector>

namespace tynima::render {

const rhi::VertexLayout& vertex_layout() noexcept {
    static const rhi::VertexAttribute attributes[] = {
        {0, rhi::VertexFormat::Float3, offsetof(Vertex, position)},
        {1, rhi::VertexFormat::Float3, offsetof(Vertex, normal)},
        {2, rhi::VertexFormat::Float2, offsetof(Vertex, uv)},
        {3, rhi::VertexFormat::Float4, offsetof(Vertex, tangent)},
    };
    static const rhi::VertexLayout layout{sizeof(Vertex), attributes, 4};
    return layout;
}

void MeshData::compute_normals(std::uint32_t first_index, std::uint32_t index_count) {
    const std::size_t begin = first_index;
    const std::size_t end = index_count == 0xFFFFFFFFu ? indices.size() : std::min<std::size_t>(indices.size(), begin + index_count);
    for (std::size_t i = begin; i < end; ++i) {
        vertices[indices[i]].normal = math::Vec3::zero();
    }
    // Accumulating the unnormalized face normal weights each face by its
    // area, which is the usual "good enough" smoothing for imported meshes.
    for (std::size_t i = begin; i + 2 < end; i += 3) {
        Vertex& a = vertices[indices[i]];
        Vertex& b = vertices[indices[i + 1]];
        Vertex& c = vertices[indices[i + 2]];
        const math::Vec3 n = math::cross(b.position - a.position, c.position - a.position);
        a.normal += n;
        b.normal += n;
        c.normal += n;
    }
    for (std::size_t i = begin; i < end; ++i) {
        Vertex& v = vertices[indices[i]];
        v.normal = math::normalize(v.normal);
    }
}

void MeshData::compute_tangents(std::uint32_t first_index, std::uint32_t index_count) {
    const std::size_t begin = first_index;
    const std::size_t end = index_count == 0xFFFFFFFFu ? indices.size() : std::min<std::size_t>(indices.size(), begin + index_count);

    // Lengyel's method: per triangle, solve the 2x2 system that maps UV
    // deltas to position deltas, giving the directions along which u and v
    // increase; accumulate per vertex, then orthogonalize against the normal.
    std::vector<math::Vec3> tan_u(vertices.size(), math::Vec3::zero());
    std::vector<math::Vec3> tan_v(vertices.size(), math::Vec3::zero());
    for (std::size_t i = begin; i + 2 < end; i += 3) {
        const std::uint32_t i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];
        const Vertex& v0 = vertices[i0];
        const Vertex& v1 = vertices[i1];
        const Vertex& v2 = vertices[i2];
        const math::Vec3 e1 = v1.position - v0.position;
        const math::Vec3 e2 = v2.position - v0.position;
        const math::Vec2 d1 = v1.uv - v0.uv;
        const math::Vec2 d2 = v2.uv - v0.uv;
        const float det = d1.x * d2.y - d2.x * d1.y;
        if (math::approx_equal(det, 0.0f, 1e-12f)) {
            continue; // degenerate UVs: this triangle says nothing about the frame
        }
        const float r = 1.0f / det;
        const math::Vec3 sdir = (e1 * d2.y - e2 * d1.y) * r;
        const math::Vec3 tdir = (e2 * d1.x - e1 * d2.x) * r;
        for (const std::uint32_t idx : {i0, i1, i2}) {
            tan_u[idx] += sdir;
            tan_v[idx] += tdir;
        }
    }
    for (std::size_t i = begin; i < end; ++i) {
        const std::uint32_t idx = indices[i];
        Vertex& v = vertices[idx];
        const math::Vec3 n = v.normal;
        math::Vec3 t = tan_u[idx] - n * math::dot(n, tan_u[idx]); // Gram-Schmidt against the normal
        float w = 1.0f;
        if (math::length_squared(t) < 1e-12f) {
            // No usable UV information: any perpendicular will do.
            t = math::cross(n, math::Vec3::unit_x());
            if (math::length_squared(t) < 1e-6f) {
                t = math::cross(n, math::Vec3::unit_y());
            }
        } else {
            w = math::dot(math::cross(n, t), tan_v[idx]) < 0.0f ? -1.0f : 1.0f;
        }
        v.tangent = {math::normalize(t), w};
    }
}

void MeshData::compute_bounds() {
    if (vertices.empty()) {
        bounds_min = bounds_max = math::Vec3::zero();
        return;
    }
    bounds_min = math::Vec3{std::numeric_limits<float>::max()};
    bounds_max = math::Vec3{std::numeric_limits<float>::lowest()};
    for (const Vertex& v : vertices) {
        bounds_min = math::min(bounds_min, v.position);
        bounds_max = math::max(bounds_max, v.position);
    }
}

bool upload_mesh(rhi::Device& device, const MeshData& data, Mesh& out) noexcept {
    TY_PROFILE_SCOPE_NAMED("render::upload_mesh");
    destroy_mesh(device, out);
    if (data.vertices.empty() || data.indices.empty()) {
        return false;
    }
    out.vertex_buffer = device.create_buffer_with_data(
        rhi::BufferUsage::Vertex, data.vertices.data(), static_cast<std::uint32_t>(data.vertices.size() * sizeof(Vertex)));
    out.index_buffer = device.create_buffer_with_data(
        rhi::BufferUsage::Index, data.indices.data(),
        static_cast<std::uint32_t>(data.indices.size() * sizeof(std::uint32_t)));
    if (out.vertex_buffer == nullptr || out.index_buffer == nullptr) {
        destroy_mesh(device, out);
        return false;
    }
    out.index_count = static_cast<std::uint32_t>(data.indices.size());
    out.submeshes = data.submeshes;
    if (out.submeshes.empty()) {
        out.submeshes.push_back({0, out.index_count, 0});
    }
    out.bounds_min = data.bounds_min;
    out.bounds_max = data.bounds_max;
    return true;
}

void destroy_mesh(rhi::Device& device, Mesh& mesh) noexcept {
    device.destroy_buffer(mesh.vertex_buffer);
    device.destroy_buffer(mesh.index_buffer);
    mesh = Mesh{};
}

void bind_mesh(rhi::RenderPass& pass, const Mesh& mesh) noexcept {
    if (mesh.vertex_buffer != nullptr && mesh.index_buffer != nullptr) {
        pass.bind_vertex_buffer(*mesh.vertex_buffer);
        pass.bind_index_buffer(*mesh.index_buffer, rhi::IndexType::Uint32);
    }
}

void draw_mesh(rhi::RenderPass& pass, const Mesh& mesh) noexcept {
    if (mesh.vertex_buffer == nullptr || mesh.index_buffer == nullptr) {
        return;
    }
    bind_mesh(pass, mesh);
    for (const Submesh& sub : mesh.submeshes) {
        pass.draw_indexed(sub.index_count, sub.first_index);
    }
}

} // namespace tynima::render
