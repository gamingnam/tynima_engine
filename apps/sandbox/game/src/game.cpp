// The sandbox's game module: built as a shared library, loaded by the sandbox
// at run time through sdk::GameModule, and reloaded whenever this file is
// rebuilt — while the sandbox keeps running.
//
// Try it: with the sandbox open, press L to launch the pile. Then change
// kLaunchSpeed below (or the spread), save, and run
//     cmake --build --preset macos-debug --target tynima_sandbox_game
// Press L again: the new numbers are live within a second, and nothing
// else restarted — same world, same pile, same camera.
//
// Rules of the road: this module links nothing from the engine. It includes
// header-only pieces (math, component structs) and reaches the engine through
// the tynima_api table it is handed. All state lives in the World, because
// this library's globals die on every reload — the launch settings too: they
// are a component on a "launcher" entity, which the editor's inspector can
// edit live, and which the module finds again after a reload.
#include <tynima.h>

#include <tynima/core/math.h>
#include <tynima/core/reflect.h>
#include <tynima/scene/components.h>
#include <tynima/sdk/reflect.h>

#include <cstdint>

using namespace tynima::math;
using tynima::scene::Name;
using tynima::scene::RigidBody;
using tynima::scene::Transform;

namespace {

// The module's own component, described to the engine through the C API
// (sdk::register_component does that from the TY_REFLECT list), so a tool
// that has never heard of it still shows and edits its fields.
struct Launcher {
    static constexpr const char* kName = "Launcher";
    float speed = 4.0f;  // <- edit me: metres per second straight up
    float spread = 1.5f; // <- or me: how hard the pile is pushed apart
    std::uint32_t launches = 0;
};
TY_REFLECT(Launcher, TY_FIELD(speed), TY_FIELD(spread),
           TY_FIELD_FLAGS(launches, tynima::core::kFieldReadOnly));

constexpr float kBottleMass = 0.6f; // what the sandbox gives each body (kg); impulse = mass * speed

// Component ids are resolved on every load: they are the module's only
// per-load state, and they are derived, not owned.
tynima_component_id g_transform = 0;
tynima_component_id g_rigid_body = 0;
tynima_component_id g_launcher = 0;
tynima_component_id g_name = 0;

// The one launcher entity, found in the World (after a reload) or created.
Launcher* find_launcher(const tynima_api* api, tynima_engine* engine) {
    struct Found {
        Launcher* launcher = nullptr;
    } found;
    api->each_chunk(
        engine, &g_launcher, 1,
        [](void* user, const tynima_entity*, uint32_t count, void* const* columns) {
            auto* f = static_cast<Found*>(user);
            if (count > 0 && f->launcher == nullptr) {
                f->launcher = static_cast<Launcher*>(columns[0]);
            }
        },
        &found);
    return found.launcher;
}

void on_load(const tynima_api* api, tynima_engine* engine, bool reloaded) {
    g_transform = tynima::sdk::register_component<Transform>(*api, engine);
    g_rigid_body = tynima::sdk::register_component<RigidBody>(*api, engine);
    g_name = tynima::sdk::register_component<Name>(*api, engine);
    g_launcher = tynima::sdk::register_component<Launcher>(*api, engine);
    if (find_launcher(api, engine) == nullptr) {
        const Launcher launcher{};
        const Name name("launcher");
        const tynima_component_id ids[2] = {g_launcher, g_name};
        const void* values[2] = {&launcher, &name};
        (void)api->create_entity(engine, ids, values, 2);
    }
    api->log(engine, TYNIMA_LOG_INFO, reloaded ? "hot reloaded: same world, new code" : "game module loaded");
}

void on_unload(const tynima_api* api, tynima_engine* engine, bool reloading) {
    api->log(engine, TYNIMA_LOG_INFO, reloading ? "unloading for a reload" : "game module unloaded");
}

struct Launch {
    const tynima_api* api;
    tynima_engine* engine;
    float speed;
    float spread;
};

// Every body gets a kick: up, and away from the pile's axis so it scatters.
void launch_chunk(void* user, const tynima_entity* /*entities*/, uint32_t count, void* const* columns) {
    const Launch& launch = *static_cast<const Launch*>(user);
    const auto* transforms = static_cast<const Transform*>(columns[0]);
    const auto* bodies = static_cast<const RigidBody*>(columns[1]);
    for (uint32_t i = 0; i < count; ++i) {
        const Vec3 p = transforms[i].position;
        const Vec3 outward = normalize(Vec3{p.x, 0.0f, p.z}); // zero at the exact centre: straight up
        const Vec3 velocity = Vec3{0.0f, launch.speed, 0.0f} + outward * launch.spread;
        const tynima_body body{bodies[i].body.index, bodies[i].body.generation};
        launch.api->body_add_impulse(
            launch.engine, body,
            tynima_vec3{velocity.x * kBottleMass, velocity.y * kBottleMass, velocity.z * kBottleMass});
    }
}

void on_update(const tynima_api* api, tynima_engine* engine, float /*dt*/) {
    if (!api->key_pressed(engine, TYNIMA_KEY_L)) {
        return;
    }
    Launcher* launcher = find_launcher(api, engine);
    const Launcher defaults{};
    const Launcher& settings = launcher != nullptr ? *launcher : defaults;
    Launch launch{api, engine, settings.speed, settings.spread};
    const tynima_component_id ids[] = {g_transform, g_rigid_body};
    api->each_chunk(engine, ids, 2, launch_chunk, &launch);
    if (launcher != nullptr) {
        ++launcher->launches;
    }
    api->log(engine, TYNIMA_LOG_INFO, "launch!");
}

const tynima_game kGame{TYNIMA_API_VERSION, on_load, on_unload, on_update};

} // namespace

extern "C" TYNIMA_GAME_EXPORT const tynima_game* tynima_game_entry(void) {
    return &kGame;
}
