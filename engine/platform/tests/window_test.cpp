#include <doctest/doctest.h>
#include <tynima/platform/events.h>
#include <tynima/platform/input.h>
#include <tynima/platform/platform.h>
#include <tynima/platform/time.h>
#include <tynima/platform/window.h>

#include <vector>

namespace platform = tynima::platform;

TEST_CASE("headless platform: init, window, pump, shutdown") {
    REQUIRE(platform::init({.headless = true}));
    CHECK(platform::is_initialized());
    CHECK(platform::init()); // idempotent

    auto window = platform::Window::create({.title = "test", .width = 640, .height = 480});
    REQUIRE(window != nullptr);
    CHECK(window->id() != 0);
    CHECK(window->width() == 640);
    CHECK(window->height() == 480);
    CHECK(window->pixel_width() >= 640);
    CHECK(window->pixel_height() >= 480);
    CHECK(window->pixel_density() >= 1.0f);
    CHECK(window->native_handle() != nullptr);
    window->set_title("renamed");

    platform::Input input;
    std::vector<platform::Event> events;
    platform::pump_events(input, events);
    for (const auto& event : events) {
        CHECK(event.type != platform::EventType::Quit);
        CHECK(event.type != platform::EventType::WindowClose);
    }

    window.reset();
    platform::shutdown();
    CHECK_FALSE(platform::is_initialized());
    platform::shutdown(); // idempotent
}

TEST_CASE("creating a window before init fails with an explanation") {
    CHECK_FALSE(platform::is_initialized());
    auto window = platform::Window::create({});
    CHECK(window == nullptr);
    CHECK(platform::last_error()[0] != '\0');
}

TEST_CASE("time is monotonic") {
    const auto a = platform::now_ns();
    const double s0 = platform::now_seconds();
    platform::sleep_ns(1'000'000); // 1 ms
    const auto b = platform::now_ns();
    CHECK(b >= a + 500'000);
    CHECK(platform::now_seconds() > s0);
}
