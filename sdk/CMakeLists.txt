# sdk — the public C ABI (include/tynima.h) plus, later, the project CLI,
# templates and codegen. The only thing the editor and games may see.
tynima_add_module(NAME sdk
  DEPENDS core platform rhi render physics scene assets script)
