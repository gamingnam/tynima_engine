# platform — window, input, filesystem, time, threads.
# Wraps SDL3 so that no layer above rhi/ ever includes an SDL header
# (tools/check_layering.py enforces it).
tynima_add_module(NAME platform
  DEPENDS core
  LINK    SDL3::SDL3)
