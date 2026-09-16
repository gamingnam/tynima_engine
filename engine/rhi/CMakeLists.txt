# rhi — render hardware interface: devices, shaders, pipelines, command
# buffers, in engine vocabulary. SDL3 GPU underneath for now; a native Metal
# backend joins it in Phase 4 so the frame graph can fuse tile passes.
tynima_add_module(NAME rhi
  DEPENDS core platform
  LINK    SDL3::SDL3)

# The native Metal backend: Objective-C++ under ARC, against the frameworks.
if(APPLE)
  set_source_files_properties(src/metal_device.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(tynima_rhi PUBLIC "-framework Metal" "-framework QuartzCore" "-framework Foundation")
endif()
