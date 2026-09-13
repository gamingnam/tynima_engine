# platform — window, input, filesystem, time, threads.
# Wraps SDL3 so that no layer above this one ever includes an SDL header.
set(platform_libs)
if(TYNIMA_FETCH_SDL3)
  list(APPEND platform_libs SDL3::SDL3)
endif()
tynima_add_module(NAME platform
  DEPENDS core
  LINK    ${platform_libs})
