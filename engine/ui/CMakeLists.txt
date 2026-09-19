# ui — Dear ImGui as the engine hosts it: one context, fed from platform/'s
# input and drawn through rhi/. Every panel of the editor is ImGui, and so
# is a game's debug overlay; ImGui itself never sees SDL or Metal. Only this
# module and the editor include imgui.h (tools/check_layering.py enforces it).
tynima_add_module(NAME ui
  DEPENDS core platform rhi
  LINK    imgui::imgui)
