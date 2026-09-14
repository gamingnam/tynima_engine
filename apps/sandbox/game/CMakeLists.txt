# The sandbox's hot-reloadable game module. Headers from sdk/scene/physics/
# core, no engine linking; see tynima_add_game_module in cmake/TynimaModule.cmake.
tynima_add_game_module(NAME sandbox_game
  DEPENDS sdk scene physics core)
