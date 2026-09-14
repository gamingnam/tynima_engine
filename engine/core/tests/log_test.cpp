#include <doctest/doctest.h>
#include <tynima/core/log.h>
#include <tynima/core/memory.h>

#include <cstring>
#include <string>

using namespace tynima::core;

namespace {

// A sink that keeps the last event it saw, copied into fixed storage so the
// "logging does not allocate" case measures the logger and not the test.
struct Capture {
    int calls = 0;
    LogLevel level = LogLevel::Trace;
    char category[32] = {};
    char message[kMaxLogMessageLength] = {};
    std::size_t message_length = 0;
    const char* file = nullptr;
    int line = 0;
    std::uint32_t thread_index = 0;

    static void sink(const LogEvent& event, void* user) noexcept {
        auto* c = static_cast<Capture*>(user);
        ++c->calls;
        c->level = event.level;
        std::strncpy(c->category, event.category, sizeof(c->category) - 1);
        std::memcpy(c->message, event.message, event.message_length + 1);
        c->message_length = event.message_length;
        c->file = event.file;
        c->line = event.line;
        c->thread_index = event.thread_index;
    }
};

// Installs a Capture for the scope and holds the console quiet meanwhile so
// the test output stays readable; everything is put back on the way out.
struct CaptureScope {
    Capture capture;
    LogLevel previous_level = log_level();

    CaptureScope() {
        REQUIRE(remove_log_sink(console_log_sink));
        REQUIRE(add_log_sink(Capture::sink, &capture));
    }
    ~CaptureScope() {
        CHECK(remove_log_sink(Capture::sink, &capture));
        CHECK(add_log_sink(console_log_sink));
        set_log_level(previous_level);
    }
};

bool ends_with(const char* text, const char* suffix) {
    const std::size_t n = std::strlen(text), m = std::strlen(suffix);
    return n >= m && std::strcmp(text + (n - m), suffix) == 0;
}

// __FILE__ carries backslashes under MSVC; the path is the same either way.
bool same_path(const char* a, const char* b) {
    for (; *a != '\0' && *b != '\0'; ++a, ++b) {
        const bool slashes = (*a == '/' || *a == '\\') && (*b == '/' || *b == '\\');
        if (!slashes && *a != *b) {
            return false;
        }
    }
    return *a == *b;
}

} // namespace

TEST_CASE("an event carries level, category, message and source location") {
    CaptureScope scope;
    set_log_level(LogLevel::Trace);

    const int line = __LINE__ + 1;
    TY_LOG_WARN("test", "%d bottles of %s on the wall", 99, "water");
    CHECK(scope.capture.calls == 1);
    CHECK(scope.capture.level == LogLevel::Warn);
    CHECK(std::string(scope.capture.category) == "test");
    CHECK(std::string(scope.capture.message) == "99 bottles of water on the wall");
    CHECK(scope.capture.message_length == std::strlen("99 bottles of water on the wall"));
    CHECK(scope.capture.line == line);
    CHECK(ends_with(scope.capture.file, "log_test.cpp"));
    CHECK(scope.capture.thread_index == 0);

    // A format without arguments is the common case and must work too.
    TY_LOG_INFO("test", "plain");
    CHECK(scope.capture.calls == 2);
    CHECK(std::string(scope.capture.message) == "plain");
}

TEST_CASE("events below the minimum level never reach a sink") {
    CaptureScope scope;
    set_log_level(LogLevel::Warn);
    CHECK(log_level() == LogLevel::Warn);
    CHECK_FALSE(log_enabled(LogLevel::Info));
    CHECK(log_enabled(LogLevel::Warn));

    TY_LOG_INFO("test", "dropped");
    TY_LOG_DEBUG("test", "dropped");
    CHECK(scope.capture.calls == 0);
    TY_LOG_ERROR("test", "kept");
    CHECK(scope.capture.calls == 1);
    CHECK(scope.capture.level == LogLevel::Error);

    set_log_level(LogLevel::Off);
    TY_LOG_FATAL("test", "dropped");
    CHECK(scope.capture.calls == 1);
}

