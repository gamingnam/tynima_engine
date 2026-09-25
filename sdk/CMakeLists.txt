# sdk — the public C ABI (include/tynima.h), the runtime behind its host
# half (sdk/runtime.h: the window, the GPU, the world, the physics, the game
# module and the UI, a frame at a time) plus, later, the project CLI,
# templates and codegen. The only thing the editor and games may see.
tynima_add_module(NAME sdk
  DEPENDS core platform rhi render ui physics scene assets cooker script)
# A script's Lua is the engine's own until a game ships its own copy.

# The templates the tests play, where `tynima new` copies them from. Until
# there is an install, that is the source tree.
if(TARGET tynima_sdk_tests)
  target_compile_definitions(tynima_sdk_tests PRIVATE
    TYNIMA_TEMPLATE_DIR="${PROJECT_SOURCE_DIR}/sdk/templates")
endif()
