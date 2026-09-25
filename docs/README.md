# Tynima documentation

- **[Your first game in thirty minutes](first-game.md)** — start here. A
  paddle, falling cubes, and a score, built up a piece at a time in Lua
  while the game keeps running. Nothing to compile.
- **[The Lua API](lua-api.md)** — everything `ty` offers a script, which is
  how most games are written.
- **[The C API](c-api.md)** — `tynima.h`, the whole public ABI: what a game
  module written in C or C++ calls, and what the Lua above it rests on.

The two reference pages are generated from the files they document —
`engine/script/lua/tynima.lua` and `sdk/include/tynima.h` — by
`tools/gen_docs.py`, and a test regenerates them and fails when they have
drifted. Edit the source, not the page. The tutorial is written by hand,
and the program it ends with is run by the test suite on every build.

The engine's own design, module by module, is in the [repository
README](../README.md).
