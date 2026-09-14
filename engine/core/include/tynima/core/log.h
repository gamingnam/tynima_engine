#pragma once

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>

// Structured logging. Every message is an event — level, category, source
// location, thread, time — handed to each installed sink, which formats it
// however it likes. The built-in console sink writes one line per event to
// stderr, coloured when that is a terminal (and, on Windows under a debugger,
// to the Output window); the profiler sink puts the line on Tracy's timeline.
//
//   TY_LOG_INFO("gpu", "%s, depth %s", backend, format);
//   TY_LOG_WARN("gltf", "image %zu: %s", i, error.c_str());
//
// The category is a short static string naming the subsystem: "gpu", "gltf",
// "game". It is the column a reader filters on. Format strings are printf's,
// checked at compile time on Clang and GCC; a message is formatted into a
// stack buffer, never the heap, so logging inside a frame is allowed. Anything
// longer than kMaxLogMessageLength is cut.
//
// Events below the minimum level are dropped before formatting — one atomic
// load and a compare — so a TY_LOG_TRACE in a hot loop costs nothing when
// tracing is off. The minimum is Debug in Debug builds and Info otherwise,
// or whatever the TYNIMA_LOG environment variable says (trace, debug, info,
// warn, error, fatal, off), or what set_log_level() was last given.
//
// TY_ASSERT failures are logged at Fatal, in the "assert" category, before
// the debugger is invoked; see assert.h.

namespace tynima::core {

enum class LogLevel : std::uint8_t {
    Trace, // per-frame detail, off by default even in Debug
    Debug, // what a developer wants to see while working on it
    Info,  // what happened: a device opened, a model loaded, a module reloaded
    Warn,  // something is wrong but the engine carries on (a missing texture)
    Error, // an operation failed; the caller has to cope
    Fatal, // the process is about to stop (a failed assert)
    Off,   // as a threshold only: nothing is logged
};

// "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL" ("OFF" for Off).
[[nodiscard]] const char* log_level_name(LogLevel level) noexcept;

// Parses one of the names above (any case) into `out`; false on anything else.
[[nodiscard]] bool parse_log_level(const char* text, LogLevel& out) noexcept;

struct LogEvent {
    LogLevel level;
    const char* category;       // never null
    const char* file;           // the call site's __FILE__; null when unknown
    int line;                   // 0 when unknown
    std::uint32_t thread_index; // JobSystem::current_thread_index(): 0 outside the workers
    std::uint64_t time_ns;      // since the first log event of the process
    const char* message;        // formatted, NUL-terminated; valid for the sink call only
    std::size_t message_length;
};

// Sinks run on the thread that logged, one at a time (under a lock), so they
// need no locking of their own but must be quick. Logging from inside a sink
// goes straight to stderr rather than back through the sinks.
using LogSink = void (*)(const LogEvent& event, void* user);

constexpr std::size_t kMaxLogSinks = 8;
constexpr std::size_t kMaxLogMessageLength = 1024; // including the terminator

// Sinks are identified by the (sink, user) pair. add returns false when
// kMaxLogSinks are already installed; remove returns false when the pair is
// not installed.
bool add_log_sink(LogSink sink, void* user = nullptr) noexcept;
bool remove_log_sink(LogSink sink, void* user = nullptr) noexcept;

// The built-in sinks, both installed at startup.
void console_log_sink(const LogEvent& event, void* user) noexcept;  // stderr (+ the Windows debugger)
void profiler_log_sink(const LogEvent& event, void* user) noexcept; // Tracy; nothing without TYNIMA_PROFILE

// `file` with the repository root stripped, when it is inside it: what the
// console shows after an error. Paths from elsewhere come back unchanged.
[[nodiscard]] const char* relative_source_path(const char* file) noexcept;

// The minimum level that gets through. The default is Debug in Debug builds
// and Info in Release, overridden by TYNIMA_LOG in the environment on first use.
[[nodiscard]] LogLevel log_level() noexcept;
void set_log_level(LogLevel level) noexcept;

namespace detail {
// 0..Off once initialised; kLogLevelUnset until the environment has been read.
constexpr std::uint8_t kLogLevelUnset = 0xFF;
extern std::atomic<std::uint8_t> g_log_min_level;
[[nodiscard]] bool log_enabled_slow(LogLevel level) noexcept;
} // namespace detail

// True when an event at `level` would reach the sinks. The macros call this
// before formatting anything.
[[nodiscard]] inline bool log_enabled(LogLevel level) noexcept {
    const std::uint8_t min = detail::g_log_min_level.load(std::memory_order_relaxed);
    if (min == detail::kLogLevelUnset) [[unlikely]] {
        return detail::log_enabled_slow(level);
    }
    return static_cast<std::uint8_t>(level) >= min;
}

#if defined(__GNUC__) || defined(__clang__)
#define TY_PRINTF_FORMAT(format_index, first_argument)                                               \
    __attribute__((format(printf, format_index, first_argument)))
#else
#define TY_PRINTF_FORMAT(format_index, first_argument)
#endif

// Formats and dispatches one event. Prefer the macros, which fill in the
// source location and skip disabled levels before evaluating the arguments.
TY_PRINTF_FORMAT(5, 6)
void log_message(LogLevel level, const char* category, const char* file, int line, const char* format,
                 ...) noexcept;
TY_PRINTF_FORMAT(5, 0)
void log_message_v(LogLevel level, const char* category, const char* file, int line, const char* format,
                   std::va_list args) noexcept;

} // namespace tynima::core

#define TY_LOG(level, category, ...)                                                                 \
    do {                                                                                              \
        if (::tynima::core::log_enabled(level)) {                                                     \
            ::tynima::core::log_message(level, category, __FILE__, __LINE__, __VA_ARGS__);            \
        }                                                                                             \
    } while (0)

#define TY_LOG_TRACE(category, ...) TY_LOG(::tynima::core::LogLevel::Trace, category, __VA_ARGS__)
#define TY_LOG_DEBUG(category, ...) TY_LOG(::tynima::core::LogLevel::Debug, category, __VA_ARGS__)
#define TY_LOG_INFO(category, ...) TY_LOG(::tynima::core::LogLevel::Info, category, __VA_ARGS__)
#define TY_LOG_WARN(category, ...) TY_LOG(::tynima::core::LogLevel::Warn, category, __VA_ARGS__)
#define TY_LOG_ERROR(category, ...) TY_LOG(::tynima::core::LogLevel::Error, category, __VA_ARGS__)
#define TY_LOG_FATAL(category, ...) TY_LOG(::tynima::core::LogLevel::Fatal, category, __VA_ARGS__)
