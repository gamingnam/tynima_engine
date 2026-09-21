# assets — the runtime side of content: cooked model blobs read and uploaded
# as they are, and the file watch that reloads one when it changes on disk.
# Nothing here parses a source format; that is the cooker's (engine/cooker),
# which a shipping build leaves out.
tynima_add_module(NAME assets
  DEPENDS core platform rhi render)
