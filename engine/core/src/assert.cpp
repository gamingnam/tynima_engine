#include <tynima/core/assert.h>

#include <tynima/core/log.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
#else
#include <cstring>
#endif

namespace tynima::core {

namespace {

constinit std::atomic<AssertHandler> g_handler{nullptr};
constinit std::atomic<void*> g_handler_user{nullptr};

} // namespace

void set_assert_handler(AssertHandler handler, void* user) noexcept {
    g_handler_user.store(user, std::memory_order_relaxed);
    g_handler.store(handler, std::memory_order_release);
}

bool debugger_attached() noexcept {
#if defined(_WIN32)
    return IsDebuggerPresent() != 0;
#elif defined(__APPLE__)
    // Technical Q&A QA1361: ask the kernel for our own process entry and
    // look at the traced flag.
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    kinfo_proc info{};
    std::size_t size = sizeof(info);
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0) {
        return false;
    }
    return (info.kp_proc.p_flag & P_TRACED) != 0;
#else
    // Linux: /proc/self/status carries "TracerPid:\t<pid>", 0 when untraced.
    std::FILE* status = std::fopen("/proc/self/status", "r");
    if (status == nullptr) {
        return false;
    }
    bool traced = false;
    char line[256];
    while (std::fgets(line, sizeof(line), status) != nullptr) {
        if (std::strncmp(line, "TracerPid:", 10) == 0) {
            traced = std::atoi(line + 10) != 0;
            break;
        }
    }
    std::fclose(status);
    return traced;
#endif
}

bool assert_failed(const char* condition, const char* message, const char* file, int line) noexcept {
    log_message(LogLevel::Fatal, "assert", file, line, "%s: %s", condition,
                message != nullptr ? message : "");

    if (const AssertHandler handler = g_handler.load(std::memory_order_acquire); handler != nullptr) {
        const AssertInfo info{condition, message, file, line};
        return handler(info, g_handler_user.load(std::memory_order_relaxed)) == AssertAction::Break;
    }
    if (debugger_attached()) {
        return true;
    }
    std::fflush(stderr);
    std::abort();
}

} // namespace tynima::core
