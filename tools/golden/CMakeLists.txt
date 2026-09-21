# tynima-golden — the renderer's golden image tests: each scene drawn
# offscreen through the runtime and compared, perceptually, with the
# picture kept under goldens/. Run with --update after a deliberate change
# to the picture, and look at the new goldens before committing them.
tynima_add_app(NAME golden
  DEPENDS core platform render scene cooker sdk)
target_compile_definitions(tynima_golden PRIVATE
  TYNIMA_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/goldens"
  TYNIMA_GOLDEN_OUT="${CMAKE_BINARY_DIR}/golden"
  TYNIMA_GOLDEN_SAMPLE="${CMAKE_BINARY_DIR}/assets/WaterBottle.glb")

# On a machine with a GPU the scenes are compared; without one (a build
# sandbox, a runner with no Metal) the test is skipped, not failed. The
# renders and any diff strips are left in the build tree's golden/ for a look.
if(TYNIMA_BUILD_TESTS)
  add_test(NAME golden COMMAND tynima_golden)
  set_tests_properties(golden PROPERTIES SKIP_RETURN_CODE 77)
endif()
