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
