// The engine's own PhysicsWorld: the AABB tree for the broadphase, GJK/EPA
// and clipped manifolds for the narrowphase, and a sequential-impulse
// solver with warm starting, restitution and friction, in the shape Erin
// Catto laid out for Box2D — velocities solved by iterating over contacts,
// positions corrected afterwards by non-linear Gauss-Seidel so no energy is
// pumped in to fix penetration.
//
// A step:
//   1. gravity, forces and damping into velocities
//   2. bounds into the broadphase; pairs out of it
//   3. narrowphase per pair into the manifold cache (impulses carried over)
//   4. joint and contact constraints: warm start, then velocity iterations
//   5. integrate positions
//   6. position iterations against the fresh anchors, joints first
//   7. impulses back into the manifolds and joints, sleep bookkeeping
//
// Joints are the same machinery as contacts with different rows: a distance
// joint is one bilateral row along the line between its anchors; a hinge
// is three point-to-point rows, two angular rows that keep the axes aligned
// and, at a limit, one inequality row along the axis — solved as one block,
// because solved one after another they undo each other (an angular
// impulse about the centre of mass moves the anchor) and converge so slowly
// that a door rebounds off its stop.
#include <tynima/physics/physics.h>

#include <tynima/core/assert.h>
#include <tynima/core/handle.h>
#include <tynima/core/log.h>
#include <tynima/core/profile.h>
#include <tynima/physics/broadphase.h>
#include <tynima/physics/collision.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <vector>

namespace tynima::physics {

namespace {

using math::Mat3;
using math::Quat;
using math::Vec3;

constexpr float kSpeculativeDistance = 0.02f; // contacts start this far apart, like Jolt's default
constexpr float kLinearSlop = 0.005f;         // penetration left alone, so stacks do not fight
constexpr float kBaumgarte = 0.2f;            // fraction of the penetration removed per position iteration
constexpr float kMaxCorrection = 0.2f;        // metres per iteration, so a deep overlap does not explode
constexpr float kRestitutionThreshold = 1.0f; // m/s: slower impacts do not bounce
constexpr int kVelocityIterations = 10;
constexpr int kPositionIterations = 4;
constexpr float kSleepSpeed = 0.05f;    // m/s, of the centre and of the farthest point
constexpr float kTimeBeforeSleep = 0.5f;
constexpr float kLinearDamping = 0.05f; // per second; Jolt's defaults, so the piles behave alike
constexpr float kAngularDamping = 0.05f;
constexpr float kDefaultDensity = 1000.0f; // kg/m^3: water, and Jolt's default
constexpr float kBroadphaseMargin = 0.05f;
constexpr float kPi = math::kPi;

struct Body {
    Shape shape;
    MotionType motion = MotionType::Dynamic;
    bool lock_rotation = false;
    Vec3 position{0.0f}; // the body origin; the shape's centre is offset from it
    Quat rotation = Quat::identity();
    Vec3 com{0.0f}; // centre of mass, world space
    Vec3 linear_velocity{0.0f};
    Vec3 angular_velocity{0.0f};
    Vec3 force{0.0f};
    float inv_mass = 0.0f;
    Vec3 inv_inertia_local{0.0f}; // about the centre of mass, body frame (diagonal for our shapes)
    Mat3 inv_inertia_world;
    float friction = 0.5f;
    float restitution = 0.0f;
    float radius = 0.0f; // how far the farthest point is from the centre of mass
    bool awake = false;
    bool moving = false; // as of the end of the last step: after the solver, not after gravity
    float sleep_time = 0.0f;
    ProxyHandle proxy;
    std::uint64_t user_data = 0;
    BodyHandle self;

    [[nodiscard]] bool dynamic() const noexcept { return motion == MotionType::Dynamic; }
    [[nodiscard]] bool moves() const noexcept { return motion != MotionType::Static && awake; }
    [[nodiscard]] Pose pose() const noexcept { return Pose{position, rotation}; }
    [[nodiscard]] Convex convex() const noexcept { return Convex{shape, pose()}; }

    void refresh_derived() noexcept {
        com = position + rotation.rotate(shape.center);
        const Mat3 r = rotation.to_mat3();
        inv_inertia_world = r * Mat3::scaling(inv_inertia_local) * transpose(r);
    }
    void set_com(const Vec3& new_com) noexcept {
        com = new_com;
        position = com - rotation.rotate(shape.center);
    }
    // Turns by the small rotation vector `theta` (radians about each axis)
    // about the centre of mass, which stays put; the origin moves with it.
    void rotate_by(const Vec3& theta) noexcept {
        const Quat spin{theta.x, theta.y, theta.z, 0.0f};
        rotation = normalize(rotation + spin * rotation * 0.5f);
        position = com - rotation.rotate(shape.center);
    }
    [[nodiscard]] Vec3 velocity_at(const Vec3& point) const noexcept {
        return linear_velocity + cross(angular_velocity, point - com);
    }
};

struct Joint {
    JointType type = JointType::Distance;
    BodyHandle a;
    BodyHandle b; // null: the world
    Vec3 anchor_a{0.0f};
    Vec3 anchor_b{0.0f};
    float length = 0.0f;
    Vec3 axis_a{0.0f, 1.0f, 0.0f};
    Vec3 axis_b{0.0f, 1.0f, 0.0f};
    Vec3 reference_a{1.0f, 0.0f, 0.0f}; // perpendicular to the axis in each frame; the same world
    Vec3 reference_b{1.0f, 0.0f, 0.0f}; // direction when the joint was made, so the angle starts at 0
    bool limited = false;
    float min_angle = -kPi;
    float max_angle = kPi;
    // Accumulated impulses, for warm starting.
    float distance_impulse = 0.0f;
    Vec3 point_impulse{0.0f};
    float angular_impulse[2] = {0.0f, 0.0f};
    float limit_impulse = 0.0f; // about the axis: <= 0 at the lower limit, >= 0 at the upper
};

// A small symmetric positive semi-definite system K x = b, factored (K = L
// L^T) once per step and solved once per iteration. Rows without mass — an
// angular row on a body that cannot turn — are dropped: they take no impulse.
template <int N>
struct SpdSystem {
    float l[N][N] = {};
    bool live[N] = {};

    void factor(const float (&k)[N][N]) noexcept {
        for (int i = 0; i < N; ++i) {
            for (int j = 0; j <= i; ++j) {
                float sum = k[i][j];
                for (int p = 0; p < j; ++p) {
                    sum -= l[i][p] * l[j][p];
                }
                if (i == j) {
                    live[i] = k[i][i] > 0.0f && sum > 1e-6f * k[i][i];
                    l[i][i] = live[i] ? std::sqrt(sum) : 0.0f;
                } else {
                    l[i][j] = live[j] ? sum / l[j][j] : 0.0f;
                }
            }
        }
    }

