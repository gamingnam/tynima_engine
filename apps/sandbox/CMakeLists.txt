# sandbox — the engine developer's playground: a pile of a loaded glTF model
# in the physics world, a depth buffer and a fly camera. Unlike the editor and
# games it may reach into engine modules directly. It is a test bed, not a
# template for anything.
tynima_add_app(NAME sandbox
  DEPENDS core platform rhi render physics scene sdk)

# The game module is a separate library the sandbox loads at run time; the
# sandbox knows where the build puts it and rebuilds it alongside itself.
add_subdirectory(game)
add_dependencies(tynima_sandbox tynima_sandbox_game)
target_compile_definitions(tynima_sandbox PRIVATE TYNIMA_SANDBOX_GAME_MODULE="$<TARGET_FILE:tynima_sandbox_game>")

# Sample content, downloaded once into the build tree rather than vendored.
# WaterBottle: CC0, Khronos glTF-Sample-Assets, 26 cm tall, full PBR texture set.
set(sandbox_assets_dir "${CMAKE_BINARY_DIR}/assets")
set(sample_commit 90d7ede14c7e280af263824604b427a1ca02cb66)
set(water_bottle "${sandbox_assets_dir}/WaterBottle.glb")
if(NOT EXISTS "${water_bottle}")
  message(STATUS "Downloading WaterBottle.glb (8.9 MB) into ${sandbox_assets_dir}")
  file(DOWNLOAD
    "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/${sample_commit}/Models/WaterBottle/glTF-Binary/WaterBottle.glb"
    "${water_bottle}"
    EXPECTED_HASH SHA256=b337e526fd6a162013c2984aeec163f5fbb4f717252724dfc3f3458bd51df94b
    STATUS download_status)
  list(GET download_status 0 download_code)
  if(NOT download_code EQUAL 0)
    list(GET download_status 1 download_message)
    message(WARNING "Sample model download failed (${download_message}); the sandbox will run without it")
    file(REMOVE "${water_bottle}")
  endif()
endif()
target_compile_definitions(tynima_sandbox PRIVATE TYNIMA_SANDBOX_ASSETS_DIR="${sandbox_assets_dir}")

# Smoke test: the full init -> window -> model load -> event loop -> shutdown
# path, without a display. Runs on CI runners and anywhere else with no window server.
#
# Determinism tests: each log under replays/ is a headless run recorded on
# one machine — five seconds of the pile dropping, the character walking and
# jumping, the game module launching the pile — ending with the physics
# world's hash, one on the engine's own physics and one on Jolt. Replaying
# a log must reach its hash, here and on every other platform (exit code 3
# when it does not). Without the sample model there is no pile, and the
# sandbox reports the test as skipped (77).
#
# When the simulation changes on purpose, record them again:
#   tynima-sandbox --headless --frames 300 --record apps/sandbox/replays/pile.tyrec
#   tynima-sandbox --headless --frames 300 --physics jolt \
#                  --record apps/sandbox/replays/pile_jolt.tyrec
if(TYNIMA_BUILD_TESTS)
  add_test(NAME sandbox_headless COMMAND tynima_sandbox --headless --frames 30)
  foreach(log IN ITEMS pile pile_jolt)
    add_test(NAME sandbox_replay_${log}
      COMMAND tynima_sandbox --headless --replay "${CMAKE_CURRENT_SOURCE_DIR}/replays/${log}.tyrec")
    set_tests_properties(sandbox_replay_${log} PROPERTIES SKIP_RETURN_CODE 77)
  endforeach()
endif()
