# render — frame graph, passes, materials, culling, draw submission.
# Knows nothing about entities: scene/ feeds it renderables. platform is
# here for the error strings the RHI leaves in platform::last_error().
tynima_add_module(NAME render
  DEPENDS core platform rhi)
