#include <tynima/render/mesh.h>

#include <tynima/core/profile.h>

#include <algorithm>
#include <cstddef>
#include <limits>

namespace tynima::render {

const rhi::VertexLayout& vertex_layout() noexcept {
    static const rhi::VertexAttribute attributes[] = {
        {0, rhi::VertexFormat::Float3, offsetof(Vertex, position)},
        {1, rhi::VertexFormat::Float3, offsetof(Vertex, normal)},
        {2, rhi::VertexFormat::Float2, offsetof(Vertex, uv)},
    };
    static const rhi::VertexLayout layout{sizeof(Vertex), attributes, 3};
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
