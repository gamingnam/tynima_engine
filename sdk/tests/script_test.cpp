#include <tynima.h>

#include <tynima/core/reflect.h>
#include <tynima/platform/file.h>
#include <tynima/platform/input_log.h>
#include <tynima/platform/time.h>
#include <tynima/scene/components.h>
#include <tynima/scene/systems.h>
#include <tynima/script/vm.h>
#include <tynima/sdk/reflect.h>
#include <tynima/sdk/runtime.h>

#include <doctest/doctest.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace scene = tynima::scene;
namespace sdk = tynima::sdk;
using namespace tynima::math;

namespace {

std::string temp_dir() {
    const char* dir = std::getenv("TMPDIR");
    return std::string(dir != nullptr ? dir : "/tmp") + "/tynima_script_test";
}

void write_script(const std::string& path, const std::string& source) {
    REQUIRE(tynima::platform::write_file(path.c_str(), source.data(), source.size()));
}

// A component the engine has never heard of, as a game module would define
// it: what a script can only reach because reflection describes it.
struct Launcher {
    static constexpr const char* kName = "Launcher";
    float speed = 4.0f;
    bool armed = true;
    std::uint32_t launches = 0;
    Vec3 aim{0.0f, 1.0f, 0.0f};
    char label[16] = "idle";
};
// Beside the type, in the same namespace: that is how it is found.
TY_REFLECT(Launcher, TY_FIELD(speed), TY_FIELD(armed), TY_FIELD(launches), TY_FIELD(aim), TY_FIELD(label));

} // namespace

