#include <tynima/platform/time.h>

#include <chrono>
#include <thread>

namespace tynima::platform {

namespace {

std::uint64_t steady_ns() noexcept {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

std::uint64_t origin_ns() noexcept {
    static const std::uint64_t origin = steady_ns();
    return origin;
}

} // namespace

std::uint64_t now_ns() noexcept {
    return steady_ns();
}

double now_seconds() noexcept {
    // Sequenced explicitly: on the first call origin_ns() initialises its
    // static, and operand evaluation order in `a - b` is unspecified.
    const std::uint64_t origin = origin_ns();
    const std::uint64_t now = steady_ns();
    return static_cast<double>(now - origin) * 1e-9;
}

void sleep_ns(std::uint64_t ns) noexcept {
    std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
}

} // namespace tynima::platform
