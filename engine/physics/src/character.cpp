#include <tynima/physics/character.h>

#include <algorithm>
#include <cmath>

namespace tynima::physics {

using math::Vec3;

namespace {
constexpr float kJumpGrace = 0.2f;              // seconds after a jump before ground counts again
constexpr Vec3 kGravity{0.0f, -9.81f, 0.0f};    // what the controller cancels while standing
} // namespace

CharacterController::CharacterController(PhysicsWorld& world, const CharacterDesc& desc)
    : world_(world), desc_(desc) {
    BodyDesc body;
    body.shape = Shape::capsule(desc.radius, desc.half_height);
    body.position = desc.position;
    body.mass = desc.mass;
    body.friction = 0.0f; // the controller sets the velocity itself; friction would only fight it
    body.restitution = 0.0f;
    body.lock_rotation = true;
    body.user_data = desc.user_data;
    body_ = world.create_body(body);
}

CharacterController::~CharacterController() {
    if (world_.valid(body_)) {
        world_.destroy_body(body_);
    }
}

Vec3 CharacterController::position() const {
    return world_.body_state(body_).position;
}

Vec3 CharacterController::velocity() const {
    return world_.body_state(body_).linear_velocity;
}

// Ground is whatever the last step's contacts say the capsule stands on:
// a contact whose normal, pointing into the capsule, is within the slope
// limit of straight up. Steeper is a wall and gives no footing.
void CharacterController::probe_ground() {
    on_ground_ = false;
    ground_normal_ = Vec3{0.0f, 1.0f, 0.0f};
    const float min_up = math::cosine(desc_.max_slope_degrees * math::kPi / 180.0f);
    float best = min_up;
    world_.each_contact([&](const Contact& c) {
        Vec3 up;
        if (c.a == body_) {
            up = c.normal * -1.0f; // from a to b; the ground pushes the other way
        } else if (c.b == body_) {
            up = c.normal;
        } else {
            return;
        }
        if (up.y >= best) {
            best = up.y;
            on_ground_ = true;
            ground_normal_ = up;
        }
    });
}

void CharacterController::move(const Vec3& walk_velocity, bool jump, float dt) {
    if (!world_.valid(body_)) {
        return;
    }
    airborne_ = std::max(0.0f, airborne_ - dt);
    probe_ground();
    if (airborne_ > 0.0f) {
        on_ground_ = false; // just jumped: last step's contacts are stale
    }

    Vec3 velocity = world_.body_state(body_).linear_velocity;
    const Vec3 wish{walk_velocity.x, 0.0f, walk_velocity.z};
    if (on_ground_) {
        // Walk along the ground, at the asked-for speed even up or down a
        // slope, and hold still when nothing is asked: gravity is cancelled
        // for the step, so nothing creeps downhill.
        const Vec3& n = ground_normal_;
        Vec3 along = wish - n * dot(wish, n);
        const float wanted = length(wish);
        const float got = length(along);
        if (got > 1e-6f && wanted > 0.0f) {
            along = along * (wanted / got);
        }
        velocity = along;
        if (jump) {
            velocity.y += desc_.jump_speed;
            airborne_ = kJumpGrace;
            on_ground_ = false;
        } else {
            world_.add_force(body_, kGravity * -desc_.mass);
        }
    } else {
        // In the air: full control over the horizontal, gravity owns the vertical.
        velocity.x = wish.x;
        velocity.z = wish.z;
    }
    world_.set_velocity(body_, velocity, Vec3{0.0f});
}

} // namespace tynima::physics