TEST_CASE("a Lua script is a game module: it builds a world, runs every frame, and reloads") {
    if (!tynima::script::Vm::available()) {
        MESSAGE("built without a script VM");
        return;
    }
    const std::string dir = temp_dir();
    REQUIRE(tynima::platform::make_directories(dir.c_str()));
    const std::string path = dir + "/game.lua";
    write_script(path, R"(
        local ty = require("tynima")
        local game = {}
        state = state or {}

        function game.load(reloaded)
            state.reloaded = reloaded
            local ground = ty.shape_model(ty.box(8, 0.5, 8), {0.4, 0.4, 0.4})
            local ball = ty.shape_model(ty.sphere(0.5), {0.8, 0.2, 0.2}, 0.3)
            state.floor_body = ty.body{ shape = ty.box(8, 0.5, 8), position = {0, -0.5, 0},
                                        motion = "static", user_data = 7 }
            ty.entity{ Name = {text = "ground"}, Transform = {position = {0, -0.5, 0}},
                       MeshRenderer = {model = ground} }
            state.body = ty.body{ shape = ty.sphere(0.5), position = {0, 4, 0}, mass = 2, user_data = 11 }
            state.ball = ty.entity{ Name = {text = "ball"}, Transform = {position = {0, 4, 0}},
                                    MeshRenderer = {model = ball}, RigidBody = {body = state.body} }
            ty.lighting{ sun = {0.3, 1, 0.2}, intensity = 4, sky = {0.2, 0.3, 0.4} }
            ty.look_at({0, 3, 8}, {0, 1, 0})
            state.frames = 0
            ty.log("the script built the world")
        end

        function game.update(dt)
            state.frames = state.frames + 1
            state.time = ty.time()
            -- What a game does every frame: read a component and change it.
            local transform = ty.get(state.ball, "Transform")
            state.height = transform.position.y
        end

        function game.unload(reloading) state.unloaded = reloading end
        return game
    )");

    sdk::Runtime runtime;
    sdk::RuntimeDesc desc;
    desc.headless = true;
    desc.max_entities = 64;
    desc.max_bodies = 16;
    desc.script = path.c_str();
    desc.asset_poll_seconds = 0.0f; // a change is seen at the next frame
    REQUIRE(runtime.create(desc));
    tynima::script::Vm* vm = runtime.script();
    REQUIRE_MESSAGE(vm != nullptr, tynima::script::Vm::last_create_error());
    REQUIRE_MESSAGE(vm->loaded(), vm->last_error());

    // The script built the world through the same table a C module gets.
    CHECK(runtime.world().entity_count() == 2);
    // And what it built is drawable: an entity with a Transform and a mesh
    // has the LocalToWorld the renderer collects, without the script saying so.
    tynima::render::DrawItem draws[8];
    CHECK(scene::collect_draws(runtime.world(), runtime.model(0), runtime.model_count(), draws, 8) == 2);
    CHECK(runtime.model_count() == 2);
    CHECK(runtime.physics().body_count() == 2);
    CHECK(runtime.sun.intensity == 4.0f);
    CHECK(runtime.sky.z == doctest::Approx(0.4f));
    CHECK(runtime.camera.position.z == doctest::Approx(8.0f));
    // The camera looks at the point it was given: forward is towards it.
    const Vec3 forward = runtime.camera.forward();
    CHECK(forward.z < 0.0f);
    CHECK(forward.y < 0.0f);

    std::string out;
    REQUIRE(vm->eval("return state.reloaded", out));
    CHECK(out == "false");

    // Frames: the script runs, the ball falls, the script sees it fall.
    for (int frame = 0; frame < 30; ++frame) {
        REQUIRE(runtime.begin_frame());
        runtime.end_frame();
    }
    REQUIRE(vm->eval("return state.frames", out));
    CHECK(out == "30");
    REQUIRE(vm->eval("return state.height < 4", out));
    CHECK(out == "true");
    REQUIRE(vm->eval("return state.time > 0", out));
    CHECK(out == "true");

    // A ray, cast from Lua, finds the ball the script made.
    REQUIRE(vm->eval("local ty = require('tynima'); "
                     "local hit = ty.cast_ray({0, 10, 0}, {0, -1, 0}, 20); "
                     "return hit ~= nil and tonumber(hit.user_data) or -1",
                     out));
    CHECK(out == "11");

    // The file changes: the script is loaded again, and knows it.
    write_script(path, R"(
        local ty = require("tynima")
        local game = {}
        function game.load(reloaded) state.reloaded = reloaded; state.after = ty.entity_count() end
        function game.update(dt) state.frames = state.frames + 100 end
        return game
    )");
    bool reloaded = false;
    for (int frame = 0; frame < 4 && !reloaded; ++frame) {
        REQUIRE(runtime.begin_frame());
        runtime.end_frame();
        reloaded = runtime.stats().script_reloads > 0;
    }
    CHECK(reloaded);
    CHECK(vm->load_count() == 2);
    REQUIRE(vm->eval("return state.reloaded, state.unloaded, state.after", out));
    CHECK(out == "true\ttrue\t2"); // the world it built is still there
    REQUIRE(vm->eval("return state.frames >= 130", out));
    CHECK(out == "true");

    // Naming LocalToWorld keeps the script's own: the rule fills a gap, it
    // does not overrule. An entity with no Transform gets nothing either way,
    // and neither is drawn — one has no mesh, the other no transform.
    REQUIRE(vm->eval("local ty = require('tynima'); "
                     "ty.entity{ Name = {text = 'no transform'} }; "
                     "ty.entity{ Transform = {position = {1, 1, 1}}, LocalToWorld = {} }; "
                     "return ty.entity_count()",
                     out));
    CHECK(out == "4");
    CHECK(scene::collect_draws(runtime.world(), runtime.model(0), runtime.model_count(), draws, 8) == 2);

    (void)tynima::platform::remove_file(path.c_str());
}

namespace {

// A player holding W, as the platform would report it: what the sandbox's
// scripted input does, in one key.
tynima::platform::InputFrame hold_w(long, float dt, void*) {
    tynima::platform::InputFrame frame;
    frame.dt = dt;
    frame.keys_down.set(static_cast<std::size_t>(tynima::platform::Key::W));
    return frame;
}

} // namespace

