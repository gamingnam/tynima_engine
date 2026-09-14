#pragma once

// TY_ASSERT(condition, "message"): checked in Debug builds, compiled away in
// Release. On failure it prints where and why, then breaks into the debugger
// (or aborts when none is attached). Task 6 of Phase 2 routes the message
// through the logger; the contract stays the same.

namespace tynima::core {
[[noreturn]] void assert_failed(const char* condition, const char* message, const char* file, int line) noexcept;
}

#if defined(_MSC_VER)
#define TY_DEBUG_BREAK() __debugbreak()
#elif defined(__clang__)
#define TY_DEBUG_BREAK() __builtin_debugtrap()
#else
#define TY_DEBUG_BREAK() __builtin_trap()
#endif

#if defined(NDEBUG)
#define TY_ASSERT(condition, message) ((void)0)
#else
#define TY_ASSERT(condition, message)                                                                          \
    do {                                                                                                       \
        if (!(condition)) {                                                                                    \
            ::tynima::core::assert_failed(#condition, message, __FILE__, __LINE__);                            \
        }                                                                                                      \
    } while (0)
#endif
