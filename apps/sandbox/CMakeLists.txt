# sandbox — the engine developer's playground (Phase 0: window, then triangle).
# Unlike the editor and games it may reach into engine modules directly.
# It is a test bed, not a template for anything.
tynima_add_app(NAME sandbox
  DEPENDS core platform rhi render physics scene assets script)
