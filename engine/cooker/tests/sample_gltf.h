#pragma once

// The one glTF the cooker's tests share, as text.
namespace tynima::cooker::tests {

// Generated once by a small script. Geometry: a quad (normals, UVs, uint16
// indices, material 1) under a node translated by (1,2,3), and a triangle
// with no normals and no indices under a node scaled by 2. Materials: a
// plain double-sided emissive one, and a textured one using two 2x2 PNGs
// stored in buffer views — image 0 as base color and emissive (sRGB), image
// 1 as metallic-roughness, normal and occlusion (linear data). The buffer is
// a data URI, so the test needs nothing on disk.
constexpr const char* kGltf =
    R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,1]}],"nodes":[{"mesh":0,"translation":[1,2,3]},{"mesh":1,"scale":[2,2,2]}],"meshes":[{"name":"quad","primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":1}]},{"name":"tri","primitives":[{"attributes":{"POSITION":4}}]}],"materials":[{"name":"plain","emissiveFactor":[1,0,0],"doubleSided":true},{"name":"textured","pbrMetallicRoughness":{"baseColorTexture":{"index":0},"baseColorFactor":[0.5,1,1,1],"metallicFactor":0.2,"roughnessFactor":0.7,"metallicRoughnessTexture":{"index":1}},"normalTexture":{"index":1,"scale":0.8},"occlusionTexture":{"index":1,"strength":0.5},"emissiveTexture":{"index":0},"emissiveFactor":[1,1,1]}],"textures":[{"source":0},{"source":1}],"images":[{"bufferView":5,"mimeType":"image/png"},{"bufferView":6,"mimeType":"image/png"}],"buffers":[{"byteLength":326,"uri":"data:application/octet-stream;base64,AACAvwAAgL8AAAAAAACAPwAAgL8AAAAAAACAPwAAgD8AAAAAAACAvwAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAgD8AAIA/AAAAAAAAgD8AAAEAAgAAAAIAAwAAAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAAAAAACAPwAAAACJUE5HDQoaCgAAAA1JSERSAAAAAgAAAAIIBgAAAHK2DSQAAAATSURBVHicY/jPwPAfDIE0CDQAAElJCXgooNt3AAAAAElFTkSuQmCCiVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAEUlEQVR4nGNocPj/H4QZYAwAXlIK+eiZmxgAAAAASUVORK5CYII="}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48},{"buffer":0,"byteOffset":48,"byteLength":48},{"buffer":0,"byteOffset":96,"byteLength":32},{"buffer":0,"byteOffset":128,"byteLength":12},{"buffer":0,"byteOffset":140,"byteLength":36},{"buffer":0,"byteOffset":176,"byteLength":76},{"buffer":0,"byteOffset":252,"byteLength":74}],"accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},{"bufferView":1,"componentType":5126,"count":4,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":4,"type":"VEC2"},{"bufferView":3,"componentType":5123,"count":6,"type":"SCALAR"},{"bufferView":4,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}]})";

} // namespace tynima::cooker::tests
