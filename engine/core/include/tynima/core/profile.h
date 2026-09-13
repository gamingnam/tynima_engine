#pragma once

// Profiling hooks, backed by Tracy when the build defines TYNIMA_PROFILE (the
// default for every configuration; on-demand, so nothing is recorded until a
// profiler connects). Without it every macro compiles away. Instrument freely.
//
//   TY_PROFILE_THREAD("main");           once per thread, before its first zone
//   TY_PROFILE_FRAME();                  at the end of every frame
//   TY_PROFILE_SCOPE();                  a zone named after the enclosing function
//   TY_PROFILE_SCOPE_NAMED("culling");   a zone with an explicit name (string literal)
//   TY_PROFILE_PLOT("draw calls", n);    a value tracked over time (int64, float or double)
//   TY_PROFILE_MESSAGE("hot reload");    a marker on the timeline (string literal)
//
// One TY_PROFILE_SCOPE* per C++ scope: each declares a hidden local.
// Core is the only module that may include Tracy headers directly.

#if defined(TYNIMA_PROFILE)
#include <tracy/Tracy.hpp>
#define TY_PROFILE_THREAD(name) tracy::SetThreadName(name)
#define TY_PROFILE_FRAME() FrameMark
#define TY_PROFILE_SCOPE() ZoneScoped
#define TY_PROFILE_SCOPE_NAMED(name) ZoneScopedN(name)
#define TY_PROFILE_PLOT(name, value) TracyPlot(name, value)
#define TY_PROFILE_MESSAGE(text) TracyMessageL(text)
#else
#define TY_PROFILE_THREAD(name) ((void)0)
#define TY_PROFILE_FRAME() ((void)0)
#define TY_PROFILE_SCOPE() ((void)0)
#define TY_PROFILE_SCOPE_NAMED(name) ((void)0)
#define TY_PROFILE_PLOT(name, value) ((void)0)
#define TY_PROFILE_MESSAGE(text) ((void)0)
#endif

namespace tynima::core {

// True when instrumentation is compiled into this build.
[[nodiscard]] constexpr bool profiling_compiled() noexcept {
#if defined(TYNIMA_PROFILE)
    return true;
#else
    return false;
#endif
}

// True while a profiler is connected to this process.
[[nodiscard]] bool profiler_connected() noexcept;

} // namespace tynima::core
