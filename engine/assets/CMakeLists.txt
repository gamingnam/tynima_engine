# assets — runtime loading of cooked blobs, GPU upload, file-watch hot reload.
# Until the cooker exists (Phase 5) this is also where glTF import and image
# decoding live, so the sandbox can load sample models directly. Only
# assets/ includes cgltf and stb_image.
tynima_add_module(NAME assets
  DEPENDS core platform render scene
  LINK    cgltf::cgltf stb::stb)
