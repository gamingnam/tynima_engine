# tynima — the command a game is made and run with: new, run, cook, build.
# It hosts the engine itself for `run`, so a Lua game needs no other binary.
tynima_add_app(NAME cli
  DEPENDS core platform assets cooker sdk)
# The command is `tynima`, not `tynima-cli`.
set_target_properties(tynima_cli PROPERTIES OUTPUT_NAME tynima)
# Where `new` copies from. Until there is an install, that is the source tree.
target_compile_definitions(tynima_cli PRIVATE TYNIMA_TEMPLATE_DIR="${PROJECT_SOURCE_DIR}/sdk/templates")

# End to end, the way the docs tell a stranger to start: make a project,
# cook it (there is nothing in it yet, which is not an error), and run it
# headless for a few frames. If `tynima new` ever produces something that
# does not run, this fails.
if(TYNIMA_BUILD_TESTS AND TYNIMA_LUA)
  set(cli_scratch "${CMAKE_BINARY_DIR}/cli-test")
  # `new` refuses to write over a project, which is what we want of it — so
  # the last run's is cleared first, and the two together set the fixture
  # the other tests wait for.
  add_test(NAME cli_clean COMMAND "${CMAKE_COMMAND}" -E rm -rf "${cli_scratch}")
  add_test(NAME cli_new COMMAND tynima_cli new demo --into "${cli_scratch}/demo")
  add_test(NAME cli_run COMMAND tynima_cli run --headless --frames 20)
  add_test(NAME cli_cook COMMAND tynima_cli cook)
  set_tests_properties(cli_clean cli_new PROPERTIES FIXTURES_SETUP cli_project)
  set_tests_properties(cli_new PROPERTIES DEPENDS cli_clean)
  set_tests_properties(cli_run cli_cook PROPERTIES
    FIXTURES_REQUIRED cli_project
    WORKING_DIRECTORY "${cli_scratch}/demo")
endif()
