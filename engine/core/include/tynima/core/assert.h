#pragma once

// TY_ASSERT(condition, "message"): checked in Debug builds, compiled away in
// Release. On failure the condition, message and location are logged at
// Fatal (category "assert", so every sink — the console, Tracy, a debugger's
// Output window — sees it), then the process breaks into the debugger when
// one is attached, right at the failing line, or aborts when none is. Under
// a debugger you can step past the trap and carry on.
//
// An assert handler, when installed, decides instead: Break trips the trap at
// the call site, Continue lets execution go on. Tests install one to check
// that an assert fires without stopping the run.

namespace tynima::core {

struct AssertInfo {
    const char* condition; // the expression, as written
    const char* message;
    const char* file;
    int line;
};

enum class AssertAction { Break, Continue };

using AssertHandler = AssertAction (*)(const AssertInfo& info, void* user);

// nullptr restores the default (log, then break or abort).
void set_assert_handler(AssertHandler handler, void* user = nullptr) noexcept;

// True while a debugger is attached to this process.
[[nodiscard]] bool debugger_attached() noexcept;

// Called by TY_ASSERT. Logs, then returns true when the caller should trap
// (a debugger is attached, or the handler said Break); never returns when
// nothing is attached and no handler is installed.
[[nodiscard]] bool assert_failed(const char* condition, const char* message, const char* file,
                                 int line) noexcept;

} // namespace tynima::core

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
// The trap is in the macro, not in assert_failed(), so the debugger stops on
// the line that failed rather than inside core.
#define TY_ASSERT(condition, message)                                                                \
    do {                                                                                              \
        if (!(condition)) {                                                                           \
            if (::tynima::core::assert_failed(#condition, message, __FILE__, __LINE__)) {             \
                TY_DEBUG_BREAK();                                                                     \
            }                                                                                         \
        }                                                                                             \
    } while (0)
#endif
