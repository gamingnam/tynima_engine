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
# The console log sink strips this prefix from source paths, so an error reads
# "apps/sandbox/src/main.cpp:817" rather than the whole absolute path.
target_compile_definitions(tynima_core PRIVATE TYNIMA_SOURCE_DIR="${PROJECT_SOURCE_DIR}/")
if(TYNIMA_PROFILE)
  target_compile_definitions(tynima_core PUBLIC TYNIMA_PROFILE=1)
endif()