    // Solves the leading n x n block in place: x holds b on the way in.
    void solve(int n, float (&x)[N]) const noexcept {
        for (int i = 0; i < n; ++i) {
            if (!live[i]) {
                x[i] = 0.0f;
                continue;
            }
            float sum = x[i];
            for (int p = 0; p < i; ++p) {
                sum -= l[i][p] * x[p];
            }
            x[i] = sum / l[i][i];
        }
        for (int i = n - 1; i >= 0; --i) {
            if (!live[i]) {
                continue;
            }
            float sum = x[i];
            for (int p = i + 1; p < n; ++p) {
                sum -= l[p][i] * x[p];
            }
            x[i] = sum / l[i][i];
        }
    }
};

// [r]x as a matrix: skew(r) * v == cross(r, v).
Mat3 skew(const Vec3& r) noexcept {
    return Mat3{Vec3{0.0f, r.z, -r.y}, Vec3{-r.z, 0.0f, r.x}, Vec3{r.y, -r.x, 0.0f}};
}

Mat3 operator-(const Mat3& a, const Mat3& b) noexcept {
    return Mat3{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

Mat3 operator+(const Mat3& a, const Mat3& b) noexcept {
    return Mat3{a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

// Mass and inertia of the uniform solid, about its centre of mass.
struct MassProperties {
    float mass;
    Vec3 inertia; // diagonal, body frame
    float radius; // bounding radius about the centre
};

MassProperties mass_properties(const Shape& shape, float density) noexcept {
    MassProperties m{};
    switch (shape.type) {
    case ShapeType::Box: {
        const Vec3& h = shape.half_extents;
        m.mass = density * 8.0f * h.x * h.y * h.z;
        m.inertia =
            Vec3{h.y * h.y + h.z * h.z, h.x * h.x + h.z * h.z, h.x * h.x + h.y * h.y} * (m.mass / 3.0f);
        m.radius = length(h);
        break;
    }
    case ShapeType::Sphere: {
        const float r = shape.radius;
        m.mass = density * (4.0f / 3.0f) * kPi * r * r * r;
        m.inertia = Vec3{0.4f * m.mass * r * r};
        m.radius = r;
        break;
    }
    case ShapeType::Capsule: {
        // A cylinder and two hemispheres, the latter shifted out to their
        // own centres (3r/8 from the flat face) by the parallel-axis rule.
        const float r = shape.radius;
        const float h = shape.half_height;
        const float cylinder = density * kPi * r * r * 2.0f * h;
        const float hemisphere = density * (2.0f / 3.0f) * kPi * r * r * r;
        m.mass = cylinder + 2.0f * hemisphere;
        const float d = h + 3.0f * r / 8.0f;
        const float axial = cylinder * r * r * 0.5f + 2.0f * hemisphere * 0.4f * r * r;
        const float across = cylinder * (3.0f * r * r + 4.0f * h * h) / 12.0f +
                             2.0f * hemisphere * (83.0f / 320.0f * r * r + d * d);
        m.inertia = Vec3{across, axial, across};
        m.radius = h + r;
        break;
    }
    }
    return m;
}

// An orthonormal basis around a unit normal, for friction.
void tangent_basis(const Vec3& n, Vec3& t1, Vec3& t2) noexcept {
    const Vec3 helper = std::fabs(n.x) < 0.57735f ? Vec3::unit_x() : Vec3::unit_y();
    t1 = normalize(cross(n, helper));
    t2 = cross(n, t1);
}

struct ConstraintPoint {
    Vec3 ra, rb;       // from each centre of mass to the contact point
    Vec3 local_a, local_b;
    float normal_mass;
    float tangent_mass[2];
    float target_vn;   // the relative normal velocity to reach: bounce, or the speculative approach
    float normal_impulse;
    float tangent_impulse[2];
};

struct ContactConstraint {
    Body* a;
    Body* b;
    Manifold* manifold; // in the cache: impulses go back there
    Vec3 normal, tangent[2];
    float friction, restitution;
    std::uint32_t count;
    ConstraintPoint points[Manifold::kMaxPoints];
};

struct JointConstraint {
    Joint* joint;
    Body* a;
    Body* b;
    Vec3 ra, rb; // from each centre of mass to its anchor
    // Distance: one row along the line between the anchors.
    Vec3 n;
    float distance_mass;
    // Hinge: three point rows, two angular rows about the axes across the
    // hinge (t) and one about it (h), which only acts at a limit.
    static constexpr int kHingeRows = 6;
    Vec3 h, t[2];
    int limit = 0; // -1 at the lower limit, +1 at the upper, 0 free
    SpdSystem<kHingeRows> system;
    float limit_column[kHingeRows - 1]; // K's last column: how the limit impulse moves the other rows
};

class TynimaWorld final : public PhysicsWorld {
public:
    explicit TynimaWorld(const WorldDesc& desc)
        : gravity_(desc.gravity), bodies_(std::max<std::uint32_t>(desc.max_bodies, 1)),
          joints_(std::max<std::uint32_t>(desc.max_bodies, 1)),
          broadphase_(create_aabb_tree(std::max<std::uint32_t>(desc.max_bodies, 1), kBroadphaseMargin)),
          manifolds_(std::max<std::uint32_t>(desc.max_bodies, 1) * kPairsPerBody),
          max_constraints_(std::max<std::uint32_t>(desc.max_bodies, 1) * kPairsPerBody) {
        pairs_.reserve(max_constraints_);
        constraints_.reserve(max_constraints_);
        joint_constraints_.reserve(joints_.capacity());
        doomed_.reserve(64);
        world_body_.motion = MotionType::Static;
        world_body_.refresh_derived();
    }

    const char* backend_name() const noexcept override { return "Tynima"; }

    // ----------------------------------------------------------- joints

    JointHandle create_joint(const JointDesc& desc) override {
        Body* a = bodies_.get(desc.a);
        Body* b = desc.b ? bodies_.get(desc.b) : &world_body_;
        if (a == nullptr || b == nullptr) {
            return JointHandle::null();
        }
        Joint joint;
        joint.type = desc.type;
        joint.a = desc.a;
        joint.b = desc.b;
        joint.anchor_a = desc.anchor_a;
        joint.anchor_b = desc.anchor_b;
        joint.limited = desc.limited;
        joint.min_angle = std::min(desc.min_angle, 0.0f);
        joint.max_angle = std::max(desc.max_angle, 0.0f);
        const Vec3 world_a = a->pose().to_world(desc.anchor_a);
        const Vec3 world_b = b->pose().to_world(desc.anchor_b);
        if (desc.type == JointType::Distance) {
            joint.length = desc.length >= 0.0f ? desc.length : length(world_b - world_a);
        } else {
            joint.axis_a = normalize(desc.axis_a);
            joint.axis_b = normalize(desc.axis_b);
            // One world direction perpendicular to the axis, remembered in
            // each body's frame: the angle between them reads zero now.
            const Vec3 h = a->rotation.rotate(joint.axis_a);
            Vec3 t1, t2;
            tangent_basis(h, t1, t2);
            joint.reference_a = a->pose().direction_to_local(t1);
            joint.reference_b = b->pose().direction_to_local(t1);
        }
        const JointHandle handle = joints_.create(joint);
        if (handle) {
            wake(*a);
            wake(*b);
        }
        return handle;
    }

    bool destroy_joint(JointHandle handle) override {
        if (const Joint* joint = joints_.get(handle); joint != nullptr) {
            // Cut loose, the bodies move again.
            wake_other(*joint, joint->a);
            wake_other(*joint, joint->b);
        }
        return joints_.destroy(handle);
    }
    bool valid(JointHandle handle) const noexcept override { return joints_.contains(handle); }
    std::uint32_t joint_count() const noexcept override { return joints_.size(); }

    float hinge_angle(JointHandle handle) const override {
        const Joint* joint = joints_.get(handle);
        if (joint == nullptr || joint->type != JointType::Hinge) {
            return 0.0f;
        }
        const Body* a = bodies_.get(joint->a);
        const Body* b = joint->b ? bodies_.get(joint->b) : &world_body_;
        return a != nullptr && b != nullptr ? hinge_angle_of(*joint, *a, *b) : 0.0f;
    }

    BodyHandle create_body(const BodyDesc& desc) override {
        const BodyHandle handle = bodies_.create();
        if (!handle) {
            return handle;
        }
        Body& body = *bodies_.get(handle);
        body.shape = desc.shape;
        body.motion = desc.motion;
        body.position = desc.position;
        body.rotation = normalize(desc.rotation);
        body.friction = desc.friction;
        body.restitution = desc.restitution;
        body.user_data = desc.user_data;
        body.self = handle;
        const MassProperties m = mass_properties(desc.shape, kDefaultDensity);
        body.radius = m.radius;
        body.lock_rotation = desc.lock_rotation;
        if (desc.motion == MotionType::Dynamic) {
            const float mass = desc.mass > 0.0f ? desc.mass : m.mass;
            const Vec3 inertia = m.inertia * (mass / std::max(m.mass, 1e-12f));
            body.inv_mass = 1.0f / mass;
            body.inv_inertia_local =
                desc.lock_rotation ? Vec3{0.0f} : Vec3{1.0f / inertia.x, 1.0f / inertia.y, 1.0f / inertia.z};
            body.linear_velocity = desc.linear_velocity;
            body.angular_velocity = desc.angular_velocity;
            body.awake = desc.start_active;
        } else {
            body.awake = desc.motion == MotionType::Kinematic;
            if (body.motion == MotionType::Kinematic) {
                body.linear_velocity = desc.linear_velocity;
                body.angular_velocity = desc.angular_velocity;
            }
        }
        body.refresh_derived();
        body.proxy = broadphase_->add(proxy_bounds(body), handle.packed());
        if (!body.proxy) {
            bodies_.destroy(handle);
            return BodyHandle::null();
        }
        return handle;
    }

    bool destroy_body(BodyHandle handle) override {
        const Body* body = bodies_.get(handle);
        if (body == nullptr) {
            return false;
        }
        // A joint cannot outlive either of its bodies; what hung from it wakes.
        doomed_.clear();
        joints_.for_each([&](JointHandle joint, Joint& j) {
            if (j.a == handle || j.b == handle) {
                doomed_.push_back(joint);
                wake_other(j, handle);
            }
        });
        for (const JointHandle joint : doomed_) {
            joints_.destroy(joint);
        }
        // Whatever rested on the body wakes too, or it would hang in the air.
        broadphase_->query(
            proxy_bounds(*body),
            [](void* user, ProxyHandle, std::uint64_t user_data) {
                auto& self = *static_cast<TynimaWorld*>(user);
                if (Body* other = self.bodies_.get(BodyHandle::from_packed(user_data)); other != nullptr) {
                    wake(*other);
                }
            },
            this);
        broadphase_->remove(body->proxy);
        return bodies_.destroy(handle);
    }

    void wake_other(const Joint& joint, BodyHandle gone) {
        if (Body* other = bodies_.get(joint.a == gone ? joint.b : joint.a); other != nullptr) {
            wake(*other);
        }
    }

    bool valid(BodyHandle handle) const noexcept override { return bodies_.contains(handle); }
    std::uint32_t body_count() const noexcept override { return bodies_.size(); }
    std::uint32_t active_body_count() const noexcept override {
        std::uint32_t count = 0;
        bodies_.for_each([&](BodyHandle, const Body& body) {
            count += body.dynamic() && body.awake ? 1 : 0;
        });
        return count;
    }

    BodyState body_state(BodyHandle handle) const override {
        BodyState state;
        if (const Body* body = bodies_.get(handle)) {
            state.position = body->position;
            state.rotation = body->rotation;
            state.linear_velocity = body->linear_velocity;
            state.angular_velocity = body->angular_velocity;
            state.active = body->dynamic() && body->awake;
        }
        return state;
    }

    math::Aabb body_bounds(BodyHandle handle) const override {
        const Body* body = bodies_.get(handle);
        return body != nullptr ? body->convex().bounds() : math::Aabb::empty();
    }

    std::uint64_t user_data(BodyHandle handle) const override {
        const Body* body = bodies_.get(handle);
        return body != nullptr ? body->user_data : 0;
    }

    void set_transform(BodyHandle handle, Vec3 position, Quat rotation) override {
        if (Body* body = bodies_.get(handle)) {
            body->position = position;
            body->rotation = normalize(rotation);
            body->refresh_derived();
            broadphase_->update(body->proxy, proxy_bounds(*body));
            wake(*body);
        }
    }

    void set_velocity(BodyHandle handle, Vec3 linear, Vec3 angular) override {
        if (Body* body = bodies_.get(handle); body != nullptr && body->motion != MotionType::Static) {
            body->linear_velocity = linear;
            body->angular_velocity = angular;
            wake(*body);
        }
    }

    void add_force(BodyHandle handle, Vec3 force) override {
        if (Body* body = bodies_.get(handle); body != nullptr && body->dynamic()) {
            body->force += force;
            wake(*body);
        }
    }

    void add_impulse(BodyHandle handle, Vec3 impulse) override {
        if (Body* body = bodies_.get(handle); body != nullptr && body->dynamic()) {
            body->linear_velocity += impulse * body->inv_mass;
            wake(*body);
        }
    }

    void add_impulse_at(BodyHandle handle, Vec3 impulse, Vec3 world_point) override {
        if (Body* body = bodies_.get(handle); body != nullptr && body->dynamic()) {
            body->linear_velocity += impulse * body->inv_mass;
            body->angular_velocity += body->inv_inertia_world * cross(world_point - body->com, impulse);
            wake(*body);
        }
    }

    void step(float dt) override {
        TY_PROFILE_SCOPE_NAMED("physics::step (Tynima)");
        if (!(dt > 0.0f)) {
            return;
        }
        integrate_velocities(dt);
        update_broadphase();
        find_contacts(dt);
        prepare_joints();
        solve_velocities();
        integrate_positions(dt);
        solve_positions();
        finish_step(dt);
    }

    bool cast_ray(Vec3 origin, Vec3 direction, float max_distance, RayHit& hit) const override {
        const Vec3 dir = normalize(direction);
        float best = max_distance;
        bool found = false;
        broadphase_->cast_ray(origin, dir, max_distance, [&](ProxyHandle, std::uint64_t user) {
            const Body* body = bodies_.get(BodyHandle::from_packed(user));
            if (body == nullptr) {
                return;
            }
            float t;
            Vec3 normal;
            if (ray_shape(*body, origin, dir, best, t, normal) && t < best) {
                best = t;
                found = true;
                hit.body = body->self;
                hit.fraction = t / max_distance;
                hit.point = origin + dir * t;
                hit.normal = normal;
            }
        });
        return found;
    }

    std::uint32_t contact_count() const noexcept override { return manifolds_count_; }

    void each_contact(void (*fn)(void* user, const Contact& contact), void* user) const override {
        struct Thunk {
            void (*fn)(void*, const Contact&);
            void* user;
        } thunk{fn, user};
        manifolds_.each(
            [](void* raw, BodyHandle a, BodyHandle b, const Manifold& m) {
                if (m.count == 0) {
                    return;
                }
                Contact c;
                c.a = a;
                c.b = b;
                c.normal = m.normal;
                c.depth = m.points[0].depth;
                for (std::uint32_t i = 1; i < m.count; ++i) {
                    c.depth = std::max(c.depth, m.points[i].depth);
                }
                c.point = m.points[0].on_a;
                c.point_count = m.count;
                auto* t = static_cast<Thunk*>(raw);
                t->fn(t->user, c);
            },
            &thunk);
    }

private:
    static constexpr std::uint32_t kPairsPerBody = 8;

    // ---------------------------------------------------------- the step

    void integrate_velocities(float dt) {
        TY_PROFILE_SCOPE_NAMED("integrate velocities");
        bodies_.for_each([&](BodyHandle, Body& body) {
            if (!body.dynamic() || !body.awake) {
                body.force = Vec3{0.0f};
                return;
            }
            body.linear_velocity += (gravity_ + body.force * body.inv_mass) * dt;
            body.force = Vec3{0.0f};
            body.linear_velocity *= std::max(0.0f, 1.0f - kLinearDamping * dt);
            body.angular_velocity *= std::max(0.0f, 1.0f - kAngularDamping * dt);
            body.refresh_derived();
        });
    }

    void update_broadphase() {
        TY_PROFILE_SCOPE_NAMED("broadphase");
        bodies_.for_each([&](BodyHandle, Body& body) {
            if (body.moves()) {
                broadphase_->update(body.proxy, proxy_bounds(body));
            }
        });
        broadphase_->find_pairs(pairs_);
    }

    void find_contacts(float dt) {
        TY_PROFILE_SCOPE_NAMED("narrowphase");
        manifolds_.begin_frame();
        constraints_.clear();
        for (const ProxyPair& pair : pairs_) {
            Body* a = bodies_.get(BodyHandle::from_packed(broadphase_->user_data(pair.a)));
            Body* b = bodies_.get(BodyHandle::from_packed(broadphase_->user_data(pair.b)));
            if (a == nullptr || b == nullptr) {
                continue;
            }
            if (a->self.index > b->self.index) {
                std::swap(a, b); // one order per pair, so the cache matches frame to frame
            }
            if (!a->dynamic() && !b->dynamic()) {
                continue; // static and kinematic bodies do not respond
            }
            if (!a->moves() && !b->moves()) {
                continue; // both asleep (or one asleep against something static): leave them
            }
            Manifold fresh;
            if (!collide(a->convex(), b->convex(), kSpeculativeDistance, fresh)) {
                continue;
            }
            Manifold* manifold = manifolds_.update(a->self, b->self, fresh);
            if (manifold == nullptr) {
                warn_once("manifold cache full; some contacts were dropped");
                continue;
            }
            // Something moving into a sleeper wakes it. "Moving" is judged
            // by last step's settled velocity: right now every awake body
            // carries a step of gravity the solver has yet to cancel.
            if (!a->awake && a->dynamic() && b->moving) {
                wake(*a);
            }
            if (!b->awake && b->dynamic() && a->moving) {
                wake(*b);
            }
            if (constraints_.size() == max_constraints_) {
                warn_once("too many contacts for the constraint buffer; some were dropped");
                continue;
            }
            constraints_.push_back(make_constraint(*a, *b, *manifold, dt));
        }
    }

    ContactConstraint make_constraint(Body& a, Body& b, Manifold& manifold, float dt) const {
        ContactConstraint c;
        c.a = &a;
        c.b = &b;
        c.manifold = &manifold;
        c.normal = manifold.normal;
        tangent_basis(c.normal, c.tangent[0], c.tangent[1]);
        c.friction = std::sqrt(a.friction * b.friction);
        c.restitution = std::max(a.restitution, b.restitution);
        c.count = manifold.count;
        const float inv_ma = effective_inv_mass(a);
        const float inv_mb = effective_inv_mass(b);
        for (std::uint32_t i = 0; i < c.count; ++i) {
            const ContactPoint& source = manifold.points[i];
            ConstraintPoint& p = c.points[i];
            const Vec3 point = (source.on_a + source.on_b) * 0.5f;
            p.ra = point - a.com;
            p.rb = point - b.com;
            p.local_a = source.local_a;
            p.local_b = source.local_b;
            p.normal_mass = inverse_effective_mass(a, b, inv_ma, inv_mb, p.ra, p.rb, c.normal);
            p.tangent_mass[0] = inverse_effective_mass(a, b, inv_ma, inv_mb, p.ra, p.rb, c.tangent[0]);
            p.tangent_mass[1] = inverse_effective_mass(a, b, inv_ma, inv_mb, p.ra, p.rb, c.tangent[1]);
            // Touching: no approach at all (positive is separating). A gap:
            // approach at up to the speed that just closes it this step, so a
            // point still in the air pushes nothing. Fast impacts bounce.
            const float vn = dot(b.velocity_at(point) - a.velocity_at(point), c.normal);
            p.target_vn = source.depth < 0.0f ? source.depth / dt : 0.0f;
            if (vn < -kRestitutionThreshold) {
                p.target_vn = std::max(p.target_vn, -c.restitution * vn);
            }
            p.normal_impulse = source.normal_impulse;
            p.tangent_impulse[0] = source.tangent_impulse[0];
            p.tangent_impulse[1] = source.tangent_impulse[1];
        }
        return c;
    }

    // -------------------------------------------------------- joint rows

    // How far A has turned about the axis relative to B: the angle from B's
    // reference direction round to A's, right-handed about the axis.
    [[nodiscard]] static float hinge_angle_of(const Joint& joint, const Body& a, const Body& b) noexcept {
        const Vec3 h = a.rotation.rotate(joint.axis_a);
        const Vec3 ua = a.rotation.rotate(joint.reference_a);
        const Vec3 ub = b.rotation.rotate(joint.reference_b);
        return std::atan2(dot(cross(ub, ua), h), dot(ua, ub));
    }

    void prepare_joints() {
        TY_PROFILE_SCOPE_NAMED("joints");
        joint_constraints_.clear();
        joints_.for_each([&](JointHandle, Joint& joint) {
            Body* a = bodies_.get(joint.a);
            Body* b = joint.b ? bodies_.get(joint.b) : &world_body_;
            if (a == nullptr || b == nullptr) {
                return; // a body went away: the joint is inert until destroyed
            }
            // Joined bodies sleep and wake together.
            if (a->moving || b->moving || (a->awake && !b->awake && b->dynamic()) ||
                (b->awake && !a->awake && a->dynamic())) {
                wake(*a);
                wake(*b);
            }
            if (!a->moves() && !b->moves()) {
                return;
            }
            JointConstraint c{};
            c.joint = &joint;
            c.a = a;
            c.b = b;
            const Vec3 pa = a->pose().to_world(joint.anchor_a);
            const Vec3 pb = b->pose().to_world(joint.anchor_b);
            c.ra = pa - a->com;
            c.rb = pb - b->com;
            const float inv_ma = effective_inv_mass(*a);
            const float inv_mb = effective_inv_mass(*b);
            if (joint.type == JointType::Distance) {
                const Vec3 d = pb - pa;
                const float len = length(d);
                c.n = len > 1e-6f ? d * (1.0f / len) : Vec3::unit_y();
                c.distance_mass = inverse_effective_mass(*a, *b, inv_ma, inv_mb, c.ra, c.rb, c.n);
            } else {
                c.h = a->rotation.rotate(joint.axis_a);
                tangent_basis(c.h, c.t[0], c.t[1]);
                if (joint.limited) {
                    const float angle = hinge_angle_of(joint, *a, *b);
                    c.limit = angle <= joint.min_angle ? -1 : (angle >= joint.max_angle ? 1 : 0);
                }
                if (c.limit == 0) {
                    joint.limit_impulse = 0.0f;
                }
                hinge_system(*a, *b, inv_ma, inv_mb, c);
            }
            joint_constraints_.push_back(c);
        });
    }

    void warm_start_joints() {
        for (JointConstraint& c : joint_constraints_) {
            Joint& j = *c.joint;
            if (j.type == JointType::Distance) {
                apply_impulse(*c.a, *c.b, c.ra, c.rb, c.n * j.distance_impulse);
            } else {
                apply_impulse(*c.a, *c.b, c.ra, c.rb, j.point_impulse);
                apply_angular_impulse(*c.a, *c.b,
                                      c.t[0] * j.angular_impulse[0] + c.t[1] * j.angular_impulse[1] +
                                          c.h * j.limit_impulse);
            }
        }
    }

    void solve_joint_velocities() {
        for (JointConstraint& c : joint_constraints_) {
            Body& a = *c.a;
            Body& b = *c.b;
            Joint& j = *c.joint;
            if (j.type == JointType::Distance) {
                const Vec3 dv = (b.linear_velocity + cross(b.angular_velocity, c.rb)) -
                                (a.linear_velocity + cross(a.angular_velocity, c.ra));
                const float lambda = -c.distance_mass * dot(dv, c.n);
                j.distance_impulse += lambda;
                apply_impulse(a, b, c.ra, c.rb, c.n * lambda);
                continue;
            }
            solve_hinge_velocity(c, a, b, j);
        }
    }

    // K = J M^-1 J^T for the hinge's rows, in the order the impulses are
    // applied: a point impulse P on B and its opposite on A, then angular
    // impulses about t0, t1 and h on B and their opposites on A.
    static void hinge_system(const Body& a, const Body& b, float inv_ma, float inv_mb,
                             JointConstraint& c) noexcept {
        constexpr int kRows = JointConstraint::kHingeRows;
        const Mat3 zero{Vec3{0.0f}, Vec3{0.0f}, Vec3{0.0f}};
        const Mat3& ia = inv_ma > 0.0f ? a.inv_inertia_world : zero;
        const Mat3& ib = inv_mb > 0.0f ? b.inv_inertia_world : zero;
        const Mat3 sa = skew(c.ra);
        const Mat3 sb = skew(c.rb);
        const Mat3 point = Mat3::scaling(Vec3{inv_ma + inv_mb}) - sa * ia * sa - sb * ib * sb;
        // How an angular impulse about u moves the anchors apart, and how a
        // point impulse turns the bodies: the same matrix, transposed.
        const Mat3 coupling = (sa * ia + sb * ib) * -1.0f;
        const Mat3 angular = ia + ib;
        const Vec3 axes[3] = {c.t[0], c.t[1], c.h};
        float k[kRows][kRows];
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                k[i][j] = point(static_cast<std::size_t>(i), static_cast<std::size_t>(j));
            }
            for (int u = 0; u < 3; ++u) {
                const Vec3 column = coupling * axes[u];
                k[i][3 + u] = column[static_cast<std::size_t>(i)];
                k[3 + u][i] = column[static_cast<std::size_t>(i)];
            }
        }
        for (int u = 0; u < 3; ++u) {
            for (int v = 0; v < 3; ++v) {
                k[3 + u][3 + v] = dot(axes[u], angular * axes[v]);
            }
        }
        for (int i = 0; i < kRows - 1; ++i) {
            c.limit_column[i] = k[i][kRows - 1];
        }
        c.system.factor(k);
    }

    // One Gauss-Seidel visit of the hinge: all its rows at once. At a limit
    // the axis row joins in, and if that would pull the accumulated limit
    // impulse through zero, it is pinned there and the rest re-solved
    // without it — the block equivalent of clamping a single row.
    static void solve_hinge_velocity(const JointConstraint& c, Body& a, Body& b, Joint& j) noexcept {
        constexpr int kRows = JointConstraint::kHingeRows;
        const Vec3 dv = (b.linear_velocity + cross(b.angular_velocity, c.rb)) -
                        (a.linear_velocity + cross(a.angular_velocity, c.ra));
        const Vec3 dw = b.angular_velocity - a.angular_velocity;
        float x[kRows] = {-dv.x, -dv.y, -dv.z, -dot(dw, c.t[0]), -dot(dw, c.t[1]), -dot(dw, c.h)};
        int rows = kRows - 1;
        float limit_lambda = 0.0f;
        if (c.limit != 0) {
            rows = kRows;
            c.system.solve(kRows, x);
            // The upper limit may only ever push A back (a positive impulse
            // on B, its opposite on A), the lower limit only forward.
            const float total = j.limit_impulse + x[kRows - 1];
            if ((c.limit > 0 && total < 0.0f) || (c.limit < 0 && total > 0.0f)) {
                limit_lambda = -j.limit_impulse;
                x[0] = -dv.x;
                x[1] = -dv.y;
                x[2] = -dv.z;
                x[3] = -dot(dw, c.t[0]);
                x[4] = -dot(dw, c.t[1]);
                for (int i = 0; i < kRows - 1; ++i) {
                    x[i] -= c.limit_column[i] * limit_lambda;
                }
                rows = kRows - 1;
                c.system.solve(rows, x);
            } else {
                limit_lambda = x[kRows - 1];
            }
        } else {
            c.system.solve(rows, x);
        }
        const Vec3 point{x[0], x[1], x[2]};
        j.point_impulse += point;
        j.angular_impulse[0] += x[3];
        j.angular_impulse[1] += x[4];
        j.limit_impulse += limit_lambda;
        apply_impulse(a, b, c.ra, c.rb, point);
        apply_angular_impulse(a, b, c.t[0] * x[3] + c.t[1] * x[4] + c.h * limit_lambda);
    }

    void solve_joint_positions() {
        for (const JointConstraint& c : joint_constraints_) {
            Body& a = *c.a;
            Body& b = *c.b;
            const Joint& j = *c.joint;
            const float inv_ma = effective_inv_mass(a);
            const float inv_mb = effective_inv_mass(b);
            const Vec3 pa = a.pose().to_world(j.anchor_a);
            const Vec3 pb = b.pose().to_world(j.anchor_b);
            const Vec3 ra = pa - a.com;
            const Vec3 rb = pb - b.com;
            if (j.type == JointType::Distance) {
                const Vec3 d = pb - pa;
                const float len = length(d);
                if (len < 1e-6f) {
                    continue;
                }
                const Vec3 n = d * (1.0f / len);
                const float error = math::clamp(len - j.length, -kMaxCorrection, kMaxCorrection);
                const float mass = inverse_effective_mass(a, b, inv_ma, inv_mb, ra, rb, n);
                apply_position_impulse(a, b, ra, rb, n * (-kBaumgarte * error * mass));
                continue;
            }
            // Axes: rotate the bodies so B's axis meets A's.
            const Vec3 ha = a.rotation.rotate(j.axis_a);
            const Vec3 hb = b.rotation.rotate(j.axis_b);
            const Vec3 misalignment = cross(ha, hb);
            Vec3 t1, t2;
            tangent_basis(ha, t1, t2);
            for (const Vec3& t : {t1, t2}) {
                const float error = math::clamp(dot(misalignment, t), -0.5f, 0.5f);
                const float mass = angular_effective_mass(a, b, inv_ma, inv_mb, t);
                apply_angular_position_impulse(a, b, t * (-kBaumgarte * error * mass));
            }
            if (j.limited) {
                const float angle = hinge_angle_of(j, a, b);
                float error = 0.0f;
                if (angle < j.min_angle) {
                    error = angle - j.min_angle;
                } else if (angle > j.max_angle) {
                    error = angle - j.max_angle;
                }
                if (error != 0.0f) {
                    // Turn A back inside the range (the impulse lands on B, its opposite on A).
                    const float mass = angular_effective_mass(a, b, inv_ma, inv_mb, ha);
                    const float correction = kBaumgarte * math::clamp(error, -0.5f, 0.5f) * mass;
                    apply_angular_position_impulse(a, b, ha * correction);
                }
            }
            // The anchors, last.
            const Vec3 pa2 = a.pose().to_world(j.anchor_a);
            const Vec3 pb2 = b.pose().to_world(j.anchor_b);
            const Vec3 ra2 = pa2 - a.com;
            const Vec3 rb2 = pb2 - b.com;
            Vec3 error = pb2 - pa2;
            const float len = length(error);
            if (len > kMaxCorrection) {
                error *= kMaxCorrection / len;
            }
            const Mat3 mass = point_mass_matrix(a, b, inv_ma, inv_mb, ra2, rb2);
            apply_position_impulse(a, b, ra2, rb2, mass * (error * -kBaumgarte));
        }
    }

    // ------------------------------------------------------ the velocity solve

    void solve_velocities() {
        TY_PROFILE_SCOPE_NAMED("solve velocities");
        // Warm start: last frame's impulses, applied at once.
        warm_start_joints();
        for (ContactConstraint& c : constraints_) {
            for (std::uint32_t i = 0; i < c.count; ++i) {
                const ConstraintPoint& p = c.points[i];
                const Vec3 impulse = c.normal * p.normal_impulse + c.tangent[0] * p.tangent_impulse[0] +
                                     c.tangent[1] * p.tangent_impulse[1];
                apply_impulse(*c.a, *c.b, p.ra, p.rb, impulse);
            }
        }
        for (int iteration = 0; iteration < kVelocityIterations; ++iteration) {
            solve_joint_velocities();
            for (ContactConstraint& c : constraints_) {
                Body& a = *c.a;
                Body& b = *c.b;
                // Friction first, so the normal constraint has the last word.
                for (std::uint32_t i = 0; i < c.count; ++i) {
                    ConstraintPoint& p = c.points[i];
                    const float max_friction = c.friction * p.normal_impulse;
                    for (int k = 0; k < 2; ++k) {
                        const Vec3 dv = relative_velocity(a, b, p);
                        const float vt = dot(dv, c.tangent[k]);
                        const float wanted = p.tangent_impulse[k] - p.tangent_mass[k] * vt;
                        const float clamped = math::clamp(wanted, -max_friction, max_friction);
                        const float lambda = clamped - p.tangent_impulse[k];
                        p.tangent_impulse[k] = clamped;
                        apply_impulse(a, b, p.ra, p.rb, c.tangent[k] * lambda);
                    }
                }
                for (std::uint32_t i = 0; i < c.count; ++i) {
                    ConstraintPoint& p = c.points[i];
                    const float vn = dot(relative_velocity(a, b, p), c.normal);
                    const float wanted = p.normal_impulse + p.normal_mass * (p.target_vn - vn);
                    const float clamped = std::max(wanted, 0.0f);
                    const float lambda = clamped - p.normal_impulse;
                    p.normal_impulse = clamped;
                    apply_impulse(a, b, p.ra, p.rb, c.normal * lambda);
                }
            }
        }
    }

    void integrate_positions(float dt) {
        TY_PROFILE_SCOPE_NAMED("integrate positions");
        bodies_.for_each([&](BodyHandle, Body& body) {
            if (!body.moves()) {
                return;
            }
            body.set_com(body.com + body.linear_velocity * dt);
            body.rotate_by(body.angular_velocity * dt);
            body.position = body.com - body.rotation.rotate(body.shape.center);
        });
    }

    // Non-linear Gauss-Seidel: the anchors re-read from the moved bodies,
    // penetration beyond the slop pushed out a fraction at a time, straight
    // into positions, never into velocities.
    void solve_positions() {
        TY_PROFILE_SCOPE_NAMED("solve positions");
        for (int iteration = 0; iteration < kPositionIterations; ++iteration) {
            solve_joint_positions();
            for (const ContactConstraint& c : constraints_) {
                Body& a = *c.a;
                Body& b = *c.b;
                const float inv_ma = effective_inv_mass(a);
                const float inv_mb = effective_inv_mass(b);
                for (std::uint32_t i = 0; i < c.count; ++i) {
                    const ConstraintPoint& p = c.points[i];
                    const Vec3 pa = a.pose().to_world(p.local_a);
                    const Vec3 pb = b.pose().to_world(p.local_b);
                    const float separation = dot(pb - pa, c.normal);
                    const float error = std::min(0.0f, separation + kLinearSlop);
                    if (error >= 0.0f) {
                        continue;
                    }
                    const float correction = math::clamp(kBaumgarte * error, -kMaxCorrection, 0.0f);
                    const Vec3 ra = pa - a.com;
                    const Vec3 rb = pb - b.com;
                    const float mass = inverse_effective_mass(a, b, inv_ma, inv_mb, ra, rb, c.normal);
                    apply_position_impulse(a, b, ra, rb, c.normal * (-correction * mass));
                }
            }
        }
    }

    void finish_step(float dt) {
        TY_PROFILE_SCOPE_NAMED("finish step");
        // The impulses go back into the manifolds for next frame's warm start
        // — before end_frame(), which rebuilds the table and moves them.
        for (const ContactConstraint& c : constraints_) {
            for (std::uint32_t i = 0; i < c.count; ++i) {
                ContactPoint& target = c.manifold->points[i];
                target.normal_impulse = c.points[i].normal_impulse;
                target.tangent_impulse[0] = c.points[i].tangent_impulse[0];
                target.tangent_impulse[1] = c.points[i].tangent_impulse[1];
            }
        }
        manifolds_.end_frame();
        manifolds_count_ = manifolds_.count();
        bodies_.for_each([&](BodyHandle, Body& body) {
            const float speed = length(body.linear_velocity);
            const float spin = length(body.angular_velocity) * body.radius;
            body.moving = body.moves() && (speed > 2.0f * kSleepSpeed || spin > 2.0f * kSleepSpeed);
            if (!body.dynamic() || !body.awake) {
                return;
            }
            body.refresh_derived();
            if (speed < kSleepSpeed && spin < kSleepSpeed) {
                body.sleep_time += dt;
                if (body.sleep_time >= kTimeBeforeSleep) {
                    body.awake = false;
                    body.moving = false;
                    body.linear_velocity = Vec3{0.0f};
                    body.angular_velocity = Vec3{0.0f};
                }
            } else {
                body.sleep_time = 0.0f;
            }
        });
    }

    // ------------------------------------------------------------ helpers

    [[nodiscard]] static math::Aabb proxy_bounds(const Body& body) noexcept {
        return body.convex().bounds().expanded(kSpeculativeDistance);
    }

    // A sleeper takes part as if static; the wake-up rule decides when it stops being one.
    [[nodiscard]] static float effective_inv_mass(const Body& body) noexcept {
        return body.dynamic() && body.awake ? body.inv_mass : 0.0f;
    }

    static void wake(Body& body) noexcept {
        if (body.dynamic()) {
            body.awake = true;
        }
        body.moving = body.motion != MotionType::Static; // let it wake what it touches next step
        body.sleep_time = 0.0f;
    }

    [[nodiscard]] static float inverse_effective_mass(const Body& a, const Body& b, float inv_ma,
                                                      float inv_mb, const Vec3& ra, const Vec3& rb,
                                                      const Vec3& n) noexcept {
        const Vec3 rna = cross(ra, n);
        const Vec3 rnb = cross(rb, n);
        float k = inv_ma + inv_mb;
        if (inv_ma > 0.0f) {
            k += dot(rna, a.inv_inertia_world * rna);
        }
        if (inv_mb > 0.0f) {
            k += dot(rnb, b.inv_inertia_world * rnb);
        }
        return k > 0.0f ? 1.0f / k : 0.0f;
    }

    [[nodiscard]] static Vec3 relative_velocity(const Body& a, const Body& b,
                                                const ConstraintPoint& p) noexcept {
        return (b.linear_velocity + cross(b.angular_velocity, p.rb)) -
               (a.linear_velocity + cross(a.angular_velocity, p.ra));
    }

    // Effective mass for a relative angular velocity along `axis`.
    [[nodiscard]] static float angular_effective_mass(const Body& a, const Body& b, float inv_ma,
                                                      float inv_mb, const Vec3& axis) noexcept {
        float k = 0.0f;
        if (inv_ma > 0.0f) {
            k += dot(axis, a.inv_inertia_world * axis);
        }
        if (inv_mb > 0.0f) {
            k += dot(axis, b.inv_inertia_world * axis);
        }
        return k > 0.0f ? 1.0f / k : 0.0f;
    }

    // The inverse of the 3x3 effective-mass matrix of a point-to-point row.
    [[nodiscard]] static Mat3 point_mass_matrix(const Body& a, const Body& b, float inv_ma, float inv_mb,
                                                const Vec3& ra, const Vec3& rb) noexcept {
        Mat3 k = Mat3::scaling(Vec3{inv_ma + inv_mb});
        if (inv_ma > 0.0f) {
            const Mat3 s = skew(ra);
            k = k - s * a.inv_inertia_world * s;
        }
        if (inv_mb > 0.0f) {
            const Mat3 s = skew(rb);
            k = k - s * b.inv_inertia_world * s;
        }
        return determinant(k) > 1e-20f ? inverse(k) : Mat3{Vec3{0.0f}, Vec3{0.0f}, Vec3{0.0f}};
    }

    // An angular impulse on B, its opposite on A.
    static void apply_angular_impulse(Body& a, Body& b, const Vec3& impulse) noexcept {
        if (effective_inv_mass(a) > 0.0f) {
            a.angular_velocity -= a.inv_inertia_world * impulse;
        }
        if (effective_inv_mass(b) > 0.0f) {
            b.angular_velocity += b.inv_inertia_world * impulse;
        }
    }

    // Position-solver counterparts: straight into poses, never velocities.
    static void apply_position_impulse(Body& a, Body& b, const Vec3& ra, const Vec3& rb,
                                       const Vec3& impulse) noexcept {
        if (const float inv_ma = effective_inv_mass(a); inv_ma > 0.0f) {
            a.set_com(a.com - impulse * inv_ma);
            a.rotate_by((a.inv_inertia_world * cross(ra, impulse)) * -1.0f);
        }
        if (const float inv_mb = effective_inv_mass(b); inv_mb > 0.0f) {
            b.set_com(b.com + impulse * inv_mb);
            b.rotate_by(b.inv_inertia_world * cross(rb, impulse));
        }
    }

    static void apply_angular_position_impulse(Body& a, Body& b, const Vec3& impulse) noexcept {
        if (effective_inv_mass(a) > 0.0f) {
            a.rotate_by((a.inv_inertia_world * impulse) * -1.0f);
        }
        if (effective_inv_mass(b) > 0.0f) {
            b.rotate_by(b.inv_inertia_world * impulse);
        }
    }

    // `impulse` on B, its opposite on A.
    static void apply_impulse(Body& a, Body& b, const Vec3& ra, const Vec3& rb,
                              const Vec3& impulse) noexcept {
        if (const float inv_ma = effective_inv_mass(a); inv_ma > 0.0f) {
            a.linear_velocity -= impulse * inv_ma;
            a.angular_velocity -= a.inv_inertia_world * cross(ra, impulse);
        }
        if (const float inv_mb = effective_inv_mass(b); inv_mb > 0.0f) {
            b.linear_velocity += impulse * inv_mb;
            b.angular_velocity += b.inv_inertia_world * cross(rb, impulse);
        }
    }

    // Exact ray against one body's shape, in its own frame. `t` is a
    // distance along the unit direction, less than `max_t`.
    [[nodiscard]] static bool ray_shape(const Body& body, const Vec3& origin, const Vec3& dir, float max_t,
                                        float& t, Vec3& normal) noexcept {
        const Pose pose = body.pose();
        const Vec3 o = pose.to_local(origin) - body.shape.center;
        const Vec3 d = pose.direction_to_local(dir);
        Vec3 local_normal;
        bool hit = false;
        switch (body.shape.type) {
        case ShapeType::Box:
            hit = ray_box(o, d, body.shape.half_extents, max_t, t, local_normal);
            break;
        case ShapeType::Sphere:
            hit = ray_sphere(o, d, Vec3{0.0f}, body.shape.radius, max_t, t, local_normal);
            break;
        case ShapeType::Capsule:
            hit = ray_capsule(o, d, body.shape.radius, body.shape.half_height, max_t, t, local_normal);
            break;
        }
        if (hit) {
            normal = pose.rotation.rotate(local_normal);
        }
        return hit;
    }

    static bool ray_box(const Vec3& o, const Vec3& d, const Vec3& h, float max_t, float& t,
                        Vec3& n) noexcept {
        float t_near = 0.0f, t_far = max_t;
        int axis_in = -1;
        const float os[3] = {o.x, o.y, o.z}, ds[3] = {d.x, d.y, d.z}, hs[3] = {h.x, h.y, h.z};
        for (int i = 0; i < 3; ++i) {
            if (std::fabs(ds[i]) < 1e-12f) {
                if (std::fabs(os[i]) > hs[i]) {
                    return false;
                }
                continue;
            }
            float t0 = (-hs[i] - os[i]) / ds[i];
            float t1 = (hs[i] - os[i]) / ds[i];
            if (t0 > t1) {
                std::swap(t0, t1);
            }
            if (t0 > t_near) {
                t_near = t0;
                axis_in = i;
            }
            t_far = std::min(t_far, t1);
            if (t_near > t_far) {
                return false;
            }
        }
        if (axis_in < 0) {
            return false; // started inside: no surface crossed going in
        }
        t = t_near;
        n = Vec3{0.0f};
        const float sign = ds[axis_in] > 0.0f ? -1.0f : 1.0f;
        (axis_in == 0 ? n.x : (axis_in == 1 ? n.y : n.z)) = sign;
        return true;
    }

    static bool ray_sphere(const Vec3& o, const Vec3& d, const Vec3& c, float r, float max_t, float& t,
                           Vec3& n) noexcept {
        const Vec3 m = o - c;
        const float b = dot(m, d);
        const float cc = dot(m, m) - r * r;
        if (cc > 0.0f && b > 0.0f) {
            return false; // outside and pointing away
        }
        const float disc = b * b - cc;
        if (disc < 0.0f) {
            return false;
        }
        const float root = -b - std::sqrt(disc);
        if (root < 0.0f || root > max_t) {
            return false;
        }
        t = root;
        n = (o + d * t - c) * (1.0f / r);
        return true;
    }

    static bool ray_capsule(const Vec3& o, const Vec3& d, float r, float h, float max_t, float& t,
                            Vec3& n) noexcept {
        bool hit = false;
        t = max_t;
        // The cylinder around the y axis: solve in the xz plane, keep hits
        // whose y is within the straight part.
        const float a = d.x * d.x + d.z * d.z;
        if (a > 1e-12f) {
            const float b = o.x * d.x + o.z * d.z;
            const float c = o.x * o.x + o.z * o.z - r * r;
            const float disc = b * b - a * c;
            if (disc >= 0.0f) {
                const float root = (-b - std::sqrt(disc)) / a;
                const float y = o.y + d.y * root;
                if (root >= 0.0f && root < t && std::fabs(y) <= h) {
                    t = root;
                    n = normalize(Vec3{o.x + d.x * root, 0.0f, o.z + d.z * root});
                    hit = true;
                }
            }
        }
        for (const float end : {-h, h}) {
            float tc;
            Vec3 nc;
            if (ray_sphere(o, d, Vec3{0.0f, end, 0.0f}, r, t, tc, nc) && tc < t) {
                t = tc;
                n = nc;
                hit = true;
            }
        }
        return hit;
    }

    void warn_once(const char* what) {
        if (!warned_) {
            warned_ = true;
            TY_LOG_WARN("physics", "%s (raise WorldDesc::max_bodies)", what);
        }
    }

    Vec3 gravity_;
    core::HandlePool<Body, BodyTag> bodies_;
    core::HandlePool<Joint, JointTag> joints_;
    Body world_body_; // what a joint to the world attaches to: static, at the origin
    std::vector<JointConstraint> joint_constraints_;
    std::vector<JointHandle> doomed_;
    std::unique_ptr<Broadphase> broadphase_;
    ManifoldCache manifolds_;
    std::uint32_t manifolds_count_ = 0;
    std::uint32_t max_constraints_;
    std::vector<ProxyPair> pairs_;
    std::vector<ContactConstraint> constraints_;
    bool warned_ = false;
};

} // namespace

std::unique_ptr<PhysicsWorld> create_tynima_world(const WorldDesc& desc) {
    return std::make_unique<TynimaWorld>(desc);
}

} // namespace tynima::physics
