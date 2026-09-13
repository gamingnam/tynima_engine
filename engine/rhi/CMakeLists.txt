# rhi — render hardware interface: devices, shaders, pipelines, command
# buffers, in engine vocabulary. SDL3 GPU underneath for now; a native Metal
# backend joins it in Phase 4 so the frame graph can fuse tile passes.
tynima_add_module(NAME rhi
  DEPENDS core platform
  LINK    SDL3::SDL3)
