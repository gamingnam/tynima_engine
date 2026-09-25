// The templates `tynima new` copies, played. Each one is loaded into a
// headless runtime with the keys a player would hold, and what the game
// does with them is checked: a template is the first thing anyone runs, and
// the only way to know it still runs is to run it.
//
// The scripts are read straight out of sdk/templates, {{name}} and all —
// that placeholder only ever appears in a comment or a string, so a
// template is valid Lua before it is copied anywhere.
#include <tynima/core/bytes.h>
#include <tynima/physics/physics.h>
#include <tynima/platform/file.h>
#include <tynima/platform/input_log.h>
#include <tynima/scene/world.h>
#include <tynima/script/vm.h>
#include <tynima/sdk/runtime.h>

#include <doctest/doctest.h>

#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

#ifndef TYNIMA_TEMPLATE_DIR
#define TYNIMA_TEMPLATE_DIR "sdk/templates"
#endif

namespace platform = tynima::platform;
namespace script = tynima::script;
namespace sdk = tynima::sdk;

namespace {

using platform::Key;

// A key held over a range of frames, with the two edges a real keyboard
// would have sent at its ends.
struct Hold {
    long from, to; // frames [from, to)
    Key key;
};

struct Plan {
    std::vector<Hold> holds;
    long click_frame = -1; // the left mouse button, once
    long look_frame = -1;  // one flick of the mouse, in points
    float look_dx = 0.0f, look_dy = 0.0f;
};

platform::InputFrame play(long frame, float dt, void* user) {
    const Plan& plan = *static_cast<const Plan*>(user);
    platform::InputFrame input;
    input.dt = dt;
    for (const Hold& hold : plan.holds) {
        const auto i = static_cast<std::size_t>(hold.key);
        if (frame >= hold.from && frame < hold.to) {
            input.keys_down.set(i);
            if (frame == hold.from) {
                input.keys_pressed.set(i);
            }
        } else if (frame == hold.to) {
            input.keys_released.set(i);
        }
    }
    if (frame == plan.click_frame) {
        input.buttons_down.set(static_cast<std::size_t>(platform::MouseButton::Left));
        input.buttons_pressed.set(static_cast<std::size_t>(platform::MouseButton::Left));
    }
    if (frame == plan.look_frame) {
        input.mouse_dx = plan.look_dx;
        input.mouse_dy = plan.look_dy;
    }
    return input;
}

std::string template_script(const char* name) {
    return std::string(TYNIMA_TEMPLATE_DIR) + "/" + name + "/src/game.lua";
}

// The world as the game sees it: the same ty.each a script uses.
std::string ask(script::Vm& vm, const char* lua) {
    std::string out;
    REQUIRE_MESSAGE(vm.eval(lua, out), out);
    return out;
}

float number(script::Vm& vm, const char* lua) {
    return std::strtof(ask(vm, lua).c_str(), nullptr);
}

// Where the entity called `what` is, along one axis: the world read the way
// the game itself reads it.
std::string position_of(const char* what, char axis) {
    return std::string("local ty, ffi = require('tynima'), require('ffi')\n"
                       "local found = ty.vec3(0, 0, 0)\n"
                       "ty.each('Name', 'Transform', function(_, name, transform)\n"
                       "  if ffi.string(name.text) == '") +
           what + "' then found = ty.vec3(transform.position) end\nend)\n" +
           "return string.format('%.4f', found." + axis + ")";
}

void run(sdk::Runtime& runtime, int frames) {
    for (int frame = 0; frame < frames; ++frame) {
        REQUIRE(runtime.begin_frame());
        runtime.end_frame();
    }
}

bool start(sdk::Runtime& runtime, const std::string& path, Plan& plan) {
    sdk::RuntimeDesc desc;
    desc.headless = true;
    desc.max_entities = 256;
    desc.script = path.c_str();
    desc.scripted_input = play;
    desc.scripted_input_user = &plan;
    desc.asset_poll_seconds = 0.0f; // a script that changes is seen next frame
    REQUIRE(runtime.create(desc));
    REQUIRE(runtime.script() != nullptr);
    REQUIRE_MESSAGE(runtime.script()->loaded(), runtime.script()->last_error());
    return true;
}

} // namespace

TEST_CASE("the platformer template runs, and holds a player up") {
    if (!script::Vm::available()) {
        MESSAGE("built without a script VM");
        return;
    }
    // D for half a second, then Space, then nothing: run, jump, land.
    Plan plan{.holds = {{0, 24, Key::D}, {24, 54, Key::Space}}};
    sdk::Runtime runtime;
    REQUIRE(start(runtime, template_script("platformer"), plan));
    script::Vm& vm = *runtime.script();

    // The floor is 0.5 m thick and the player 0.5 m to the middle: 1.0 m is
    // where they come to rest, whatever the solver does on the way.
    run(runtime, 24);
    const float resting = number(vm, position_of("player", 'y').c_str());
    CHECK(resting == doctest::Approx(1.0f).epsilon(0.05));
    // Half a second of running, from a standing start at x = 0.
    const float ran = number(vm, position_of("player", 'x').c_str());
    CHECK(ran > 1.5f);
    CHECK(ran < 5.0f); // still on the block they started on

    // Space, and a quarter of a second later they are well off the ground.
    run(runtime, 15);
    CHECK(number(vm, position_of("player", 'y').c_str()) > resting + 0.9f);
    // The jump is 1.4 m, and what goes up comes back to the same floor.
    run(runtime, 70);
    CHECK(number(vm, position_of("player", 'y').c_str()) == doctest::Approx(resting).epsilon(0.05));

    // A 2D game stays on its plane, whatever it collides with.
    CHECK(number(vm, position_of("player", 'z').c_str()) == doctest::Approx(0.0f).epsilon(0.01));
}

