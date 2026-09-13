# editor — ImGui tools. A client of the SDK with no special privileges:
# it may include tynima.h and nothing else. tools/check_layering.py enforces it.
tynima_add_app(NAME editor
  DEPENDS sdk)
