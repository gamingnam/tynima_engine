// The sandbox's game module: built as a shared library, loaded by the sandbox
// at run time through sdk::GameModule, and reloaded whenever this file is
// rebuilt — while the sandbox keeps running.
//
// Try it: with the sandbox open, press Space to launch the pile. Then change
// kLaunchSpeed below (or the spread), save, and run
//     cmake --build --preset macos-debug --target tynima_sandbox_game
// Press Space again: the new numbers are live within a second, and nothing
// else restarted — same world, same pile, same camera.
//
// Rules of the road: this module links nothing from the engine. It includes
// header-only pieces (math, component structs) and reaches the engine through
// the tynima_api table it is handed. All state lives in the World, because
// this library's globals die on every reload.
#include <tynima.h>

#include <tynima/core/math.h>
#include <tynima/scene/components.h>

#include <cstdint>

using namespace tynima::math;
using tynima::scene::RigidBody;
using tynima::scene::Transform;

namespace {

constexpr float kLaunchSpeed = 4.0f;  // <- edit me: metres per second straight up
constexpr float kLaunchSpread = 1.5f; // <- or me: how hard the pile is pushed apart
constexpr float kBottleMass = 0.6f;   // what the sandbox gives each body (kg); impulse = mass * speed

// Component ids are resolved on every load: they are the module's only
// per-load state, and they are derived, not owned.
tynima_component_id g_transform = 0;
tynima_component_id g_rigid_body = 0;

void on_load(const tynima_api* api, tynima_engine* engine, bool reloaded) {
    g_transform = api->register_component(engine, Transform::kName, sizeof(Transform), alignof(Transform));
    g_rigid_body = api->register_component(engine, RigidBody::kName, sizeof(RigidBody), alignof(RigidBody));
    api->log(engine, TYNIMA_LOG_INFO, reloaded ? "hot reloaded: same world, new code" : "game module loaded");
}

void on_unload(const tynima_api* api, tynima_engine* engine, bool reloading) {
    api->log(engine, TYNIMA_LOG_INFO, reloading ? "unloading for a reload" : "game module unloaded");
}

struct Launch {
    const tynima_api* api;
    tynima_engine* engine;
};

// Every body gets a kick: up, and away from the pile's axis so it scatters.
void launch_chunk(void* user, const tynima_entity* /*entities*/, uint32_t count, void* const* columns) {
    const Launch& launch = *static_cast<const Launch*>(user);
    const auto* transforms = static_cast<const Transform*>(columns[0]);
    const auto* bodies = static_cast<const RigidBody*>(columns[1]);
    for (uint32_t i = 0; i < count; ++i) {
        const Vec3 p = transforms[i].position;
        const Vec3 outward = normalize(Vec3{p.x, 0.0f, p.z}); // zero at the exact centre: straight up
        const Vec3 velocity = Vec3{0.0f, kLaunchSpeed, 0.0f} + outward * kLaunchSpread;
        const tynima_body body{bodies[i].body.index, bodies[i].body.generation};
        launch.api->body_add_impulse(launch.engine, body,
                                     tynima_vec3{velocity.x * kBottleMass, velocity.y * kBottleMass,
                                                 velocity.z * kBottleMass});
    }
}

void on_update(const tynima_api* api, tynima_engine* engine, float /*dt*/) {
    if (!api->key_pressed(engine, TYNIMA_KEY_Space)) {
        return;
    }
    Launch launch{api, engine};
    const tynima_component_id ids[] = {g_transform, g_rigid_body};
    api->each_chunk(engine, ids, 2, launch_chunk, &launch);
    api->log(engine, TYNIMA_LOG_INFO, "launch!");
}

const tynima_game kGame{TYNIMA_API_VERSION, on_load, on_unload, on_update};

} // namespace

extern "C" TYNIMA_GAME_EXPORT const tynima_game* tynima_game_entry(void) {
    return &kGame;
}
