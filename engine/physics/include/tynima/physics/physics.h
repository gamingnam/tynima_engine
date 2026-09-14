#pragma once

#include <tynima/core/handle.h>
#include <tynima/core/math.h>

#include <cstdint>
#include <memory>

// Rigid-body physics behind one interface. Phase 3 starts with Jolt behind
// it (create_jolt_world) as the reference implementation, and replaces it
// piece by piece — broadphase, narrowphase, solver — with our own, measured
// against Jolt on the same scenes through this same API.
//
// Units are SI: metres, kilograms, seconds, newtons; y is up and gravity
// points down it. Bodies are generational handles: a destroyed body's handle
// stops resolving, and every query on a stale handle is a harmless no-op.
namespace tynima::core {
class JobSystem;
}

namespace tynima::physics {

struct BodyTag {};
using BodyHandle = core::Handle<BodyTag>;

enum class ShapeType : std::uint8_t { Box, Sphere, Capsule };

// A collision shape, placed at `center` in the body's own frame — so a mesh
// whose origin is at its base can still carry a box around its middle.
struct Shape {
    ShapeType type = ShapeType::Box;
    math::Vec3 half_extents{0.5f}; // Box
    float radius = 0.5f;           // Sphere, Capsule
    float half_height = 0.5f;      // Capsule: half the length of the straight part
    math::Vec3 center{0.0f};

    [[nodiscard]] static Shape box(math::Vec3 half_extents, math::Vec3 center = math::Vec3{0.0f}) noexcept {
        Shape s;
        s.type = ShapeType::Box;
        s.half_extents = half_extents;
        s.center = center;
        return s;
    }
    [[nodiscard]] static Shape sphere(float radius, math::Vec3 center = math::Vec3{0.0f}) noexcept {
        Shape s;
        s.type = ShapeType::Sphere;
        s.radius = radius;
        s.center = center;
        return s;
    }
    [[nodiscard]] static Shape capsule(float radius, float half_height,
                                       math::Vec3 center = math::Vec3{0.0f}) noexcept {
        Shape s;
        s.type = ShapeType::Capsule;
        s.radius = radius;
        s.half_height = half_height;
        s.center = center;
        return s;
    }
};

enum class MotionType : std::uint8_t {
    Static,    // never moves; the floor
    Kinematic, // moved by set_transform / set_velocity, pushes dynamic bodies, is not pushed
    Dynamic,   // simulated
};

struct BodyDesc {
    Shape shape;
    math::Vec3 position{0.0f};
    math::Quat rotation = math::Quat::identity();
    MotionType motion = MotionType::Dynamic;
    float mass = 0.0f; // kg; 0 = from the shape's volume at 1000 kg/m^3 (water)
    float friction = 0.5f;
    float restitution = 0.0f; // 0 = no bounce, 1 = all of it back
    math::Vec3 linear_velocity{0.0f};
    math::Vec3 angular_velocity{0.0f};
    bool start_active = true;    // false: asleep until touched
    std::uint64_t user_data = 0; // the application's, e.g. an Entity, packed
};

struct BodyState {
    math::Vec3 position{0.0f};
    math::Quat rotation = math::Quat::identity();
    math::Vec3 linear_velocity{0.0f};
    math::Vec3 angular_velocity{0.0f};
    bool active = false; // false while asleep (or static)
};

struct RayHit {
    BodyHandle body;
    float fraction = 0.0f; // of max_distance along the ray
    math::Vec3 point{0.0f};
    math::Vec3 normal{0.0f};
};

// A contact between two bodies as the last step found it: for drawing, and
// for measuring our own narrowphase against the reference. Sleeping pairs
// report nothing.
struct Contact {
    BodyHandle a;
    BodyHandle b;
    math::Vec3 normal{0.0f}; // from a to b
    float depth = 0.0f;      // of the deepest point; negative while still apart (speculative)
    math::Vec3 point{0.0f};  // the first manifold point, on a's surface
    std::uint32_t point_count = 0;
};

struct WorldDesc {
    math::Vec3 gravity{0.0f, -9.81f, 0.0f};
    std::uint32_t max_bodies = 4096;
    // Steps run across this job system when given, on the calling thread otherwise.
    core::JobSystem* jobs = nullptr;
};

class PhysicsWorld {
public:
    virtual ~PhysicsWorld() = default;
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    [[nodiscard]] virtual const char* backend_name() const noexcept = 0; // "Jolt 5.6.0"

    // The null handle when the world is full or the shape is invalid.
    [[nodiscard]] virtual BodyHandle create_body(const BodyDesc& desc) = 0;
    virtual bool destroy_body(BodyHandle body) = 0; // false for a stale handle
    [[nodiscard]] virtual bool valid(BodyHandle body) const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t body_count() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t active_body_count() const noexcept = 0; // awake and dynamic

    // A default BodyState for a stale handle.
    [[nodiscard]] virtual BodyState body_state(BodyHandle body) const = 0;
    // The world-space box around the body's shape right now; empty for a stale handle.
    [[nodiscard]] virtual math::Aabb body_bounds(BodyHandle body) const = 0;
    [[nodiscard]] virtual std::uint64_t user_data(BodyHandle body) const = 0;

    // Teleports; wakes the body. For kinematic bodies this is how they move.
    virtual void set_transform(BodyHandle body, math::Vec3 position, math::Quat rotation) = 0;
    virtual void set_velocity(BodyHandle body, math::Vec3 linear, math::Vec3 angular) = 0;
    // Accumulates for the next step (newtons, at the centre of mass).
    virtual void add_force(BodyHandle body, math::Vec3 force) = 0;
    // Changes the velocity now (newton-seconds); wakes the body. The second
    // form applies it at a world-space point, which also spins the body.
    virtual void add_impulse(BodyHandle body, math::Vec3 impulse) = 0;
    virtual void add_impulse_at(BodyHandle body, math::Vec3 impulse, math::Vec3 world_point) = 0;

    // Advances the simulation by dt seconds. How that is sub-stepped is the
    // implementation's business; the fixed-timestep loop around it is the
    // caller's (Phase 3, task 6).
    virtual void step(float dt) = 0;

    // The closest body along origin + direction * t, t in [0, max_distance].
    [[nodiscard]] virtual bool cast_ray(math::Vec3 origin, math::Vec3 direction, float max_distance,
                                        RayHit& hit) const = 0;

    // The contacts the last step() worked with, one per touching pair.
    [[nodiscard]] virtual std::uint32_t contact_count() const noexcept = 0;
    virtual void each_contact(void (*fn)(void* user, const Contact& contact), void* user) const = 0;
    template <typename Fn>
    void each_contact(Fn&& fn) const {
        each_contact([](void* user, const Contact& c) { (*static_cast<Fn*>(user))(c); },
                     const_cast<void*>(static_cast<const void*>(&fn)));
    }

protected:
    PhysicsWorld() = default;
};

// Jolt Physics behind the interface: the reference implementation.
[[nodiscard]] std::unique_ptr<PhysicsWorld> create_jolt_world(const WorldDesc& desc);

// The engine's own: the AABB tree, GJK/EPA manifolds and a sequential-impulse
// solver (see tynima_world.cpp). Measured against Jolt on the same scenes.
[[nodiscard]] std::unique_ptr<PhysicsWorld> create_tynima_world(const WorldDesc& desc);

} // namespace tynima::physics
