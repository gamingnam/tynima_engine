# Third-party code arrives through FetchContent as release tarballs, pinned by
# SHA256. Nothing is vendored; the first configure needs network access.
# Tarballs beat git clones here: a fraction of the download, no .git directory
# in _deps, and the hash guarantees the bytes are the ones we reviewed.
# SYSTEM marks their headers as system headers, so our warnings-as-errors CI
# only ever judges our own code.
include(FetchContent)

# Fetched projects sometimes declare a cmake_minimum_required older than CMake 4
# accepts; this lets them configure without patching.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

if(TYNIMA_FETCH_SDL3)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    URL      https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz
    URL_HASH SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68
    EXCLUDE_FROM_ALL SYSTEM)
  FetchContent_MakeAvailable(SDL3)
else()
  # Bring your own: brew install sdl3 / vcpkg install sdl3, then point
  # CMAKE_PREFIX_PATH at it.
  find_package(SDL3 CONFIG REQUIRED)
endif()

if(TYNIMA_BUILD_TESTS)
  set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
  set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
  FetchContent_Declare(doctest
    URL      https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz
    URL_HASH SHA256=174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331
    EXCLUDE_FROM_ALL SYSTEM)
  FetchContent_MakeAvailable(doctest)
endif()

# cgltf is a single header with no build system of its own.
FetchContent_Declare(cgltf
  URL      https://github.com/jkuhlmann/cgltf/archive/refs/tags/v1.15.tar.gz
  URL_HASH SHA256=84e352092e5cd6aab7f66de62ddb66beb5e6f18d412ca9d12950d7a55bfef25a
  EXCLUDE_FROM_ALL SYSTEM)
FetchContent_MakeAvailable(cgltf)
add_library(cgltf INTERFACE)
target_include_directories(cgltf SYSTEM INTERFACE ${cgltf_SOURCE_DIR})
add_library(cgltf::cgltf ALIAS cgltf)

# stb has no releases; pinned to a commit. Only stb_image.h is used.
FetchContent_Declare(stb
  URL      https://github.com/nothings/stb/archive/2c980bb59875b0d32144a71867fbdebb2f77cd20.tar.gz
  URL_HASH SHA256=9a955b1b49a4410088a2e0ee2a9c057c3c907d0c1d75454144cb980aca0ba515
  EXCLUDE_FROM_ALL SYSTEM)
FetchContent_MakeAvailable(stb)
add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE ${stb_SOURCE_DIR})
add_library(stb::stb ALIAS stb)

