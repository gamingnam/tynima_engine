# core — allocators, containers, math, jobs, logging, reflection, profiling.
# The bottom layer: nothing below it but the standard library and, when
# TYNIMA_PROFILE is on, the Tracy client. Only core may include Tracy headers;
# everything else goes through tynima/core/profile.h.
set(core_libs)
if(TYNIMA_PROFILE)
  list(APPEND core_libs Tracy::TracyClient)
endif()
tynima_add_module(NAME core
  LINK ${core_libs})
if(TYNIMA_PROFILE)
  target_compile_definitions(tynima_core PUBLIC TYNIMA_PROFILE=1)
endif()
