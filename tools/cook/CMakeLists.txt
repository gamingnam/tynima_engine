# tynima-cook — the asset cooker's command line: models (or directories of
# them) in, .tymodel blobs out, current ones skipped, --watch to keep them so.
tynima_add_app(NAME cook
  DEPENDS core platform assets cooker)

if(TYNIMA_BUILD_TESTS)
  # A checked-in cube cooks into the build tree, and a second run finds it
  # current; the cooker itself is tested in engine/cooker.
  set(cooked_dir "${CMAKE_BINARY_DIR}/cooked-test")
  add_test(NAME cook_cli
    COMMAND tynima_cook "${CMAKE_CURRENT_SOURCE_DIR}/tests/cube.gltf" -o "${cooked_dir}" --force)
  add_test(NAME cook_cli_current
    COMMAND tynima_cook "${CMAKE_CURRENT_SOURCE_DIR}/tests" -o "${cooked_dir}")
  set_tests_properties(cook_cli PROPERTIES FIXTURES_SETUP cooked_cube)
  set_tests_properties(cook_cli_current PROPERTIES FIXTURES_REQUIRED cooked_cube
                                                   PASS_REGULAR_EXPRESSION "current  ")
  # The sample model, when the sandbox has downloaded it: cooked here, and the
  # editor then runs on the blob alone — the shipping path, no source in sight.
  set(sample "${CMAKE_BINARY_DIR}/assets/WaterBottle.glb")
  if(EXISTS "${sample}")
    add_test(NAME cook_sample COMMAND tynima_cook "${sample}" -o "${cooked_dir}")
    set_tests_properties(cook_sample PROPERTIES FIXTURES_SETUP cooked_sample)
    if(TYNIMA_BUILD_EDITOR)
      add_test(NAME editor_headless_cooked
        COMMAND tynima_editor --headless --frames 10 --model "${cooked_dir}/WaterBottle.tymodel")
      set_tests_properties(editor_headless_cooked PROPERTIES FIXTURES_REQUIRED cooked_sample)
    endif()
  endif()
endif()
