# assets — runtime loading of cooked blobs, GPU upload, file-watch hot reload.
# Import of source formats (glTF, PNG) happens in the offline cooker, not here.
tynima_add_module(NAME assets
  DEPENDS core platform render scene)
