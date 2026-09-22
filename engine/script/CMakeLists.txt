# script — the VM game logic runs in. A script is a game module written in
# Lua: it gets the same tynima_api table a C one does, and calls through it
# with LuaJIT's FFI, so there are no bindings written by hand — the
# declarations are generated from tynima.h (tools/gen_lua.py) and the
# components come from reflection at run time.
#
# Without TYNIMA_LUA the module still builds: Vm::create() then says there
# is no VM in this build, and everything above it carries on without scripts.
set(script_libs)
if(TYNIMA_LUA)
  list(APPEND script_libs luajit::luajit)
endif()
tynima_add_module(NAME script
  DEPENDS core platform scene assets
  LINK    ${script_libs})
if(TYNIMA_LUA)
  target_compile_definitions(tynima_script PUBLIC TYNIMA_LUA=1)
endif()
# Where the Lua half lives: the generated FFI declarations and the wrapper
# a script requires. A game ships these two files beside its own.
target_compile_definitions(tynima_script PRIVATE TYNIMA_LUA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/lua")

# The Lua side of the C ABI is generated and checked in, so building a game
# needs no Python; where there is one, a test regenerates it and fails when
# it no longer matches the header it came from.
if(TYNIMA_BUILD_TESTS)
  find_program(TYNIMA_PYTHON NAMES python3 python)
  if(TYNIMA_PYTHON)
    add_test(NAME lua_bindings_current
      COMMAND "${TYNIMA_PYTHON}" "${PROJECT_SOURCE_DIR}/tools/gen_lua.py" --check)
  endif()
endif()