TEST_CASE("input reaches a script and the camera it moves") {
    if (!tynima::script::Vm::available()) {
        return;
    }
    const std::string dir = temp_dir();
    REQUIRE(tynima::platform::make_directories(dir.c_str()));
    const std::string path = dir + "/fly.lua";
    write_script(path, R"(
        local ty = require("tynima")
        local game = {}
        function game.load()
            ty.set_camera{ position = {0, 2, 10}, rotation = ty.euler(0, 0, 0) }
            held = 0
        end
        function game.update(dt)
            if ty.key_down("W") then
                held = held + 1
                local camera = ty.camera()
                -- Identity looks down -z, so W walks that way.
                camera.position = ty.vec3(camera.position) + ty.vec3(0, 0, -6 * dt)
            end
        end
        return game
    )");

    sdk::Runtime runtime;
    sdk::RuntimeDesc desc;
    desc.headless = true;
    desc.max_entities = 16;
    desc.script = path.c_str();
    desc.scripted_input = hold_w;
    REQUIRE(runtime.create(desc));
    REQUIRE(runtime.script() != nullptr);
    REQUIRE_MESSAGE(runtime.script()->loaded(), runtime.script()->last_error());
    CHECK(runtime.camera.position.z == doctest::Approx(10.0f));

    for (int frame = 0; frame < 30; ++frame) {
        REQUIRE(runtime.begin_frame());
        runtime.end_frame();
    }
    std::string out;
    REQUIRE(runtime.script()->eval("return held", out));
    CHECK(out == "30"); // the key was down on every frame the script saw
    // Half a second at six metres a second: the camera has walked forward.
    CHECK(runtime.camera.position.z == doctest::Approx(7.0f).epsilon(0.05));
    CHECK(runtime.camera.position.y == doctest::Approx(2.0f));
    (void)tynima::platform::remove_file(path.c_str());
}

TEST_CASE("a script reads a component the engine never knew, through reflection alone") {
    if (!tynima::script::Vm::available()) {
        return;
    }
    sdk::Runtime runtime;
    sdk::RuntimeDesc desc;
    desc.headless = true;
    desc.max_entities = 32;
    REQUIRE(runtime.create(desc));
    // What a game module does on load: register its component and describe it.
    REQUIRE(sdk::register_component<Launcher>(*tynima_engine_api(&runtime.context()), &runtime.context()) !=
            TYNIMA_NO_COMPONENT);

    const std::unique_ptr<tynima::script::Vm> vm = tynima::script::Vm::create(
        {.api = tynima_engine_api(&runtime.context()), .engine = &runtime.context()});
    REQUIRE_MESSAGE(vm != nullptr, tynima::script::Vm::last_create_error());

    std::string out;
    // The script has never heard of Launcher; reflection tells it the shape.
    REQUIRE_MESSAGE(vm->eval(R"(
        local ty = require('tynima')
        local e = ty.entity{ Name = {text = 'launcher'}, Launcher = {speed = 9.5, aim = {1, 0, 0}} }
        local l = ty.get(e, 'Launcher')
        return l.speed, l.armed, tonumber(l.launches), tostring(l.aim), require('ffi').string(l.label)
    )",
                             out),
                    out);
    CHECK(out == "9.5\ttrue\t0\t(1.000, 0.000, 0.000)\tidle"); // the rest from the component's defaults

    // Writing through it reaches the world's own memory.
    REQUIRE(vm->eval(R"(
        local ty = require('tynima')
        local found
        ty.each('Launcher', function(entity, launcher)
            launcher.launches = launcher.launches + 3
            launcher.armed = false
            found = entity
        end)
        local again = ty.get(found, 'Launcher')
        return tonumber(again.launches), again.armed
    )",
                     out));
    CHECK(out == "3\tfalse");
    bool seen = false;
    runtime.world().each<Launcher>([&](scene::Entity, Launcher& launcher) {
        seen = true;
        CHECK(launcher.launches == 3);
        CHECK_FALSE(launcher.armed);
        CHECK(launcher.speed == doctest::Approx(9.5f));
        CHECK(launcher.aim.x == 1.0f);
        CHECK(std::string(launcher.label) == "idle");
    });
    CHECK(seen);

    // Every component the world knows, by name, and a name nothing answers to.
    REQUIRE(
        vm->eval("local ty = require('tynima'); return #ty.components(), ty.component('Launcher') ~= nil, "
                 "ty.component('Nonsense') == nil",
                 out));
    CHECK(out == "7\ttrue\ttrue");
}
