#include <tynima/cooker/image.h>

#include <tynima/core/profile.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include <stb_image.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace tynima::cooker {

bool decode_image(const void* bytes, std::size_t size, render::ImageData& out, std::string& error) {
    TY_PROFILE_SCOPE_NAMED("cooker::decode_image");
    if (bytes == nullptr || size == 0 || size > 0x7FFFFFFFu) {
        error = "empty or oversized image data";
        return false;
    }
    int width = 0;
    int height = 0;
    int channels_in_file = 0;
    stbi_uc* pixels = stbi_load_from_memory(static_cast<const stbi_uc*>(bytes), static_cast<int>(size), &width,
                                            &height, &channels_in_file, 4);
    if (pixels == nullptr) {
        const char* reason = stbi_failure_reason();
        error = std::string("image decode failed: ") + (reason != nullptr ? reason : "unknown");
        return false;
    }
    out.width = static_cast<std::uint32_t>(width);
    out.height = static_cast<std::uint32_t>(height);
    const std::size_t byte_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    out.pixels.assign(pixels, pixels + byte_count);
    stbi_image_free(pixels);
    return true;
}

bool load_image_file(const char* path, render::ImageData& out, std::string& error) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        error = std::string("cannot open '") + path + "'";
        return false;
    }
    std::vector<unsigned char> bytes;
    unsigned char chunk[65536];
    std::size_t read = 0;
    while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        bytes.insert(bytes.end(), chunk, chunk + read);
    }
    std::fclose(file);
    return decode_image(bytes.data(), bytes.size(), out, error);
}

} // namespace tynima::cooker
