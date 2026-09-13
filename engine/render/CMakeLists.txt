# render — frame graph, passes, materials, culling, draw submission.
# Knows nothing about entities: scene/ feeds it renderables.
tynima_add_module(NAME render
  DEPENDS core rhi)
