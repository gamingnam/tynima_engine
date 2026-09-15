#pragma once

#include <tynima/physics/physics.h>

#include <algorithm>
#include <cmath>

// The fixed-timestep loop, the way Glenn Fiedler wrote it up: the world only
// ever steps by `step` — so the simulation does the same thing at 30, 60 or
// 120 frames a second, and a replay of it is the same run — and the frame
// time left over carries into the next frame. Since the frame is rarely on a
// step boundary, what is drawn is interpolated between the last two states by
// alpha(): the scene records poses in before_step() and blends by it.
namespace tynima::physics {

struct FixedStepper {
    float step = 1.0f / 60.0f;
    // A frame that owes more steps than this (a hitch, a debugger pause)
    // drops the rest rather than simulating its way back, which would only
    // make the next frame later still — the spiral of death.
    int max_steps = 4;
    float accumulator = 0.0f;
    std::uint64_t total_steps = 0;

    // Advances the world through every whole step that `dt` seconds cover,
    // calling before_step(step) ahead of each: record what interpolation
    // starts from, drive controllers, apply the frame's input. Returns how
    // many steps ran, which can be zero.
    template <typename BeforeStep>
    int advance(PhysicsWorld& world, float dt, BeforeStep&& before_step) {
        accumulator += std::max(dt, 0.0f);
        const float most = static_cast<float>(max_steps) * step;
        if (accumulator > most) {
            accumulator = most;
        }
        int steps = 0;
        while (accumulator >= step) {
            before_step(step);
            world.step(step);
            accumulator -= step;
            ++steps;
            ++total_steps;
        }
        return steps;
    }

    int advance(PhysicsWorld& world, float dt) {
        return advance(world, dt, [](float) {});
    }

    // How far the frame is between the last step's state and the next one's,
    // in [0, 1): what to draw is that fraction of the way from the pose
    // before the last step to the pose after it.
    [[nodiscard]] float alpha() const noexcept { return std::clamp(accumulator / step, 0.0f, 1.0f); }
};

} // namespace tynima::physics