# meshoptimizer: the cooker's vertex cache, overdraw and fetch ordering —
# the ordering the GPU reads a static mesh in — and its vertex welding.
# Offline only: nothing in a shipping build links it.
set(MESHOPT_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_GLTFPACK OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(MESHOPT_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(meshoptimizer
  URL      https://github.com/zeux/meshoptimizer/archive/refs/tags/v1.2.tar.gz
  URL_HASH SHA256=e40f71b809cdf3361b9a4def85fd44534e8733ce29d4b943c145b76859e4c2b4
  EXCLUDE_FROM_ALL SYSTEM)
FetchContent_MakeAvailable(meshoptimizer)
set_target_properties(meshoptimizer PROPERTIES FOLDER "third_party")
if(NOT TARGET meshoptimizer::meshoptimizer)
  add_library(meshoptimizer::meshoptimizer ALIAS meshoptimizer)
endif()

# LuaJIT: the VM game scripts run in, and the reason scripts need no
# bindings written by hand — its FFI calls C through the same tynima_api
# table a C game module gets, JIT-compiled, from declarations generated out
# of tynima.h (tools/gen_lua.py).
#
# It has no CMake build of its own, and reimplementing its two-stage one
# (minilua, then dynasm, then buildvm, then the library) is a good way to
# get it subtly wrong. So it is fetched like everything else, pinned by
# hash, and built by the Makefile its authors maintain; what CMake keeps is
# the target that depends on the resulting library.
if(TYNIMA_LUA)
  FetchContent_Declare(luajit
    URL      https://github.com/LuaJIT/LuaJIT/archive/c6ffc141a8762b41703f9287d63d93622a13dd8f.tar.gz
    URL_HASH SHA256=6e5fec07750add912e7c3eae0c194d24cd6d023714e1f04a0298a5b4819e4457
    EXCLUDE_FROM_ALL SYSTEM)
  FetchContent_MakeAvailable(luajit) # no CMakeLists inside: this only unpacks it
  # Both builds are LuaJIT's own, and both build in its source tree rather
  # than ours — which is why it is fetched per build directory.
  if(MSVC)
    # On Windows the script its authors maintain is src/msvcbuild.bat, and
    # it refuses to run outside a Visual Studio environment (it wants
    # INCLUDE set). Rather than hope the one that launched this build has
    # one, the wrapper beside this file calls vcvarsall itself, found from
    # the compiler CMake already located.
    set(luajit_library "${luajit_SOURCE_DIR}/src/lua51.lib")
    get_filename_component(luajit_msvc_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    # <VS>/VC/Tools/MSVC/<version>/bin/Host<arch>/<arch> back up to <VS>/VC.
    get_filename_component(luajit_vc_root "${luajit_msvc_bin}/../../../../../.." ABSOLUTE)
    set(luajit_vcvarsall "${luajit_vc_root}/Auxiliary/Build/vcvarsall.bat")
    if(CMAKE_GENERATOR_PLATFORM MATCHES "^([Aa][Rr][Mm]64)$")
      set(luajit_vc_arch x64_arm64)
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
      set(luajit_vc_arch x86)
    else()
      set(luajit_vc_arch x64)
    endif()
    file(TO_NATIVE_PATH "${luajit_SOURCE_DIR}/src" luajit_src_native)
    file(TO_NATIVE_PATH "${luajit_vcvarsall}" luajit_vcvarsall_native)
    set(luajit_script "${CMAKE_BINARY_DIR}/luajit_msvcbuild.bat")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/luajit_msvcbuild.bat.in" "${luajit_script}"
      @ONLY NEWLINE_STYLE CRLF)
    file(TO_NATIVE_PATH "${luajit_script}" luajit_script_native)
    # One build directory builds LuaJIT once, with the runtime library of
    # whichever configuration built it first; a second configuration in the
    # same directory wants a build directory of its own.
    #
    # `cmd /c call "..."` rather than `cmd /c "..."`: where the rest of the
    # line begins with a quote, cmd strips that quote and the last one on
    # the line, which takes a path apart.
    add_custom_command(OUTPUT "${luajit_library}"
      COMMAND "${CMAKE_COMMAND}" -E env "CL=$<IF:$<CONFIG:Debug>,/MDd,/MD>"
              cmd /c call "${luajit_script_native}"
      COMMENT "Building LuaJIT with msvcbuild.bat"
      VERBATIM)
  else()
    set(luajit_library "${luajit_SOURCE_DIR}/src/libluajit.a")
    add_custom_command(OUTPUT "${luajit_library}"
      COMMAND ${CMAKE_COMMAND} -E env
              "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
              make -C "${luajit_SOURCE_DIR}" -j 4 amalg
      WORKING_DIRECTORY "${luajit_SOURCE_DIR}"
      COMMENT "Building LuaJIT with its own Makefile"
      VERBATIM)
  endif()
  add_custom_target(luajit_build DEPENDS "${luajit_library}")
  add_library(luajit INTERFACE)
  add_dependencies(luajit luajit_build)
  target_include_directories(luajit SYSTEM INTERFACE "${luajit_SOURCE_DIR}/src")
  target_link_libraries(luajit INTERFACE "${luajit_library}")
  if(APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    # 64-bit Intel Macs: LuaJIT's allocator needs the low 4 GB, which the
    # default page-zero size hides. Apple Silicon is GC64 and needs nothing.
    target_link_options(luajit INTERFACE LINKER:-pagezero_size,10000 LINKER:-image_base,100000000)
  endif()
  add_library(luajit::luajit ALIAS luajit)
endif()

# Jolt Physics: the reference the Phase 3 solver is measured against, behind
# physics::PhysicsWorld. Built as plain CPU physics — no GPU compute back
# ends, no object streams, no built-in profiler or debug renderer — and
# cross-platform deterministic, so a simulation state hash means the same
# thing on every CI runner. Its asserts are on in Debug builds.
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE) # match the rest of the build (/MD)
set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)                 # physics/ derives from Jolt classes with RTTI on
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)             # no -Werror inside a dependency
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(CROSS_PLATFORM_DETERMINISTIC ON CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(USE_ASSERTS ON CACHE BOOL "" FORCE)
else()
  set(USE_ASSERTS OFF CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(jolt
  URL      https://github.com/jrouwe/JoltPhysics/archive/refs/tags/v5.6.0.tar.gz
  URL_HASH SHA256=6e069ee0172478cc78182047aac87e5310ba14a67a53348ae14cc37801fd3f8e
  SOURCE_SUBDIR Build
  EXCLUDE_FROM_ALL SYSTEM)
FetchContent_MakeAvailable(jolt)

# Dear ImGui, the docking branch: every panel of the editor, and any debug
# overlay a game wants. Only the library itself is built; the platform and
# renderer sides are the engine's own (engine/ui), on top of platform/ and
# rhi/, so ImGui never sees SDL or Metal and draws through the same RHI as
# the scene. Only ui/ and the editor may include its headers.
FetchContent_Declare(imgui
  URL      https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b-docking.tar.gz
  URL_HASH SHA256=90ded916bd57db2e0e171b6b098940a47c6f5042725dcdc67fb19940ca8bfdcc
  EXCLUDE_FROM_ALL SYSTEM)
FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp)
target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR})
target_compile_features(imgui PUBLIC cxx_std_20)
# No pre-1.92 API, so the texture protocol the engine implements is the only one.
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
set_target_properties(imgui PROPERTIES FOLDER "third_party")
add_library(imgui::imgui ALIAS imgui)

if(TYNIMA_PROFILE)
  # The client must match the profiler GUI's version: 0.13.1 is what
  # `brew install tracy` ships. Bump both together.
  set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
  set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)      # record only while a profiler is connected
  set(TRACY_ONLY_LOCALHOST ON CACHE BOOL "" FORCE) # never listen on the LAN
  set(TRACY_STATIC ON CACHE BOOL "" FORCE)
  FetchContent_Declare(tracy
    URL      https://github.com/wolfpld/tracy/archive/refs/tags/v0.13.1.tar.gz
    URL_HASH SHA256=d4efc50ebcb0bfcfdbba148995aeb75044c0d80f5d91223aebfaa8fa9e563d2b
    EXCLUDE_FROM_ALL SYSTEM)
  FetchContent_MakeAvailable(tracy)
  if(TARGET TracyClient AND NOT MSVC)
    # Tracy's own sources use sprintf, which Apple's SDK marks deprecated; the
    # warning is theirs, not ours, and only clutters the build log.
    target_compile_options(TracyClient PRIVATE -Wno-deprecated-declarations)
  endif()
endif()
