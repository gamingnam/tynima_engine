#include <tynima/platform/file.h>

#include <cstdio>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace tynima::platform {

std::uint64_t file_write_time(const char* path) noexcept {
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) {
        return 0;
    }
    return (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
           data.ftLastWriteTime.dwLowDateTime;
#else
    struct stat st{};
    if (stat(path, &st) != 0) {
        return 0;
    }
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(st.st_mtimespec.tv_sec) * 1000000000ull +
           static_cast<std::uint64_t>(st.st_mtimespec.tv_nsec);
#else
    return static_cast<std::uint64_t>(st.st_mtim.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(st.st_mtim.tv_nsec);
#endif
#endif
}

bool copy_file(const char* from, const char* to) noexcept {
    std::FILE* in = std::fopen(from, "rb");
    if (in == nullptr) {
        return false;
    }
    std::FILE* out = std::fopen(to, "wb");
    if (out == nullptr) {
        std::fclose(in);
        return false;
    }
    unsigned char buffer[65536];
    bool ok = true;
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (std::fwrite(buffer, 1, read, out) != read) {
            ok = false;
            break;
        }
    }
    ok = ok && std::ferror(in) == 0;
    std::fclose(in);
    ok = std::fclose(out) == 0 && ok;
#if !defined(_WIN32)
    if (ok) {
        chmod(to, 0755); // a library needs to be executable-mapped
    }
#endif
    return ok;
}

bool remove_file(const char* path) noexcept {
    return std::remove(path) == 0;
}

} // namespace tynima::platform
