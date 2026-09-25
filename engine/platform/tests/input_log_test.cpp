#include <doctest/doctest.h>
#include <tynima/platform/input.h>
#include <tynima/platform/input_log.h>

#include <cstdio>
// For the bitsets below: doctest prints both sides of a CHECK that fails,
// and the MSVC standard library's operator<<(ostream&, const bitset&) needs
// a complete basic_ostream where it is instantiated. libc++ has one by then
// and MSVC does not, which is a red Windows build and a green macOS one.
#include <ostream>
#include <string>

using namespace tynima::platform;

namespace {

std::string scratch_path(const char* name) {
    const char* dir = std::getenv("TMPDIR");
    return std::string(dir != nullptr ? dir : ".") + "/" + name;
}

} // namespace

TEST_CASE("an input log round-trips every frame, bit for bit, through a text file") {
    InputLog log;
    log.backend = "Tynima";
    // Frame 1: W goes down, the mouse moves.
    Input input;
    Input::Writer::begin_frame(input);
    Input::Writer::key(input, Key::W, true, false);
    Input::Writer::mouse_move(input, 100.5f, 20.25f, 3.0f, -1.5f);
    log.frames.push_back(capture_frame(input, 0.0166667f));
    // Frame 2: W held (repeat), Space tapped within the frame, a click, a scroll.
    Input::Writer::begin_frame(input);
    Input::Writer::key(input, Key::W, true, true);
    Input::Writer::key(input, Key::Space, true, false);
    Input::Writer::key(input, Key::Space, false, false);
    Input::Writer::mouse_button(input, MouseButton::Right, true);
    Input::Writer::wheel(input, 0.0f, 2.0f);
    log.frames.push_back(capture_frame(input, 0.0333333f));
    // Frame 3: everything released, nothing else — the shortest line there is.
    Input::Writer::begin_frame(input);
    Input::Writer::key(input, Key::W, false, false);
    Input::Writer::mouse_button(input, MouseButton::Right, false);
    log.frames.push_back(capture_frame(input, 1.0f / 3.0f));
    log.steps = 42;
    log.state_hash = 0x0123456789abcdefull;
    log.has_end = true;

    const std::string path = scratch_path("input_log_test.tyrec");
    std::string error;
    REQUIRE_MESSAGE(log.save(path.c_str(), error), error);

    InputLog back;
    REQUIRE_MESSAGE(back.load(path.c_str(), error), error);
    CHECK(back.backend == "Tynima");
    CHECK(back.has_end);
    CHECK(back.steps == 42);
    CHECK(back.state_hash == 0x0123456789abcdefull);
    REQUIRE(back.frames.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        const InputFrame& a = log.frames[i];
        const InputFrame& b = back.frames[i];
        CHECK(a.dt == b.dt); // exactly: hex floats
        CHECK(a.keys_down == b.keys_down);
        CHECK(a.keys_pressed == b.keys_pressed);
        CHECK(a.keys_released == b.keys_released);
        CHECK(a.buttons_down == b.buttons_down);
        CHECK(a.buttons_pressed == b.buttons_pressed);
        CHECK(a.buttons_released == b.buttons_released);
        CHECK(a.mouse_x == b.mouse_x);
        CHECK(a.mouse_dy == b.mouse_dy);
        CHECK(a.wheel_y == b.wheel_y);
    }

    // Played back into a fresh Input, the frames read exactly as they did live.
    Input replay;
    apply_frame(back.frames[0], replay);
    CHECK(replay.key_down(Key::W));
    CHECK(replay.key_pressed(Key::W));
    CHECK(replay.mouse_dx() == 3.0f);
    CHECK(replay.mouse_x() == 100.5f);
    apply_frame(back.frames[1], replay);
    CHECK(replay.key_down(Key::W));
    CHECK_FALSE(replay.key_pressed(Key::W)); // held, not re-pressed
    CHECK(replay.key_pressed(Key::Space));
    CHECK(replay.key_released(Key::Space)); // both edges of the tap
    CHECK_FALSE(replay.key_down(Key::Space));
    CHECK(replay.mouse_pressed(MouseButton::Right));
    CHECK(replay.wheel_y() == 2.0f);
    CHECK(replay.mouse_dx() == 0.0f); // this frame had no motion
    apply_frame(back.frames[2], replay);
    CHECK(replay.key_released(Key::W));
    CHECK_FALSE(replay.key_down(Key::W));
    CHECK(replay.mouse_released(MouseButton::Right));
    CHECK_FALSE(replay.key_pressed(Key::Space));
    std::remove(path.c_str());
}

TEST_CASE("a log that is not a log is refused, with the line") {
    const std::string path = scratch_path("input_log_bad.tyrec");
    std::FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    std::fputs("tynima-input-log 1\nbackend Tynima\nframe 0x1p-6 k=zz\n", f);
    std::fclose(f);
    InputLog log;
    std::string error;
    CHECK_FALSE(log.load(path.c_str(), error));
    CHECK(error.find(":3:") != std::string::npos);
    CHECK_FALSE(log.load("/nonexistent/none.tyrec", error));
    std::remove(path.c_str());
    // An unfinished recording loads, and says so.
    f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    std::fputs("tynima-input-log 1\nbackend Jolt\nframe 0x1p-6\n", f);
    std::fclose(f);
    REQUIRE(log.load(path.c_str(), error));
    CHECK_FALSE(log.has_end);
    CHECK(log.frames.size() == 1);
    CHECK(log.backend == "Jolt");
    std::remove(path.c_str());
}
