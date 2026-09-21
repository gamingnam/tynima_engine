# cooker — the offline side of content: glTF import (cgltf), image decoding
# (stb_image), mesh ordering (meshoptimizer) and mip generation, ending in
# the cooked blob assets/ loads. Linked into tynima-cook and, for now, into
# the runtime so a source file given to the editor cooks itself on the way
# in; a shipping build links assets/ without it and loads blobs alone. Only
# cooker/ includes cgltf, stb and meshoptimizer.
tynima_add_module(NAME cooker
  DEPENDS core platform render assets
  LINK    cgltf::cgltf stb::stb meshoptimizer::meshoptimizer)
