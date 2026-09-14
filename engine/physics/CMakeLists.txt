# physics — broadphase, narrowphase, solver, joints.
# Jolt lives behind the same interface as our own solver (Phase 3). It is a
# private dependency: no Jolt header escapes this module, and the layering
# check holds everyone else to that.
tynima_add_module(NAME physics
  DEPENDS core
  LINK    Jolt::Jolt)
