# Third-party code arrives through FetchContent as release tarballs, pinned by
# SHA256. Nothing is vendored; the first configure needs network access.
# Tarballs beat git clones here: a fraction of the download, no .git directory
# in _deps, and the hash guarantees the bytes are the ones we reviewed.
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
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(SDL3)
endif()

if(TYNIMA_BUILD_TESTS)
  set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
  set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
  FetchContent_Declare(doctest
    URL      https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz
    URL_HASH SHA256=174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(doctest)
endif()
