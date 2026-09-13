# rhi — render hardware interface: devices, buffers, textures, pipelines,
# command buffers. SDL3 GPU first; a native Metal backend joins it in Phase 4.
tynima_add_module(NAME rhi
  DEPENDS core platform)
