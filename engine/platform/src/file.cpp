#include <tynima/platform/file.h>

#include <cstdio>
#include <cstring>
#include <string>

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

bool read_file(const char* path, core::Bytes& out) noexcept {
    out.clear();
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    // Sized once from the file's length and read in one go; a file that
    // grows meanwhile (or a stream, which has no length) is read to its end
    // in pieces after that.
    bool ok = std::fseek(file, 0, SEEK_END) == 0;
    const long length = ok ? std::ftell(file) : -1;
    ok = ok && length >= 0 && std::fseek(file, 0, SEEK_SET) == 0;
    if (ok && length > 0) {
        out.resize(static_cast<std::size_t>(length));
        const std::size_t read = std::fread(out.data(), 1, out.size(), file);
        out.resize(read);
    }
    std::uint8_t chunk[65536];
    std::size_t more = 0;
    while (ok && (more = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        const std::size_t had = out.size();
        out.resize(had + more);
        std::memcpy(out.data() + had, chunk, more);
    }
    ok = ok && std::ferror(file) == 0;
    std::fclose(file);
    if (!ok) {
        out.clear();
    }
    return ok;
}

bool write_file(const char* path, const void* data, std::size_t size) noexcept {
    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr) {
        return false;
    }
    const bool written = size == 0 || std::fwrite(data, 1, size, file) == size;
    return std::fclose(file) == 0 && written;
}

bool rename_file(const char* from, const char* to) noexcept {
#if defined(_WIN32)
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(from, to) == 0; // POSIX rename replaces atomically
#endif
}

std::uint32_t process_id() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}

bool directory_exists(const char* path) noexcept {
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st{};
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool make_directories(const char* path) noexcept {
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    std::string partial;
    const std::size_t length = std::strlen(path);
    for (std::size_t i = 0; i <= length; ++i) {
        const char c = i < length ? path[i] : '\0';
        if ((c == '/' || c == '\\' || c == '\0') && !partial.empty() && partial.back() != ':') {
            if (!directory_exists(partial.c_str())) {
#if defined(_WIN32)
                if (!CreateDirectoryA(partial.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
                    return false;
                }
#else
                if (mkdir(partial.c_str(), 0755) != 0 && !directory_exists(partial.c_str())) {
                    return false;
                }
#endif
            }
        }
        if (c != '\0') {
            partial.push_back(c);
        }
    }
    return directory_exists(path);
}

} // namespace tynima::platform
