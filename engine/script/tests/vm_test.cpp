#include <tynima/script/vm.h>

#include <doctest/doctest.h>

#include <string>

using tynima::script::Vm;
using tynima::script::VmDesc;

// A VM with no engine behind it: what it can do without one is load a
// script, run it, and say what went wrong. Binding a world to it is the
// sdk's test (sdk/tests/script_test.cpp), since the world is above here.
TEST_CASE("a script is loaded, run, and reported on when it is wrong") {
    if (!Vm::available()) {
        MESSAGE("built without a script VM: " << Vm::last_create_error());
        return;
    }
    VmDesc desc; // no api, no engine: tynima.lua binds two null pointers
    const std::unique_ptr<Vm> vm = Vm::create(desc);
    REQUIRE_MESSAGE(vm != nullptr, Vm::last_create_error());
    CHECK_FALSE(vm->loaded());

    // Lua runs, and the result comes back.
    std::string out;
    REQUIRE_MESSAGE(vm->eval("return 6 * 7, 'and text'", out), out);
    CHECK(out == "42\tand text");
    CHECK(vm->eval("return jit.arch", out));
    CHECK_FALSE(out.empty());
    // The FFI is there, and so are the generated declarations: the sizes
    // LuaJIT computes are the ones the C compiler did.
    REQUIRE(vm->eval("local ffi = require('ffi'); require('tynima_ffi'); "
                     "return ffi.sizeof('tynima_api'), ffi.sizeof('tynima_transform'), "
                     "ffi.sizeof('tynima_body_desc'), ffi.offsetof('tynima_body_desc', 'user_data')",
                     out));
    CHECK(out == std::to_string(sizeof(void*) * 49) + "\t40\t120\t112");

    // A script must return the table of what to call.
    CHECK_FALSE(vm->load_source("local x = 1", "no-table"));
    CHECK(std::string(vm->last_error()).find("must return a table") != std::string::npos);
    // A syntax error is reported with the name the source was given.
    CHECK_FALSE(vm->load_source("this is not lua", "broken"));
    CHECK(std::string(vm->last_error()).find("broken") != std::string::npos);
    // So is an error while it runs, with a traceback.
    CHECK_FALSE(vm->load_source("error('thrown on purpose')", "thrower"));
    CHECK(std::string(vm->last_error()).find("thrown on purpose") != std::string::npos);
    CHECK(std::string(vm->last_error()).find("stack traceback") != std::string::npos);
    CHECK_FALSE(vm->loaded());

    // A whole script: load, update, unload, and state that lives between them.
    REQUIRE(vm->load_source(R"(
        local game = {}
        counted = 0
        function game.load(reloaded) counted = reloaded and 100 or 0 end
        function game.update(dt) counted = counted + dt end
        function game.unload() counted = -1 end
        return game
    )",
                            "counter"));
    CHECK(vm->loaded());
    CHECK(vm->load_count() == 1);
    REQUIRE(vm->eval("return counted", out));
    CHECK(out == "0");
    vm->update(0.5f);
    vm->update(0.25f);
    REQUIRE(vm->eval("return counted", out));
    CHECK(out == "0.75");

    // A script that throws in update says so once and then stops.
    REQUIRE(
        vm->load_source("local g = {}; function g.update() error('every frame') end; return g", "thrower"));
    vm->update(0.016f);
    CHECK(std::string(vm->last_error()).find("every frame") != std::string::npos);
    vm->update(0.016f); // no second complaint, and no crash

    // Loading again runs the old script's unload first.
    REQUIRE(vm->load_source(
        "local g = {}; function g.load(reloaded) counted = reloaded and 100 or 0 end; return g", "again"));
    CHECK(vm->load_count() == 3);
    REQUIRE(vm->eval("return counted", out));
    CHECK(out == "100"); // the second load knows it is one
    vm->unload();
    CHECK_FALSE(vm->loaded());

    // A file that is not there is a message, not a crash.
    CHECK_FALSE(vm->load("/no/such/script.lua"));
    CHECK(std::string(vm->last_error()).find("cannot read") != std::string::npos);
}

TEST_CASE("the engine's own Lua is there, and its vectors behave like vectors") {
    if (!Vm::available()) {
        return;
    }
    const std::unique_ptr<Vm> vm = Vm::create({});
    REQUIRE(vm != nullptr);
    std::string out;
    REQUIRE_MESSAGE(vm->eval(R"(
        local ty = require('tynima')
        local a = ty.vec3(1, 2, 3)
        local b = ty.vec3{0, 1, 0}
        local sum = a + b
        local scaled = a * 2
        return tostring(sum), tostring(scaled), a:dot(b), tostring(a:cross(b)), ty.vec3(3, 4, 0):length()
    )",
                             out),
                    out);
    CHECK(out == "(1.000, 3.000, 3.000)\t(2.000, 4.000, 6.000)\t2\t(-3.000, 0.000, 1.000)\t5");

    // A rotation turns a vector the way the engine's would: half a turn
    // about y takes -z to +z, to the last place a float keeps.
    REQUIRE(vm->eval(R"(
        local ty = require('tynima')
        local turned = ty.quat({0, 1, 0}, math.pi):rotate(ty.vec3(0, 0, -1))
        local near = function(a, b) return math.abs(a - b) < 1e-6 end
        return near(turned.x, 0) and near(turned.y, 0) and near(turned.z, 1)
    )",
                     out));
    CHECK(out == "true");

    // Shapes are made for both the body and the model that draws it.
    REQUIRE(vm->eval(R"(
        local ty = require('tynima')
        local box, ball, pill = ty.box(1, 2, 3), ty.sphere(0.25), ty.capsule(0.3, 0.9)
        return box.half_extents.y, box.half_extents.z, ball.radius,
               string.format('%.2f', pill.half_height), tonumber(pill.type)
    )",
                     out));
    CHECK(out == "2\t3\t0.25\t0.90\t2");
}
