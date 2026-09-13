#pragma once

#include <tynima/render/model.h>

#include <cstddef>
#include <string>

// Image decoding (PNG, JPEG, and the other formats stb_image reads) into
// RGBA8. Like glTF import, this is development-time loading; the cooker
// (Phase 5) will store GPU-ready compressed textures instead.
namespace tynima::assets {

// `out.srgb` is left untouched: whether the bytes are display-encoded is a
// fact about how the image is used, not about the file.
[[nodiscard]] bool decode_image(const void* bytes, std::size_t size, render::ImageData& out, std::string& error);
[[nodiscard]] bool load_image_file(const char* path, render::ImageData& out, std::string& error);

} // namespace tynima::assets