TEST_CASE("the first-person template walks, is stopped by a wall, and jumps") {
    if (!script::Vm::available()) {
        return;
    }
    // S walks backwards, into the wall at z = 13; then a jump.
    Plan plan{.holds = {{0, 90, Key::S}, {95, 120, Key::Space}}};
    sdk::Runtime runtime;
    REQUIRE(start(runtime, template_script("fps"), plan));
    script::Vm& vm = *runtime.script();

    run(runtime, 20);
    const float resting = number(vm, position_of("player", 'y').c_str());
    CHECK(resting == doctest::Approx(0.85f).epsilon(0.06)); // half a capsule above the floor

    run(runtime, 70);
    const float z = number(vm, position_of("player", 'z').c_str());
    CHECK(z > 11.0f); // they started at 9 and walked back
    CHECK(z < 12.5f); // and the wall stopped them rather than letting them through
    CHECK(number(vm, position_of("player", 'x').c_str()) == doctest::Approx(0.0f).epsilon(0.2));

    run(runtime, 20);
    CHECK(number(vm, position_of("player", 'y').c_str()) > resting + 0.3f);
}

TEST_CASE("the first-person template's gun is a ray and an impulse") {
    if (!script::Vm::available()) {
        return;
    }
    // Look down at the ball two metres ahead, then shoot it. 0.0022 radians
    // a point: 211 points is the 27 degrees from the eye down to its middle.
    Plan plan;
    plan.look_frame = 10;
    plan.look_dy = 211.0f;
    plan.click_frame = 20;
    sdk::Runtime runtime;
    REQUIRE(start(runtime, template_script("fps"), plan));
    script::Vm& vm = *runtime.script();

    run(runtime, 20);
    // The ball nearest the player is the one at z = 7; the others are at 2.
    const char* furthest_ball = "local ty, ffi = require('tynima'), require('ffi')\n"
                                "local z = -100\n"
                                "ty.each('Name', 'Transform', function(_, name, transform)\n"
                                "  if ffi.string(name.text) == 'ball' then\n"
                                "    z = math.max(z, transform.position.z) end\n"
                                "end)\n"
                                "return string.format('%.4f', z)";
    CHECK(number(vm, furthest_ball) == doctest::Approx(7.0f).epsilon(0.02));
    CHECK(number(vm, "return progress.shots") == 0.0f);

    run(runtime, 40);
    CHECK(number(vm, "return progress.shots") == 1.0f);
    CHECK(number(vm, "return progress.hits") == 1.0f); // it met something loose, not the room
    CHECK(number(vm, furthest_ball) < 6.0f);           // and shoved it away
}

TEST_CASE("a template takes its world down before it builds it again") {
    if (!script::Vm::available()) {
        return;
    }
    // Every template keeps a list of what it made and destroys it in
    // unload(), because a reload runs load() again on the world that is
    // already standing. Without that, a save would leave a second level
    // inside the first — which is exactly what this counts.
    const char* tmp = std::getenv("TMPDIR");
    const std::string dir = std::string(tmp != nullptr ? tmp : "/tmp") + "/tynima_template_test";
    REQUIRE(platform::make_directories(dir.c_str()));
    const std::string copy = dir + "/game.lua";

    for (const char* which : {"basic", "fps", "platformer"}) {
        INFO("the ", which, " template");
        // Out of the tree first: a reload is a write, and a test does not
        // write to the source it is testing.
        tynima::core::Bytes source;
        REQUIRE(platform::read_file(template_script(which).c_str(), source));
        const std::string text(reinterpret_cast<const char*>(source.data()), source.size());
        REQUIRE(platform::write_file(copy.c_str(), text.data(), text.size()));

        Plan plan;
        sdk::Runtime runtime;
        REQUIRE(start(runtime, copy, plan));
        run(runtime, 10);
        const std::uint32_t entities = runtime.world().entity_count();
        const std::size_t bodies = runtime.physics().body_count();
        CHECK(entities > 1);
        CHECK(bodies > 1);

        // The file changes, in a way that changes nothing about the game.
        const std::string touched = text + "\n-- a line added while it was running\n";
        REQUIRE(platform::write_file(copy.c_str(), touched.data(), touched.size()));
        bool reloaded = false;
        for (int frame = 0; frame < 8 && !reloaded; ++frame) {
            REQUIRE(runtime.begin_frame());
            runtime.end_frame();
            reloaded = runtime.stats().script_reloads > 0;
        }
        REQUIRE_MESSAGE(reloaded, runtime.script()->last_error());
        CHECK(runtime.world().entity_count() == entities);
        CHECK(runtime.physics().body_count() == bodies);
    }
    (void)platform::remove_file(copy.c_str());
}
