#pragma once

#include <tynima/render/model.h>

#include <cstddef>
#include <string>

// Image decoding (PNG, JPEG, and the other formats stb_image reads) into
// RGBA8, for the cooker: the runtime loads the levels the cooker built
// from these (mipmaps.h) and never decodes an image itself. And the way
// back, RGBA8 to a PNG, for what a tool renders (tynima-golden).
namespace tynima::cooker {

// `out.srgb` is left untouched: whether the bytes are display-encoded is a
// fact about how the image is used, not about the file.
[[nodiscard]] bool decode_image(const void* bytes, std::size_t size, render::ImageData& out, std::string& error);
[[nodiscard]] bool load_image_file(const char* path, render::ImageData& out, std::string& error);

// The image as a PNG at `path`, the same bytes for the same pixels every
// time, so a file written twice is not a change to version control.
[[nodiscard]] bool save_png(const char* path, const render::ImageData& image, std::string& error);

} // namespace tynima::cooker
