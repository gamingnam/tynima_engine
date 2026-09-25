cmake_minimum_required(VERSION 3.28)

if(APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version")
endif()

# The engine version lives in engine/core/include/tynima/core/version.h.
# Keep this line in sync with it.
project(tynima VERSION 0.1.0 LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJC OBJCXX) # the native Metal backend (Phase 4) is Objective-C++
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(APPLE)
  set(CMAKE_OBJCXX_STANDARD 20)
  set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
  set(CMAKE_OBJCXX_EXTENSIONS OFF)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(TYNIMA_BUILD_TESTS        "Build unit tests (doctest)"             ON)
option(TYNIMA_BUILD_EDITOR       "Build the editor"                       ON)
option(TYNIMA_BUILD_APPS         "Build apps/ (the sandbox)"              ON)
option(TYNIMA_FETCH_SDL3         "Fetch and build SDL3 with FetchContent" ON)
option(TYNIMA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"      OFF)
option(TYNIMA_PROFILE            "Compile in Tracy instrumentation"       ON)
# LuaJIT is built by the script its authors maintain for the platform: the
# Makefile where there is make and a GCC-style driver, msvcbuild.bat under
# MSVC. Without it, engine/script still builds and every entry point says
# there is no VM.
option(TYNIMA_LUA "Embed LuaJIT for game scripts" ON)

if(TYNIMA_BUILD_TESTS)
  enable_testing()
endif()

if(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "AppleClang")
  # Diamond dependencies between static modules repeat libraries on the link
  # line; that is fine, but Apple's ld (Xcode 15+) warns about it by default.
  add_link_options(LINKER:-no_warn_duplicate_libraries)
endif()

include(cmake/Warnings.cmake)
include(cmake/TynimaModule.cmake)
include(cmake/Dependencies.cmake)

# Engine modules, bottom layer first. Each CMakeLists.txt declares the module's
# direct dependencies, and tools/check_layering.py fails the build when a
# #include reaches past that declaration.
add_subdirectory(engine/core)
add_subdirectory(engine/platform)
add_subdirectory(engine/rhi)
add_subdirectory(engine/render)
add_subdirectory(engine/ui)
add_subdirectory(engine/physics)
add_subdirectory(engine/scene)
add_subdirectory(engine/assets)
add_subdirectory(engine/cooker)
add_subdirectory(engine/script)

# The public boundary. Everything above it talks to the engine through tynima.h.
add_subdirectory(sdk)

if(TYNIMA_BUILD_EDITOR)
  add_subdirectory(editor)
endif()
if(TYNIMA_BUILD_APPS)
  add_subdirectory(apps/sandbox)
endif()

# Command-line tools: the command a game is made with, the asset cooker,
# and the golden image tests.
add_subdirectory(tools/cli)
add_subdirectory(tools/cook)
add_subdirectory(tools/golden)

# The reference pages are generated from the headers they document and
# checked in, so reading them needs no Python; where there is one, a test
# regenerates them and fails when they no longer match.
if(TYNIMA_BUILD_TESTS)
  find_program(TYNIMA_PYTHON NAMES python3 python)
  if(TYNIMA_PYTHON)
    add_test(NAME docs_current
      COMMAND "${TYNIMA_PYTHON}" "${PROJECT_SOURCE_DIR}/tools/gen_docs.py" --check)
  endif()
endif()
