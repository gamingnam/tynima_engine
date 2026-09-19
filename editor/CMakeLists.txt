# editor — the ImGui tools. A client of the SDK with no special privileges:
# it may include tynima.h (and Dear ImGui's headers, which the engine hosts
# for it) and nothing else. tools/check_layering.py enforces it.
tynima_add_app(NAME editor
  DEPENDS sdk
  LINK    imgui::imgui)

# Smoke test: the editor's panels drawn for a few frames without a display —
# every path through the public header, minus the pixels.
if(TYNIMA_BUILD_TESTS)
  add_test(NAME editor_headless COMMAND tynima_editor --headless --frames 30)
endif()
