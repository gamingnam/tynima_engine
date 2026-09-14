// The sandbox's game module: built as a shared library, loaded by the sandbox
// at run time through sdk::GameModule, and reloaded whenever this file is
// rebuilt — while the sandbox keeps running.
//
// Try it: with the sandbox open, change kAxis below to Vec3::unit_x(), or
// double the speed, save, and run
//     cmake --build --preset macos-debug --target tynima_sandbox_game
// The bottles change their minds within a second; nothing else restarts.
//
// Rules of the road: this module links nothing from the engine. It includes
// header-only pieces (math, component structs) and reaches the engine through
// the tynima_api table it is handed. All state lives in the World, because
// this library's globals die on every reload.
#include <tynima.h>

#include <tynima/core/math.h>
#include <tynima/scene/components.h>

#include "spin.h"

#include <cstdint>

using namespace tynima::math;
using tynima::scene::Transform;

namespace {

constexpr Vec3 kAxis = Vec3::unit_y(); // <- edit me, rebuild the module, watch
constexpr float kSpeedScale = 1.0f;    // <- or me

// Component ids are resolved on every load: they are the module's only
// per-load state, and they are derived, not owned.
tynima_component_id g_transform = 0;
tynima_component_id g_spin = 0;

void on_load(const tynima_api* api, tynima_engine* engine, bool reloaded) {
    g_transform = api->register_component(engine, Transform::kName, sizeof(Transform), alignof(Transform));
    g_spin = api->register_component(engine, Spin::kName, sizeof(Spin), alignof(Spin));
    api->log(engine, reloaded ? "hot reloaded: same world, new code" : "game module loaded");
}

void on_unload(const tynima_api* api, tynima_engine* engine, bool reloading) {
    api->log(engine, reloading ? "unloading for a reload" : "game module unloaded");
}

struct Frame {
    float time;
    float speed_scale;
};

void spin_chunk(void* user, const tynima_entity* /*entities*/, uint32_t count, void* const* columns) {
    const Frame& frame = *static_cast<const Frame*>(user);
    auto* transforms = static_cast<Transform*>(columns[0]);
    const auto* spins = static_cast<const Spin*>(columns[1]);
    for (uint32_t i = 0; i < count; ++i) {
        transforms[i].rotation =
            Quat::from_axis_angle(kAxis, frame.time * spins[i].radians_per_second * frame.speed_scale);
    }
}

void on_update(const tynima_api* api, tynima_engine* engine, float /*dt*/) {
    // Holding Space runs the spin at four times the speed: input, through the API.
    Frame frame{static_cast<float>(api->time_seconds(engine)),
                kSpeedScale * (api->key_down(engine, TYNIMA_KEY_Space) ? 4.0f : 1.0f)};
    const tynima_component_id ids[] = {g_transform, g_spin};
    api->each_chunk(engine, ids, 2, spin_chunk, &frame);
}

const tynima_game kGame{TYNIMA_API_VERSION, on_load, on_unload, on_update};

} // namespace

extern "C" TYNIMA_GAME_EXPORT const tynima_game* tynima_game_entry(void) {
    return &kGame;
}