TEST_CASE("a message longer than the buffer is cut, not overrun") {
    CaptureScope scope;
    set_log_level(LogLevel::Trace);
    const std::string longer(kMaxLogMessageLength * 2, 'x');
    TY_LOG_INFO("test", "%s", longer.c_str());
    CHECK(scope.capture.calls == 1);
    CHECK(scope.capture.message_length == kMaxLogMessageLength - 1);
    CHECK(std::strlen(scope.capture.message) == kMaxLogMessageLength - 1);
}

TEST_CASE("logging allocates nothing on the heap") {
    CaptureScope scope;
    set_log_level(LogLevel::Trace);
    const int value = 42;
    const HeapAllocationScope heap;
    TY_LOG_INFO("test", "value %d and %s", value, "text");
    TY_LOG_TRACE("test", "cheap when enabled too");
    const std::uint64_t allocations = heap.allocations(); // read before CHECK, which may allocate
    CHECK(allocations == 0);
    CHECK(scope.capture.calls == 2);
}

TEST_CASE("sinks are added and removed by (function, user) pair") {
    CaptureScope scope;
    set_log_level(LogLevel::Trace);
    Capture second;
    REQUIRE(add_log_sink(Capture::sink, &second));
    TY_LOG_INFO("test", "both");
    CHECK(scope.capture.calls == 1);
    CHECK(second.calls == 1);

    CHECK(remove_log_sink(Capture::sink, &second));
    CHECK_FALSE(remove_log_sink(Capture::sink, &second)); // already gone
    TY_LOG_INFO("test", "one");
    CHECK(scope.capture.calls == 2);
    CHECK(second.calls == 1);

    // The table is fixed-size: adding past kMaxLogSinks fails cleanly.
    Capture extra[kMaxLogSinks];
    std::size_t added = 0;
    while (added < kMaxLogSinks && add_log_sink(Capture::sink, &extra[added])) {
        ++added;
    }
    CHECK(added < kMaxLogSinks); // the built-in profiler sink and ours hold slots already
    CHECK_FALSE(add_log_sink(Capture::sink, &extra[added]));
    for (std::size_t i = 0; i < added; ++i) {
        CHECK(remove_log_sink(Capture::sink, &extra[i]));
    }
}

TEST_CASE("levels have names and parse back, in any case") {
    CHECK(std::string(log_level_name(LogLevel::Trace)) == "TRACE");
    CHECK(std::string(log_level_name(LogLevel::Fatal)) == "FATAL");
    CHECK(std::string(log_level_name(LogLevel::Off)) == "OFF");

    LogLevel level = LogLevel::Trace;
    CHECK(parse_log_level("warn", level));
    CHECK(level == LogLevel::Warn);
    CHECK(parse_log_level("WARNING", level));
    CHECK(level == LogLevel::Warn);
    CHECK(parse_log_level("Error", level));
    CHECK(level == LogLevel::Error);
    CHECK(parse_log_level("off", level));
    CHECK(level == LogLevel::Off);
    CHECK_FALSE(parse_log_level("loud", level));
    CHECK_FALSE(parse_log_level("", level));
    CHECK_FALSE(parse_log_level(nullptr, level));
    CHECK(level == LogLevel::Off); // untouched by failures
}

TEST_CASE("source paths inside the repository are shown relative to it") {
    // This file is inside the repository, so its own path is shortened.
    const char* here = relative_source_path(__FILE__);
    CHECK(same_path(here, "engine/core/tests/log_test.cpp"));
    // Anything else is left alone.
    CHECK(std::string(relative_source_path("/elsewhere/game.cpp")) == "/elsewhere/game.cpp");
    CHECK(relative_source_path(nullptr) == nullptr);
}
