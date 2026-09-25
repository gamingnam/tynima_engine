// The tutorial's program, played. docs/first-game.md teaches by building
// one game up a piece at a time and ends with the whole of it; this finds
// that listing, writes it out and runs it headless. Documentation that has
// quietly stopped working is worse than none, and a tutorial is the one
// page where every line has to still be true.
#include <tynima/core/bytes.h>
#include <tynima/physics/physics.h>
#include <tynima/platform/file.h>
#include <tynima/platform/input_log.h>
#include <tynima/scene/world.h>
#include <tynima/script/vm.h>
#include <tynima/sdk/runtime.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace platform = tynima::platform;
namespace script = tynima::script;
namespace sdk = tynima::sdk;

#ifndef TYNIMA_DOCS_DIR
#define TYNIMA_DOCS_DIR "docs"
#endif

namespace {

// The fence the tutorial marks its whole program with: ```lua first-game.
// Every other block on the page is a piece of it, out of context on purpose.
//
// Carriage returns come off first: git hands a text file to Windows with
// them in, and a fence looked for by "```lua first-game\n" is then never
// found on the one platform where the file is spelled that way.
std::string program_in(std::string markdown) {
    markdown.erase(std::remove(markdown.begin(), markdown.end(), '\r'), markdown.end());
    const std::string open = "```lua first-game\n";
    const std::size_t begin = markdown.find(open);
    if (begin == std::string::npos) {
        return "";
    }
    const std::size_t from = begin + open.size();
    const std::size_t end = markdown.find("\n```", from);
    return end == std::string::npos ? "" : markdown.substr(from, end - from);
}

// A player leaning on D for the whole run: enough to prove the paddle is
// driven by the keyboard and stops where the script says it stops.
platform::InputFrame hold_right(long, float dt, void*) {
    platform::InputFrame input;
    input.dt = dt;
    input.keys_down.set(static_cast<std::size_t>(platform::Key::D));
    return input;
}

std::string temp_path(const char* name) {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) {
        dir = "/tmp";
    }
    return (dir / name).lexically_normal().generic_string();
}

} // namespace

TEST_CASE("the program the tutorial ends with is a game that runs") {
    if (!script::Vm::available()) {
        MESSAGE("built without a script VM");
        return;
    }
    tynima::core::Bytes page;
    const std::string markdown = std::string(TYNIMA_DOCS_DIR) + "/first-game.md";
    REQUIRE_MESSAGE(platform::read_file(markdown.c_str(), page), markdown);
    const std::string program =
        program_in(std::string(reinterpret_cast<const char*>(page.data()), page.size()));
    REQUIRE_MESSAGE(!program.empty(), "docs/first-game.md has no ```lua first-game block");

    const std::string path = temp_path("tynima_tutorial_game.lua");
    REQUIRE(platform::write_file(path.c_str(), program.data(), program.size()));

    sdk::RuntimeDesc desc;
    desc.headless = true;
    desc.max_entities = 128;
    desc.script = path.c_str();
    desc.scripted_input = hold_right;
    sdk::Runtime runtime;
    REQUIRE(runtime.create(desc));
    REQUIRE(runtime.script() != nullptr);
    REQUIRE_MESSAGE(runtime.script()->loaded(), runtime.script()->last_error());

    // The ground and the paddle, before a frame has run.
    CHECK(runtime.world().entity_count() == 2);

    // A second in: the first cube has been dropped, and is falling.
    for (int frame = 0; frame < 60; ++frame) {
        REQUIRE(runtime.begin_frame());
        runtime.end_frame();
    }
    std::string out;
    const char* count_cubes = "local ty, ffi = require('tynima'), require('ffi')\n"
                              "local cubes = 0\n"
                              "ty.each('Name', 'RigidBody', function(_, name)\n"
                              "  if ffi.string(name.text) == 'cube' then cubes = cubes + 1 end\n"
                              "end)\n"
                              "return cubes";
    REQUIRE_MESSAGE(runtime.script()->eval(count_cubes, out), out);
    CHECK(out == "1");

    // Four more seconds: cubes keep coming, and each one is taken out of
    // the world again as it reaches the paddle — so the world does not grow
    // without end, which is what a leak in the script would look like.
    for (int frame = 0; frame < 240; ++frame) {
        REQUIRE(runtime.begin_frame());
        runtime.end_frame();
    }
    CHECK(std::string(runtime.script()->last_error()).empty());
    CHECK(runtime.world().entity_count() <= 5);
    CHECK(runtime.physics().body_count() <= 3);

    // And D has pushed the paddle to the far end of its reach, where the
    // script's own clamp holds it.
    const char* paddle_x = "local ty, ffi = require('tynima'), require('ffi')\n"
                           "local x = 0\n"
                           "ty.each('Name', 'Transform', function(_, name, transform)\n"
                           "  if ffi.string(name.text) == 'paddle' then x = transform.position.x end\n"
                           "end)\n"
                           "return string.format('%.2f', x)";
    REQUIRE_MESSAGE(runtime.script()->eval(paddle_x, out), out);
    CHECK(std::strtof(out.c_str(), nullptr) == doctest::Approx(5.5f));

    (void)platform::remove_file(path.c_str());
}
