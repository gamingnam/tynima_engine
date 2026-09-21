#pragma once

#include <tynima/render/model.h>

// Meshes made from numbers: what a test scene, a placeholder and the
// sandbox's floor, character and gate are drawn with. Each comes with
// normals, UVs and tangents, one submesh, bounds computed, ready for
// upload_model() or the cooker.
namespace tynima::render {

// A closed box centred on the origin, `half_extents` each way: 24 vertices
// with hard edges, one quad a face, UVs repeating twice a metre so a big
// floor still shows texture, tangents along each face's u.
[[nodiscard]] MeshData box_mesh(const math::Vec3& half_extents);

// A capsule standing on y: a cylinder of `half_height` each way with a
// hemisphere on each end; `half_height` 0 is a sphere. 24 segments around,
// 6 rings up each cap.
[[nodiscard]] MeshData capsule_mesh(float radius, float half_height);

// A model of one mesh and one untextured material: `color` is the linear
// base color, and the material is a dielectric of the given roughness.
[[nodiscard]] ModelData plain_model(MeshData mesh, const math::Vec4& color, float roughness);

} // namespace tynima::render
