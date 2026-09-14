#pragma once

#include <tynima/core/math.h>
#include <tynima/physics/physics.h>

// A character controller on top of any PhysicsWorld: a capsule that never
// tips over, driven by the velocity the game asks for, held to the ground
// it stands on and free to fall off it. It is a real body, so it pushes
// things and things push it; it is not a rigid body in the way it moves,
// since a game character accelerates to walking speed in a frame and stops
// dead when the stick is released.
//
// Each frame, before the world steps: move(). After: on_ground(), position().
namespace tynima::physics {

struct CharacterDesc {
    float radius = 0.3f;
    float half_height = 0.5f; // of the straight part: 1.6 m tall in all
    float mass = 80.0f;
    math::Vec3 position{0.0f}; // of the capsule's centre
    float max_slope_degrees = 50.0f; // steeper than this is a wall, not ground
    float jump_speed = 5.0f;         // m/s straight up: about 1.3 m of height
    std::uint64_t user_data = 0;
};

class CharacterController {
public:
    CharacterController(PhysicsWorld& world, const CharacterDesc& desc);
    ~CharacterController();
    CharacterController(const CharacterController&) = delete;
    CharacterController& operator=(const CharacterController&) = delete;

    // The velocity to walk at (horizontal; the vertical part is ignored) and
    // whether to jump this frame. Call once per frame, before world.step().
    void move(const math::Vec3& walk_velocity, bool jump, float dt);

    [[nodiscard]] bool on_ground() const noexcept { return on_ground_; }
    [[nodiscard]] math::Vec3 ground_normal() const noexcept { return ground_normal_; }
    [[nodiscard]] math::Vec3 position() const;
    [[nodiscard]] math::Vec3 velocity() const;
    [[nodiscard]] BodyHandle body() const noexcept { return body_; }

private:
    void probe_ground();

    PhysicsWorld& world_;
    CharacterDesc desc_;
    BodyHandle body_;
    bool on_ground_ = false;
    math::Vec3 ground_normal_{0.0f, 1.0f, 0.0f};
    float airborne_ = 0.0f; // seconds since a jump: no ground snapping while it lasts
};

} // namespace tynima::physics
