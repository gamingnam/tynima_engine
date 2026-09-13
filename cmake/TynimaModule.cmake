# tynima_add_module(NAME <name> [DEPENDS <module>...] [LINK <external target>...])
#
# Declares one engine module living in the calling directory:
#   include/   public headers, laid out as include/tynima/<name>/...
#   src/       private sources (globbed; CONFIGURE_DEPENDS re-globs on build)
#   tests/     doctest sources -> one test executable, registered with CTest
#
# Produces the target tynima_<name> with the alias tynima::<name>. A module with
# no sources yet is an INTERFACE library, so it still takes part in the
# dependency graph before it has code.
#
# DEPENDS is the module's *direct* dependencies, and it is the single source of
# truth for layering: tools/check_layering.py reads these declarations and
# rejects any #include <tynima/<other>/...> that isn't backed by one.
function(tynima_add_module)
  cmake_parse_arguments(ARG "" "NAME" "DEPENDS;LINK" ${ARGN})
  if(NOT ARG_NAME)
    message(FATAL_ERROR "tynima_add_module: NAME is required")
  endif()

  set(target tynima_${ARG_NAME})
  set(dir ${CMAKE_CURRENT_SOURCE_DIR})

  set(globs ${dir}/src/*.cpp ${dir}/src/*.c)
  if(APPLE)
    list(APPEND globs ${dir}/src/*.mm)
  endif()
  file(GLOB_RECURSE sources CONFIGURE_DEPENDS ${globs})

  if(sources)
    add_library(${target} STATIC ${sources})
    target_include_directories(${target} PUBLIC ${dir}/include PRIVATE ${dir}/src)
    tynima_apply_warnings(${target})
    set(scope PUBLIC)
  else()
    add_library(${target} INTERFACE)
    target_include_directories(${target} INTERFACE ${dir}/include)
    set(scope INTERFACE)
  endif()
  add_library(tynima::${ARG_NAME} ALIAS ${target})
  target_compile_features(${target} ${scope} cxx_std_20)
  set_target_properties(${target} PROPERTIES FOLDER "engine")

  foreach(dep IN LISTS ARG_DEPENDS)
    target_link_libraries(${target} ${scope} tynima::${dep})
  endforeach()
  foreach(lib IN LISTS ARG_LINK)
    target_link_libraries(${target} ${scope} ${lib})
  endforeach()

  if(TYNIMA_BUILD_TESTS)
    file(GLOB_RECURSE test_sources CONFIGURE_DEPENDS ${dir}/tests/*.cpp)
    if(test_sources)
      add_executable(${target}_tests ${test_sources})
      target_link_libraries(${target}_tests PRIVATE tynima::${ARG_NAME} doctest::doctest_with_main)
      tynima_apply_warnings(${target}_tests)
      set_target_properties(${target}_tests PROPERTIES FOLDER "tests")
      add_test(NAME ${ARG_NAME} COMMAND ${target}_tests)
    endif()
  endif()
endfunction()

# tynima_add_app(NAME <name> [DEPENDS <module>...] [LINK <external target>...])
#
# An executable in the calling directory, sources under src/. Same DEPENDS
# contract as modules, same layering check. Output name: tynima-<name>.
function(tynima_add_app)
  cmake_parse_arguments(ARG "" "NAME" "DEPENDS;LINK" ${ARGN})
  if(NOT ARG_NAME)
    message(FATAL_ERROR "tynima_add_app: NAME is required")
  endif()

  set(target tynima_${ARG_NAME})
  set(dir ${CMAKE_CURRENT_SOURCE_DIR})

  set(globs ${dir}/src/*.cpp ${dir}/src/*.c)
  if(APPLE)
    list(APPEND globs ${dir}/src/*.mm)
  endif()
  file(GLOB_RECURSE sources CONFIGURE_DEPENDS ${globs})
  if(NOT sources)
    message(FATAL_ERROR "tynima_add_app(${ARG_NAME}): no sources under ${dir}/src")
  endif()

  add_executable(${target} ${sources})
  set_target_properties(${target} PROPERTIES OUTPUT_NAME tynima-${ARG_NAME} FOLDER "apps")
  target_include_directories(${target} PRIVATE ${dir}/src)
  target_compile_features(${target} PRIVATE cxx_std_20)
  tynima_apply_warnings(${target})

  foreach(dep IN LISTS ARG_DEPENDS)
    target_link_libraries(${target} PRIVATE tynima::${dep})
  endforeach()
  foreach(lib IN LISTS ARG_LINK)
    target_link_libraries(${target} PRIVATE ${lib})
  endforeach()
endfunction()
