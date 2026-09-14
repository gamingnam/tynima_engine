#include <doctest/doctest.h>
#include <tynima/core/assert.h>
#include <tynima/core/log.h>

#include <cstring>
#include <string>

using namespace tynima::core;

namespace {

struct Seen {
    int calls = 0;
    std::string condition;
    std::string message;
    std::string file;
    int line = 0;
};

AssertAction record_and_continue(const AssertInfo& info, void* user) {
    auto* seen = static_cast<Seen*>(user);
    ++seen->calls;
    seen->condition = info.condition;
    seen->message = info.message;
    seen->file = info.file;
    seen->line = info.line;
    return AssertAction::Continue;
}

struct LastEvent {
    int calls = 0;
    LogLevel level = LogLevel::Trace;
    std::string category;
    std::string message;

    static void sink(const LogEvent& event, void* user) noexcept {
        auto* last = static_cast<LastEvent*>(user);
        ++last->calls;
        last->level = event.level;
        last->category = event.category;
        last->message = event.message;
    }
};

#if defined(NDEBUG)
constexpr bool kAssertsCompiled = false;
#else
constexpr bool kAssertsCompiled = true;
#endif

} // namespace

TEST_CASE("a failed assert is logged at Fatal and handed to the handler") {
    Seen seen;
    LastEvent last;
    set_assert_handler(record_and_continue, &seen);
    REQUIRE(remove_log_sink(console_log_sink)); // keep the expected failure off the test output
    REQUIRE(add_log_sink(LastEvent::sink, &last));

    const int line = __LINE__ + 1;
    TY_ASSERT(1 + 1 == 3, "arithmetic is broken");
    // Continue means we get here either way.

    REQUIRE(remove_log_sink(LastEvent::sink, &last));
    REQUIRE(add_log_sink(console_log_sink));
    set_assert_handler(nullptr);

    if (!kAssertsCompiled) {
        CHECK(seen.calls == 0); // Release: the check is gone entirely
        return;
    }
    CHECK(seen.calls == 1);
    CHECK(seen.condition == "1 + 1 == 3");
    CHECK(seen.message == "arithmetic is broken");
    CHECK(seen.line == line);
    CHECK(seen.file.find("assert_test.cpp") != std::string::npos);

    CHECK(last.calls == 1);
    CHECK(last.level == LogLevel::Fatal);
    CHECK(last.category == "assert");
    CHECK(last.message == "1 + 1 == 3: arithmetic is broken");
}

TEST_CASE("a passing assert calls nothing") {
    Seen seen;
    set_assert_handler(record_and_continue, &seen);
    TY_ASSERT(2 + 2 == 4, "arithmetic is fine");
    set_assert_handler(nullptr);
    CHECK(seen.calls == 0);
}

TEST_CASE("the condition is evaluated exactly once") {
    int evaluations = 0;
    Seen seen;
    set_assert_handler(record_and_continue, &seen);
    TY_ASSERT(++evaluations > 0, "never fails");
    set_assert_handler(nullptr);
    CHECK(evaluations == (kAssertsCompiled ? 1 : 0));
}

TEST_CASE("debugger detection answers without side effects") {
    // Whether one is attached depends on how the tests were launched; what
    // matters is that asking is safe and stable.
    const bool first = debugger_attached();
    const bool second = debugger_attached();
    CHECK(first == second);
}
