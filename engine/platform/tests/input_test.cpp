#include <doctest/doctest.h>
#include <tynima/platform/input.h>

#include <cstring>

using tynima::platform::Input;
using tynima::platform::Key;
using tynima::platform::MouseButton;
using Writer = tynima::platform::Input::Writer;

TEST_CASE("a key press is an edge for one frame and a level until released") {
    Input input;
    Writer::begin_frame(input);
    Writer::key(input, Key::W, true, false);
    CHECK(input.key_down(Key::W));
    CHECK(input.key_pressed(Key::W));
    CHECK_FALSE(input.key_released(Key::W));

    Writer::begin_frame(input); // held across the frame boundary
    CHECK(input.key_down(Key::W));
    CHECK_FALSE(input.key_pressed(Key::W));

    Writer::key(input, Key::W, false, false);
    CHECK_FALSE(input.key_down(Key::W));
    CHECK(input.key_released(Key::W));
}

TEST_CASE("key repeat keeps the key down but never re-triggers pressed") {
    Input input;
    Writer::begin_frame(input);
    Writer::key(input, Key::Space, true, false);
    Writer::begin_frame(input);
    Writer::key(input, Key::Space, true, true); // OS auto-repeat
    Writer::key(input, Key::Space, true, true);
    CHECK(input.key_down(Key::Space));
    CHECK_FALSE(input.key_pressed(Key::Space));
}

TEST_CASE("a press and release inside one frame reports both edges") {
    Input input;
    Writer::begin_frame(input);
    Writer::key(input, Key::Escape, true, false);
    Writer::key(input, Key::Escape, false, false);
    CHECK(input.key_pressed(Key::Escape));
    CHECK(input.key_released(Key::Escape));
    CHECK_FALSE(input.key_down(Key::Escape));
}

TEST_CASE("mouse deltas and wheel accumulate within a frame and reset on the next") {
    Input input;
    Writer::begin_frame(input);
    Writer::mouse_move(input, 10.0f, 20.0f, 3.0f, -1.0f);
    Writer::mouse_move(input, 12.0f, 18.0f, 2.0f, -2.0f);
    Writer::wheel(input, 0.0f, 1.0f);
    Writer::wheel(input, 0.0f, 0.5f);
    CHECK(input.mouse_x() == doctest::Approx(12.0f));
    CHECK(input.mouse_y() == doctest::Approx(18.0f));
    CHECK(input.mouse_dx() == doctest::Approx(5.0f));
    CHECK(input.mouse_dy() == doctest::Approx(-3.0f));
    CHECK(input.wheel_y() == doctest::Approx(1.5f));

    Writer::begin_frame(input);
    CHECK(input.mouse_x() == doctest::Approx(12.0f)); // position persists
    CHECK(input.mouse_dx() == doctest::Approx(0.0f)); // deltas do not
    CHECK(input.wheel_y() == doctest::Approx(0.0f));
}

TEST_CASE("mouse buttons have the same edge semantics as keys") {
    Input input;
    Writer::begin_frame(input);
    Writer::mouse_button(input, MouseButton::Right, true);
    CHECK(input.mouse_down(MouseButton::Right));
    CHECK(input.mouse_pressed(MouseButton::Right));
    CHECK_FALSE(input.mouse_down(MouseButton::Left));
    Writer::begin_frame(input);
    Writer::mouse_button(input, MouseButton::Right, false);
    CHECK(input.mouse_released(MouseButton::Right));
    CHECK_FALSE(input.mouse_down(MouseButton::Right));
}

TEST_CASE("key names are stable and out-of-range keys are harmless") {
    using tynima::platform::key_name;
    CHECK(std::strcmp(key_name(Key::Unknown), "Unknown") == 0);
    CHECK(std::strcmp(key_name(Key::A), "A") == 0);
    CHECK(std::strcmp(key_name(Key::Digit0), "Digit0") == 0);
    CHECK(std::strcmp(key_name(Key::RightSuper), "RightSuper") == 0);
    CHECK(std::strcmp(key_name(Key::Count), "Invalid") == 0);

    Input input;
    Writer::key(input, Key::Count, true, false); // ignored, not UB
    CHECK_FALSE(input.key_down(Key::Count));
}
