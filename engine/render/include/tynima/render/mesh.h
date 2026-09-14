#pragma once

#include <tynima/core/math.h>
#include <tynima/rhi/device.h>

#include <cstdint>
#include <vector>

namespace tynima::render {

// The one static-mesh vertex format. Interleaved and tightly packed: exactly
// what the vertex buffer holds and what the pipeline's VertexLayout describes.
// tangent.xyz points along +u on the surface; tangent.w is the handedness
// (glTF's convention): bitangent = cross(normal, tangent.xyz) * tangent.w.
struct Vertex {
    math::Vec3 position;
    math::Vec3 normal;
    math::Vec2 uv;
    math::Vec4 tangent;
};
static_assert(sizeof(Vertex) == 48, "Vertex layout is assumed by vertex_layout() and the shaders");

// The layout of Vertex for pipeline creation: attribute 0 position, 1 normal, 2 uv, 3 tangent.
[[nodiscard]] const rhi::VertexLayout& vertex_layout() noexcept;

// One draw's worth of indices, and which material it uses.
struct Submesh {
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    std::uint32_t material = 0;
};

// CPU-side mesh: what an importer produces and the cooker will serialize.
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Submesh> submeshes;
    math::Vec3 bounds_min{0.0f};
    math::Vec3 bounds_max{0.0f};

    // Face-weighted vertex normals from a range of the triangle list (all of
    // it by default), for geometry that arrives without any. Only vertices
    // referenced by that range are touched.
    void compute_normals(std::uint32_t first_index = 0, std::uint32_t index_count = 0xFFFFFFFFu);
    // Tangent frames from the UV layout of a range of the triangle list (all
    // of it by default), for geometry that arrives without any. Needs normals
    // and UVs; a vertex whose UVs are degenerate gets an arbitrary
    // perpendicular tangent so the frame is always usable.
    void compute_tangents(std::uint32_t first_index = 0, std::uint32_t index_count = 0xFFFFFFFFu);
    void compute_bounds();
};

// GPU-side mesh: buffers owned by the Device that uploaded them.
struct Mesh {
    rhi::BufferHandle vertex_buffer;
    rhi::BufferHandle index_buffer;
    std::uint32_t index_count = 0;
    std::vector<Submesh> submeshes;
    math::Vec3 bounds_min{0.0f};
    math::Vec3 bounds_max{0.0f};
};

// Uploads `data` into fresh buffers. false on failure (platform::last_error() says why).
[[nodiscard]] bool upload_mesh(rhi::Device& device, const MeshData& data, Mesh& out) noexcept;
void destroy_mesh(rhi::Device& device, Mesh& mesh) noexcept;

// Binds the vertex and index buffers; draw submeshes with pass.draw_indexed().
void bind_mesh(rhi::RenderPass& pass, const Mesh& mesh) noexcept;

// Binds the buffers and issues one indexed draw per submesh. The pipeline
// and uniforms are the caller's business.
void draw_mesh(rhi::RenderPass& pass, const Mesh& mesh) noexcept;

} // namespace tynima::render
