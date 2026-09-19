# sdk — the public C ABI (include/tynima.h), the runtime behind its host
# half (sdk/runtime.h: the window, the GPU, the world, the physics, the game
# module and the UI, a frame at a time) plus, later, the project CLI,
# templates and codegen. The only thing the editor and games may see.
tynima_add_module(NAME sdk
  DEPENDS core platform rhi render ui physics scene assets script)
