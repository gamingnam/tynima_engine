#include <tynima/core/log.h>

#include <tynima/core/jobs.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(TYNIMA_PROFILE)
#include <tracy/Tracy.hpp>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace tynima::core {

namespace {

struct Sink {
    LogSink function;
    void* user;
};

// Every piece of state is constant-initialised: logging works during static
// initialisation and static destruction, before and after main().
constinit Sink g_sinks[kMaxLogSinks] = {{console_log_sink, nullptr}, {profiler_log_sink, nullptr}};
constinit std::size_t g_sink_count = 2;
constinit std::atomic<bool> g_lock{false};
constinit std::int64_t g_epoch_ns = 0; // written once, under the lock
constinit thread_local bool t_inside_log = false;

#if defined(NDEBUG)
constexpr std::uint8_t kDefaultMinLevel = static_cast<std::uint8_t>(LogLevel::Info);
#else
constexpr std::uint8_t kDefaultMinLevel = static_cast<std::uint8_t>(LogLevel::Debug);
#endif

constexpr const char* kLevelNames[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "OFF"};
static_assert(sizeof(kLevelNames) / sizeof(kLevelNames[0]) == static_cast<std::size_t>(LogLevel::Off) + 1);

// A tiny spinlock: constant-initialisable everywhere, and a log line holds it
// for microseconds. Waiting yields, so a slow sink does not cost a core.
class LockGuard {
public:
    LockGuard() noexcept {
        while (g_lock.exchange(true, std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    ~LockGuard() { g_lock.store(false, std::memory_order_release); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
};

std::int64_t steady_now_ns() noexcept {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

char lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equals_ignoring_case(const char* a, const char* b) noexcept {
    for (; *a != '\0' && *b != '\0'; ++a, ++b) {
        if (lower(*a) != lower(*b)) {
            return false;
        }
    }
    return *a == *b;
}

// Path characters compare equal across slash styles and, on Windows, case.
bool same_path_char(char a, char b) noexcept {
    if ((a == '/' || a == '\\') && (b == '/' || b == '\\')) {
        return true;
    }
#if defined(_WIN32)
    return lower(a) == lower(b);
#else
    return a == b;
#endif
}

// ------------------------------------------------------------- console

#if defined(_WIN32)
bool console_supports_color() noexcept {
    if (!_isatty(_fileno(stderr))) {
        return false;
    }
    HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (handle == INVALID_HANDLE_VALUE || !GetConsoleMode(handle, &mode)) {
        return false;
    }
    return SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}
#else
bool console_supports_color() noexcept {
    if (!isatty(STDERR_FILENO)) {
        return false;
    }
    const char* term = std::getenv("TERM");
    if (term == nullptr || std::strcmp(term, "dumb") == 0) {
        return false;
    }
    return std::getenv("NO_COLOR") == nullptr; // https://no-color.org
}
#endif

constexpr const char* kLevelColors[] = {
    "\x1b[90m",   // Trace: grey
    "\x1b[36m",   // Debug: cyan
    "\x1b[32m",   // Info: green
    "\x1b[33m",   // Warn: yellow
    "\x1b[31m",   // Error: red
    "\x1b[1;91m", // Fatal: bold bright red
    "",
};
constexpr const char* kDim = "\x1b[2m";
constexpr const char* kReset = "\x1b[0m";

//     0.012 INFO  gpu      Metal, wants MSL shaders, depth D32_FLOAT
//     0.140 WARN  gltf     image 2: unsupported format [thread 3]
//     1.204 FATAL assert   engine_allocations == 0: a frame ... (apps/sandbox/src/main.cpp:817)
std::size_t format_console_line(char* out, std::size_t capacity, const LogEvent& e, bool color) noexcept {
    const auto level = static_cast<std::size_t>(e.level);
    const char* dim = color ? kDim : "";
    const char* tint = color ? kLevelColors[level] : "";
    const char* reset = color ? kReset : "";
    const double seconds = static_cast<double>(e.time_ns) / 1.0e9;

    int n = std::snprintf(out, capacity, "%s%9.3f%s %s%-5s%s %-8s %s", dim, seconds, reset, tint,
                          kLevelNames[level], reset, e.category, e.message);
    std::size_t length = n < 0 ? 0 : static_cast<std::size_t>(n);
    if (length >= capacity) {
        length = capacity - 1;
    }
    const auto appended = [&](int m) {
        if (m > 0) {
            length += static_cast<std::size_t>(m) < capacity - length ? static_cast<std::size_t>(m)
                                                                       : capacity - length - 1;
        }
    };
    if (e.thread_index != 0) {
        appended(std::snprintf(out + length, capacity - length, " %s[thread %u]%s", dim, e.thread_index,
                               reset));
    }
    if (e.level >= LogLevel::Error && e.file != nullptr) {
        appended(std::snprintf(out + length, capacity - length, " %s(%s:%d)%s", dim,
                               relative_source_path(e.file), e.line, reset));
    }
    if (length + 1 < capacity) {
        out[length++] = '\n';
        out[length] = '\0';
    }
    return length;
}

} // namespace

// ------------------------------------------------------------- levels

const char* log_level_name(LogLevel level) noexcept {
    const auto index = static_cast<std::size_t>(level);
    return index <= static_cast<std::size_t>(LogLevel::Off) ? kLevelNames[index] : "?";
}

bool parse_log_level(const char* text, LogLevel& out) noexcept {
    if (text == nullptr) {
        return false;
    }
    for (std::size_t i = 0; i <= static_cast<std::size_t>(LogLevel::Off); ++i) {
        if (equals_ignoring_case(text, kLevelNames[i])) {
            out = static_cast<LogLevel>(i);
            return true;
        }
    }
    if (equals_ignoring_case(text, "warning")) {
        out = LogLevel::Warn;
        return true;
    }
    return false;
}

namespace detail {

constinit std::atomic<std::uint8_t> g_log_min_level{kLogLevelUnset};

bool log_enabled_slow(LogLevel level) noexcept {
    std::uint8_t min = kDefaultMinLevel;
    LogLevel from_environment;
    if (parse_log_level(std::getenv("TYNIMA_LOG"), from_environment)) {
        min = static_cast<std::uint8_t>(from_environment);
    }
    // A set_log_level() that raced us has already decided; keep its value.
    std::uint8_t expected = kLogLevelUnset;
    g_log_min_level.compare_exchange_strong(expected, min, std::memory_order_relaxed);
    return log_enabled(level);
}

} // namespace detail

LogLevel log_level() noexcept {
    std::uint8_t min = detail::g_log_min_level.load(std::memory_order_relaxed);
    if (min == detail::kLogLevelUnset) {
        (void)detail::log_enabled_slow(LogLevel::Off);
        min = detail::g_log_min_level.load(std::memory_order_relaxed);
    }
    return static_cast<LogLevel>(min);
}

void set_log_level(LogLevel level) noexcept {
    detail::g_log_min_level.store(static_cast<std::uint8_t>(level), std::memory_order_relaxed);
}

// -------------------------------------------------------------- sinks

bool add_log_sink(LogSink sink, void* user) noexcept {
    if (sink == nullptr) {
        return false;
    }
    LockGuard lock;
    if (g_sink_count == kMaxLogSinks) {
        return false;
    }
    g_sinks[g_sink_count++] = {sink, user};
    return true;
}

bool remove_log_sink(LogSink sink, void* user) noexcept {
    LockGuard lock;
    for (std::size_t i = 0; i < g_sink_count; ++i) {
        if (g_sinks[i].function == sink && g_sinks[i].user == user) {
            for (std::size_t j = i + 1; j < g_sink_count; ++j) {
                g_sinks[j - 1] = g_sinks[j];
            }
            --g_sink_count;
            return true;
        }
    }
    return false;
}

void console_log_sink(const LogEvent& event, void*) noexcept {
    // Sinks are serialised, so this runs once, on the first event.
    static const bool color = console_supports_color();
    char line[kMaxLogMessageLength + 512];
    const std::size_t length = format_console_line(line, sizeof(line), event, color);
    std::fwrite(line, 1, length, stderr);
#if defined(_WIN32)
    if (IsDebuggerPresent()) {
        if (color) {
            (void)format_console_line(line, sizeof(line), event, false); // no escape codes for the Output window
        }
        OutputDebugStringA(line);
    }
#endif
}

void profiler_log_sink(const LogEvent& event, void*) noexcept {
#if defined(TYNIMA_PROFILE)
    constexpr std::uint32_t kColors[] = {0x808080, 0x4FC1FF, 0x9CDCFE, 0xFFC800, 0xFF5050, 0xFF0000, 0};
    TracyMessageC(event.message, event.message_length, kColors[static_cast<std::size_t>(event.level)]);
#else
    (void)event;
#endif
}

// ------------------------------------------------------------- events

const char* relative_source_path(const char* file) noexcept {
#if defined(TYNIMA_SOURCE_DIR)
    if (file == nullptr) {
        return nullptr;
    }
    const char* root = TYNIMA_SOURCE_DIR;
    const char* f = file;
    for (; *root != '\0' && *f != '\0'; ++root, ++f) {
        if (!same_path_char(*root, *f)) {
            return file;
        }
    }
    return *root == '\0' && *f != '\0' ? f : file;
#else
    return file;
#endif
}

void log_message(LogLevel level, const char* category, const char* file, int line, const char* format,
                 ...) noexcept {
    std::va_list args;
    va_start(args, format);
    log_message_v(level, category, file, line, format, args);
    va_end(args);
}

void log_message_v(LogLevel level, const char* category, const char* file, int line, const char* format,
                   std::va_list args) noexcept {
    if (!log_enabled(level) || level == LogLevel::Off) {
        return;
    }
    char message[kMaxLogMessageLength];
    const int n = std::vsnprintf(message, sizeof(message), format, args);
    std::size_t length = n < 0 ? 0 : static_cast<std::size_t>(n);
    if (length >= sizeof(message)) {
        length = sizeof(message) - 1;
    }
    message[length] = '\0';

    LogEvent event{};
    event.level = level;
    event.category = category != nullptr ? category : "";
    event.file = file;
    event.line = line;
    event.thread_index = JobSystem::current_thread_index();
    event.message = message;
    event.message_length = length;

    // A sink that logs (an assert inside one, say) must not deadlock on the
    // lock it is already under: write the line out plainly instead.
    if (t_inside_log) {
        event.time_ns = static_cast<std::uint64_t>(steady_now_ns() - g_epoch_ns);
        char plain[kMaxLogMessageLength + 512];
        std::fwrite(plain, 1, format_console_line(plain, sizeof(plain), event, false), stderr);
        return;
    }

    LockGuard lock;
    t_inside_log = true;
    if (g_epoch_ns == 0) {
        g_epoch_ns = steady_now_ns();
    }
    event.time_ns = static_cast<std::uint64_t>(steady_now_ns() - g_epoch_ns);
    for (std::size_t i = 0; i < g_sink_count; ++i) {
        g_sinks[i].function(event, g_sinks[i].user);
    }
    t_inside_log = false;
}

} // namespace tynima::core
