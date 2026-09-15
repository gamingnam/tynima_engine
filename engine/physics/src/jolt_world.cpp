// Jolt Physics behind physics::PhysicsWorld. Jolt's headers stay in this
// file: the rest of the engine sees handles and plain structs, and the
// layering check keeps it that way.
#include <tynima/physics/physics.h>

#include <tynima/core/assert.h>
#include <tynima/core/handle.h>
#include <tynima/core/jobs.h>
#include <tynima/core/log.h>
#include <tynima/core/memory.h>
#include <tynima/core/profile.h>
#include <tynima/physics/collision.h> // Pose

#include <Jolt/Jolt.h> // first, before any other Jolt header

// The rest of this file mixes with Jolt's over-aligned types and its style
// of casting; the warnings that raises (MSVC's C4324 above all) are Jolt's
// to judge, not ours, for this one file.
JPH_SUPPRESS_WARNINGS

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

namespace tynima::physics {

namespace {

using math::Quat;
using math::Vec3;

// ------------------------------------------------------------ conversions

JPH::Vec3 to_jolt(Vec3 v) noexcept {
    return JPH::Vec3(v.x, v.y, v.z);
}
JPH::Quat to_jolt(Quat q) noexcept {
    return JPH::Quat(q.x, q.y, q.z, q.w);
}
Vec3 from_jolt(JPH::Vec3Arg v) noexcept {
    return Vec3{v.GetX(), v.GetY(), v.GetZ()};
}
Quat from_jolt(JPH::QuatArg q) noexcept {
    return Quat{q.GetX(), q.GetY(), q.GetZ(), q.GetW()};
}

// --------------------------------------------------------------- runtime

// Jolt's process-wide state — allocator hooks, the type factory, its trace
// and assert routes — set up by the first world and torn down by the last.
//
// Allocations are routed through the engine's operator new, so Jolt's memory
// is counted and shows in Tracy's memory view; the world's methods run under
// an ExternalAllocationScope, so it counts as external, like the GPU driver.
void* jolt_allocate(std::size_t size) {
    return ::operator new(size);
}
void* jolt_reallocate(void* block, std::size_t old_size, std::size_t new_size) {
    void* grown = ::operator new(new_size);
    if (block != nullptr) {
        std::memcpy(grown, block, std::min(old_size, new_size));
        ::operator delete(block);
    }
    return grown;
}
void jolt_free(void* block) {
    ::operator delete(block);
}
void* jolt_aligned_allocate(std::size_t size, std::size_t alignment) {
    return ::operator new(size, std::align_val_t{alignment});
}
void jolt_aligned_free(void* block) {
    // The engine's operator delete reads the alignment back from its own
    // block header, so the plain form releases an aligned block correctly.
    ::operator delete(block);
}

TY_PRINTF_FORMAT(1, 2)
void jolt_trace(const char* format, ...) {
    std::va_list args;
    va_start(args, format);
    core::log_message_v(core::LogLevel::Info, "jolt", nullptr, 0, format, args);
    va_end(args);
}

#if defined(JPH_ENABLE_ASSERTS)
bool jolt_assert_failed(const char* expression, const char* message, const char* file, JPH::uint line) {
    return core::assert_failed(expression, message != nullptr ? message : "Jolt assertion", file,
                               static_cast<int>(line));
}
#endif

std::mutex g_runtime_mutex;
int g_runtime_users = 0;

void acquire_runtime() {
    const std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (g_runtime_users++ > 0) {
        return;
    }
    JPH::Allocate = jolt_allocate;
    JPH::Reallocate = jolt_reallocate;
    JPH::Free = jolt_free;
    JPH::AlignedAllocate = jolt_aligned_allocate;
    JPH::AlignedFree = jolt_aligned_free;
    JPH::Trace = jolt_trace;
#if defined(JPH_ENABLE_ASSERTS)
    JPH::AssertFailed = jolt_assert_failed;
#endif
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
}

void release_runtime() {
    const std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (--g_runtime_users > 0) {
        return;
    }
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

// A world's claim on the runtime. A base of JoltWorld, listed first, so it
// is set up before any Jolt member is constructed and torn down after the
// last one is destroyed.
class RuntimeReference {
public:
    RuntimeReference() { acquire_runtime(); }
    ~RuntimeReference() { release_runtime(); }
    RuntimeReference(const RuntimeReference&) = delete;
    RuntimeReference& operator=(const RuntimeReference&) = delete;
};

// ---------------------------------------------------------------- layers

// Two object layers, two broadphase layers: static bodies never test against
// each other. That is all the filtering the engine needs for now.
namespace layers {
constexpr JPH::ObjectLayer kStatic = 0;
constexpr JPH::ObjectLayer kMoving = 1;
} // namespace layers

namespace broad_phase {
constexpr JPH::BroadPhaseLayer kStatic(0);
constexpr JPH::BroadPhaseLayer kMoving(1);
constexpr JPH::uint kCount = 2;
} // namespace broad_phase

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return broad_phase::kCount; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return layer == layers::kStatic ? broad_phase::kStatic : broad_phase::kMoving;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == broad_phase::kStatic ? "static" : "moving";
    }
#endif
};

class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad) const override {
        return layer != layers::kStatic || broad != broad_phase::kStatic;
    }
};

class ObjectPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return a != layers::kStatic || b != layers::kStatic;
    }
};

// ------------------------------------------------------------- job system

// Jolt's jobs on the engine's JobSystem: one pool of threads for the whole
// engine, as the roadmap wants. Mirrors Jolt's own JobSystemThreadPool minus
// the threads — a queued job becomes one engine Job that executes and
// releases it. Jolt's barriers run jobs on the waiting thread too, so the
// main thread helps while it waits for a step.
class EngineJobSystem final : public JPH::JobSystemWithBarrier {
public:
    explicit EngineJobSystem(core::JobSystem& jobs) : jobs_(jobs) {
        JobSystemWithBarrier::Init(JPH::cMaxPhysicsBarriers);
        job_pool_.Init(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsJobs);
    }
    // A step waits for its barrier before returning, but the engine job
    // wrapping a Jolt job the barrier already ran can still be queued; it
    // must run before the pool it points into goes away.
    ~EngineJobSystem() override { jobs_.wait(in_flight_); }

    int GetMaxConcurrency() const override { return static_cast<int>(jobs_.worker_count()) + 1; }

    JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function,
                        JPH::uint32 dependencies) override {
        JPH::uint32 index;
        for (;;) {
            index = job_pool_.ConstructObject(name, color, this, function, dependencies);
            if (index != JobPool::cInvalidObjectIndex) {
                break;
            }
            // Every slot is taken: let the jobs in flight finish and retry.
            std::this_thread::yield();
        }
        Job* job = &job_pool_.Get(index);
        JobHandle handle(job); // holds a reference: the job may complete inside QueueJob
        if (dependencies == 0) {
            QueueJob(job);
        }
        return handle;
    }

protected:
    void QueueJob(Job* job) override {
        job->AddRef(); // the queue's reference, released by execute()
        jobs_.run({&execute, job}, &in_flight_);
    }
    void QueueJobs(Job** jobs, JPH::uint count) override {
        for (JPH::uint i = 0; i < count; ++i) {
            QueueJob(jobs[i]);
        }
    }
    void FreeJob(Job* job) override { job_pool_.DestructObject(job); }

private:
    using JobPool = JPH::FixedSizeFreeList<Job>;

    static void execute(void* data) {
        auto* job = static_cast<Job*>(data);
        job->Execute(); // a no-op when the barrier's thread got to it first
        job->Release();
    }

    core::JobSystem& jobs_;
    JobPool job_pool_;
    core::JobCounter in_flight_;
};

// ----------------------------------------------------------------- shapes

JPH::ShapeRefC make_shape(const Shape& shape, const char*& error) {
    JPH::ShapeSettings::ShapeResult result;
    switch (shape.type) {
    case ShapeType::Box: {
        // Jolt rounds box edges by a convex radius; it must fit inside the box.
        const float smallest = std::min({shape.half_extents.x, shape.half_extents.y, shape.half_extents.z});
        const float convex_radius = std::min(JPH::cDefaultConvexRadius, smallest * 0.5f);
        result = JPH::BoxShapeSettings(to_jolt(shape.half_extents), convex_radius).Create();
        break;
    }
    case ShapeType::Sphere:
        result = JPH::SphereShapeSettings(shape.radius).Create();
        break;
    case ShapeType::Capsule:
        result = JPH::CapsuleShapeSettings(shape.half_height, shape.radius).Create();
        break;
    }
    if (result.HasError()) {
        error = result.GetError().c_str();
        return nullptr;
    }
    JPH::ShapeRefC created = result.Get();
    const Vec3 c = shape.center;
    if (c.x != 0.0f || c.y != 0.0f || c.z != 0.0f) {
        result = JPH::RotatedTranslatedShapeSettings(to_jolt(c), JPH::Quat::sIdentity(), created.GetPtr())
                     .Create();
        if (result.HasError()) {
            error = result.GetError().c_str();
            return nullptr;
        }
        created = result.Get();
    }
    return created;
}

// --------------------------------------------------------------- contacts

// Records every manifold Jolt reports during a step. Called from Jolt's
// jobs, so from several threads at once.
class ContactRecorder final : public JPH::ContactListener {
public:
    void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2,
                        const JPH::ContactManifold& manifold, JPH::ContactSettings&) override {
        record(body1, body2, manifold);
    }
    void OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2,
                            const JPH::ContactManifold& manifold, JPH::ContactSettings&) override {
        record(body1, body2, manifold);
    }

    void clear() noexcept { contacts_.clear(); }
    void reserve(std::size_t count) { contacts_.reserve(count); }
    [[nodiscard]] const std::vector<Contact>& contacts() const noexcept { return contacts_; }

private:
    void record(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold) {
        Contact c;
        c.a = BodyHandle::from_packed(body1.GetUserData());
        c.b = BodyHandle::from_packed(body2.GetUserData());
        c.normal = from_jolt(manifold.mWorldSpaceNormal);
        c.depth = manifold.mPenetrationDepth;
        c.point_count = static_cast<std::uint32_t>(manifold.mRelativeContactPointsOn1.size());
        if (c.point_count > 0) {
            c.point = from_jolt(manifold.GetWorldSpaceContactPointOn1(0));
        }
        const std::lock_guard<std::mutex> lock(mutex_);
        contacts_.push_back(c);
    }

    std::mutex mutex_;
    std::vector<Contact> contacts_;
};

// ------------------------------------------------------------------ world

struct BodyRecord {
    JPH::BodyID id;
    std::uint64_t user_data;
};

struct JointRecord {
    JPH::Ref<JPH::Constraint> constraint;
    JointType type;
    BodyHandle a;
    BodyHandle b;
};

// Any unit vector perpendicular to `v`, for a hinge's reference direction.
JPH::Vec3 perpendicular(JPH::Vec3Arg v) noexcept {
    return v.GetNormalizedPerpendicular();
}

class JoltWorld final : private RuntimeReference, public PhysicsWorld {
public:
    explicit JoltWorld(const WorldDesc& desc)
        : bodies_(desc.max_bodies), joints_(desc.max_bodies), temp_allocator_(kTempAllocatorBytes),
          job_system_(desc.jobs != nullptr
                          ? static_cast<JPH::JobSystem*>(new EngineJobSystem(*desc.jobs))
                          : static_cast<JPH::JobSystem*>(new JPH::JobSystemSingleThreaded(
                                static_cast<JPH::uint>(JPH::cMaxPhysicsJobs)))) {
        // Pairs and contacts scale with bodies; a pile touches a few
        // neighbours each. Overflow is reported by step(), not fatal.
        const JPH::uint max_bodies = std::max<JPH::uint>(desc.max_bodies, 1);
        const JPH::uint max_pairs = std::max<JPH::uint>(1024, max_bodies * 4);
        const JPH::uint max_contacts = std::max<JPH::uint>(1024, max_bodies * 2);
        system_.Init(max_bodies, 0, max_pairs, max_contacts, broad_phase_layers_, object_vs_broad_phase_,
                     object_pairs_);
        system_.SetGravity(to_jolt(desc.gravity));
        contacts_.reserve(static_cast<std::size_t>(max_contacts));
        system_.SetContactListener(&contacts_);
    }

    ~JoltWorld() override {
        TY_EXTERNAL_ALLOCATIONS();
        joints_.for_each([&](JointHandle, JointRecord& record) {
            system_.RemoveConstraint(record.constraint);
        });
        JPH::BodyInterface& bi = system_.GetBodyInterface();
        bodies_.for_each([&](BodyHandle, BodyRecord& record) {
            bi.RemoveBody(record.id);
            bi.DestroyBody(record.id);
        });
        delete job_system_;
    }

    const char* backend_name() const noexcept override { return kBackendName; }

    BodyHandle create_body(const BodyDesc& desc) override {
        TY_EXTERNAL_ALLOCATIONS();
        const char* error = nullptr;
        const JPH::ShapeRefC shape = make_shape(desc.shape, error);
        if (shape == nullptr) {
            TY_LOG_ERROR("physics", "shape rejected: %s", error != nullptr ? error : "unknown");
            return BodyHandle::null();
        }
        const BodyHandle handle = bodies_.create(BodyRecord{JPH::BodyID(), desc.user_data});
        if (!handle) {
            return BodyHandle::null(); // the world is full
        }

        JPH::EMotionType motion = JPH::EMotionType::Dynamic;
        JPH::ObjectLayer layer = layers::kMoving;
        switch (desc.motion) {
        case MotionType::Static:
            motion = JPH::EMotionType::Static;
            layer = layers::kStatic;
            break;
        case MotionType::Kinematic:
            motion = JPH::EMotionType::Kinematic;
            break;
        case MotionType::Dynamic:
            break;
        }
        JPH::BodyCreationSettings settings(shape, to_jolt(desc.position), to_jolt(desc.rotation), motion,
                                           layer);
        settings.mFriction = desc.friction;
        settings.mRestitution = desc.restitution;
        settings.mLinearVelocity = to_jolt(desc.linear_velocity);
        settings.mAngularVelocity = to_jolt(desc.angular_velocity);
        settings.mUserData = handle.packed(); // Jolt hands it back from queries
        // A kinematic body goes where it is told, however slowly: left to
        // Jolt it would fall asleep below the sleep speed and stop. Only
        // while it moves, though — at rest it may sleep, and let what rests
        // on it sleep too (see set_velocity).
        const bool under_way = motion == JPH::EMotionType::Kinematic &&
                               (desc.linear_velocity != Vec3{0.0f} || desc.angular_velocity != Vec3{0.0f});
        settings.mAllowSleeping = !under_way;
        if (desc.lock_rotation) {
            settings.mAllowedDOFs =
                JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY |
                JPH::EAllowedDOFs::TranslationZ;
        }
        if (desc.mass > 0.0f && motion == JPH::EMotionType::Dynamic) {
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = desc.mass;
        }
        const JPH::EActivation activation = desc.start_active && motion != JPH::EMotionType::Static
                                                ? JPH::EActivation::Activate
                                                : JPH::EActivation::DontActivate;
        const JPH::BodyID id = system_.GetBodyInterface().CreateAndAddBody(settings, activation);
        if (id.IsInvalid()) {
            bodies_.destroy(handle);
            TY_LOG_ERROR("physics", "Jolt has no room for another body (%u in use)", bodies_.size());
            return BodyHandle::null();
        }
        bodies_.get(handle)->id = id;
        return handle;
    }

    bool destroy_body(BodyHandle handle) override {
        TY_EXTERNAL_ALLOCATIONS();
        const BodyRecord* record = bodies_.get(handle);
        if (record == nullptr) {
            return false;
        }
        JPH::BodyInterface& bi = system_.GetBodyInterface();
        // A joint cannot outlive either of its bodies; what hung from it wakes.
        joints_.for_each([&](JointHandle joint, JointRecord& j) {
            if ((j.a == handle || j.b == handle) && doomed_count_ < kMaxJointsPerBody) {
                system_.RemoveConstraint(j.constraint);
                doomed_joints_[doomed_count_++] = joint;
                wake_other(j, handle);
            }
        });
        for (std::uint32_t i = 0; i < doomed_count_; ++i) {
            joints_.destroy(doomed_joints_[i]);
        }
        doomed_count_ = 0;
        // Whatever rested on the body wakes too, or it would hang in the air.
        // (The bounds are read under a lock that must be gone before activating.)
        JPH::AABox bounds;
        bool have_bounds = false;
        {
            const JPH::BodyLockRead lock(system_.GetBodyLockInterface(), record->id);
            if (lock.Succeeded()) {
                bounds = lock.GetBody().GetWorldSpaceBounds();
                have_bounds = true;
            }
        }
        if (have_bounds) {
            bi.ActivateBodiesInAABox(bounds, {}, {});
        }
        bi.RemoveBody(record->id);
        bi.DestroyBody(record->id);
        return bodies_.destroy(handle);
    }

    void wake_other(const JointRecord& joint, BodyHandle gone) {
        const BodyHandle other = joint.a == gone ? joint.b : joint.a;
        if (const BodyRecord* record = bodies_.get(other); record != nullptr) {
            system_.GetBodyInterface().ActivateBody(record->id);
        }
    }

    bool valid(BodyHandle handle) const noexcept override { return bodies_.contains(handle); }
    std::uint32_t body_count() const noexcept override { return bodies_.size(); }
    std::uint32_t active_body_count() const noexcept override {
        return system_.GetNumActiveBodies(JPH::EBodyType::RigidBody);
    }

    BodyState body_state(BodyHandle handle) const override {
        BodyState state;
        const BodyRecord* record = bodies_.get(handle);
        if (record == nullptr) {
            return state;
        }
        const JPH::BodyInterface& bi = system_.GetBodyInterface();
        JPH::RVec3 position;
        JPH::Quat rotation;
        bi.GetPositionAndRotation(record->id, position, rotation);
        JPH::Vec3 linear, angular;
        bi.GetLinearAndAngularVelocity(record->id, linear, angular);
        state.position = from_jolt(position);
        state.rotation = from_jolt(rotation);
        state.linear_velocity = from_jolt(linear);
        state.angular_velocity = from_jolt(angular);
        state.active = bi.IsActive(record->id);
        return state;
    }

    math::Aabb body_bounds(BodyHandle handle) const override {
        const BodyRecord* record = bodies_.get(handle);
        if (record == nullptr) {
            return math::Aabb::empty();
        }
        const JPH::BodyLockRead lock(system_.GetBodyLockInterface(), record->id);
        if (!lock.Succeeded()) {
            return math::Aabb::empty();
        }
        const JPH::AABox& bounds = lock.GetBody().GetWorldSpaceBounds();
        return math::Aabb{from_jolt(bounds.mMin), from_jolt(bounds.mMax)};
    }

    std::uint64_t user_data(BodyHandle handle) const override {
        const BodyRecord* record = bodies_.get(handle);
        return record != nullptr ? record->user_data : 0;
    }

    void set_transform(BodyHandle handle, Vec3 position, Quat rotation) override {
        TY_EXTERNAL_ALLOCATIONS();
        if (const BodyRecord* record = bodies_.get(handle)) {
            system_.GetBodyInterface().SetPositionAndRotation(record->id, to_jolt(position),
                                                              to_jolt(rotation), JPH::EActivation::Activate);
        }
    }

    void set_velocity(BodyHandle handle, Vec3 linear, Vec3 angular) override {
        TY_EXTERNAL_ALLOCATIONS();
        if (const BodyRecord* record = bodies_.get(handle)) {
            JPH::BodyInterface& bi = system_.GetBodyInterface();
            bi.SetLinearAndAngularVelocity(record->id, to_jolt(linear), to_jolt(angular));
            bi.ActivateBody(record->id); // Jolt would let a zero velocity leave it asleep
            {
                // A moving kinematic body must not doze off below the sleep
                // speed; a still one may, and its island with it.
                const JPH::BodyLockWrite lock(system_.GetBodyLockInterface(), record->id);
                if (lock.Succeeded() && lock.GetBody().IsKinematic()) {
                    lock.GetBody().SetAllowSleeping(linear == Vec3{0.0f} && angular == Vec3{0.0f});
                }
            }
        }
    }

    void add_force(BodyHandle handle, Vec3 force) override {
        TY_EXTERNAL_ALLOCATIONS();
        if (const BodyRecord* record = bodies_.get(handle)) {
            system_.GetBodyInterface().AddForce(record->id, to_jolt(force));
        }
    }

    void add_impulse(BodyHandle handle, Vec3 impulse) override {
        TY_EXTERNAL_ALLOCATIONS();
        if (const BodyRecord* record = bodies_.get(handle)) {
            system_.GetBodyInterface().AddImpulse(record->id, to_jolt(impulse));
        }
    }

    void add_impulse_at(BodyHandle handle, Vec3 impulse, Vec3 world_point) override {
        TY_EXTERNAL_ALLOCATIONS();
        if (const BodyRecord* record = bodies_.get(handle)) {
            system_.GetBodyInterface().AddImpulse(record->id, to_jolt(impulse), to_jolt(world_point));
        }
    }

    void step(float dt) override {
        TY_PROFILE_SCOPE_NAMED("physics::step (Jolt)");
        TY_EXTERNAL_ALLOCATIONS();
        if (!(dt > 0.0f)) {
            return;
        }
        // Jolt wants collision steps of about 1/60 s: one per 60 Hz frame,
        // more when the caller hands it a bigger slice of time.
        const int collision_steps = std::max(1, static_cast<int>(std::ceil(dt * 60.0f - 1.0e-3f)));
        contacts_.clear();
        const JPH::EPhysicsUpdateError error =
            system_.Update(dt, collision_steps, &temp_allocator_, job_system_);
        if (error != JPH::EPhysicsUpdateError::None && !reported_overflow_) {
            reported_overflow_ = true;
            TY_LOG_WARN("physics", "Jolt ran out of room for contacts (error %u); some were dropped. "
                                   "Raise WorldDesc::max_bodies.",
                        static_cast<unsigned>(error));
        }
    }

    JointHandle create_joint(const JointDesc& desc) override {
        TY_EXTERNAL_ALLOCATIONS();
        const BodyRecord* record_a = bodies_.get(desc.a);
        const BodyRecord* record_b = bodies_.get(desc.b);
        if (record_a == nullptr || (desc.b && record_b == nullptr)) {
            return JointHandle::null();
        }
        // Anchors and axes go to Jolt in world space, from where the bodies are now.
        const BodyState state_a = body_state(desc.a);
        const Pose pose_a{state_a.position, state_a.rotation};
        Pose pose_b;
        if (record_b != nullptr) {
            const BodyState state_b = body_state(desc.b);
            pose_b = Pose{state_b.position, state_b.rotation};
        }
        const JPH::Vec3 anchor_a = to_jolt(pose_a.to_world(desc.anchor_a));
        const JPH::Vec3 anchor_b = to_jolt(pose_b.to_world(desc.anchor_b));

        JPH::Ref<JPH::Constraint> constraint;
        {
            // The bodies are locked only while the constraint is built.
            JPH::BodyID ids[2] = {record_a->id, record_b != nullptr ? record_b->id : JPH::BodyID()};
            const JPH::BodyLockMultiWrite lock(system_.GetBodyLockInterface(), ids,
                                               record_b != nullptr ? 2 : 1);
            JPH::Body* body_a = lock.GetBody(0);
            JPH::Body* body_b = record_b != nullptr ? lock.GetBody(1) : &JPH::Body::sFixedToWorld;
            if (body_a == nullptr || body_b == nullptr) {
                return JointHandle::null();
            }
            constraint = build_constraint(desc, pose_a, pose_b, anchor_a, anchor_b, *body_a, *body_b);
        }
        if (constraint == nullptr) {
            return JointHandle::null();
        }
        const JointHandle handle = joints_.create(JointRecord{constraint, desc.type, desc.a, desc.b});
        if (!handle) {
            return handle;
        }
        system_.AddConstraint(constraint);
        return handle;
    }

    static JPH::Ref<JPH::Constraint> build_constraint(const JointDesc& desc, const Pose& pose_a,
                                                      const Pose& pose_b, JPH::Vec3Arg anchor_a,
                                                      JPH::Vec3Arg anchor_b, JPH::Body& body_a,
                                                      JPH::Body& body_b) {
        switch (desc.type) {
        case JointType::Distance: {
            JPH::DistanceConstraintSettings settings;
            settings.mPoint1 = anchor_a;
            settings.mPoint2 = anchor_b;
            settings.mMinDistance = desc.length;
            settings.mMaxDistance = desc.length;
            return settings.Create(body_a, body_b);
        }
        case JointType::Hinge: {
            // Jolt measures its angle as body 2 relative to body 1; ours is A
            // relative to B, so B goes in first and the angle and limits map straight across.
            JPH::HingeConstraintSettings settings;
            settings.mPoint1 = anchor_b;
            settings.mPoint2 = anchor_a;
            settings.mHingeAxis1 = to_jolt(pose_b.rotation.rotate(desc.axis_b)).Normalized();
            settings.mHingeAxis2 = to_jolt(pose_a.rotation.rotate(desc.axis_a)).Normalized();
            // One shared reference direction: the angle reads zero as built.
            settings.mNormalAxis1 = perpendicular(settings.mHingeAxis1);
            settings.mNormalAxis2 = settings.mNormalAxis1;
            if (desc.limited) {
                settings.mLimitsMin = std::min(desc.min_angle, 0.0f);
                settings.mLimitsMax = std::max(desc.max_angle, 0.0f);
            }
            return settings.Create(body_b, body_a);
        }
        }
        return nullptr;
    }

    bool destroy_joint(JointHandle handle) override {
        TY_EXTERNAL_ALLOCATIONS();
        const JointRecord* record = joints_.get(handle);
        if (record == nullptr) {
            return false;
        }
        system_.RemoveConstraint(record->constraint);
        // Cut loose, the bodies move again.
        wake_other(*record, record->a);
        wake_other(*record, record->b);
        return joints_.destroy(handle);
    }

    bool valid(JointHandle handle) const noexcept override { return joints_.contains(handle); }
    std::uint32_t joint_count() const noexcept override { return joints_.size(); }

    float hinge_angle(JointHandle handle) const override {
        const JointRecord* record = joints_.get(handle);
        if (record == nullptr || record->type != JointType::Hinge) {
            return 0.0f;
        }
        return static_cast<const JPH::HingeConstraint*>(record->constraint.GetPtr())->GetCurrentAngle();
    }

    std::uint32_t contact_count() const noexcept override {
        return static_cast<std::uint32_t>(contacts_.contacts().size());
    }

    void each_contact(void (*fn)(void* user, const Contact& contact), void* user) const override {
        for (const Contact& c : contacts_.contacts()) {
            fn(user, c);
        }
    }

    bool cast_ray(Vec3 origin, Vec3 direction, float max_distance, RayHit& hit) const override {
        TY_EXTERNAL_ALLOCATIONS();
        const JPH::RRayCast ray(to_jolt(origin), to_jolt(direction) * max_distance);
        JPH::RayCastResult result;
        if (!system_.GetNarrowPhaseQuery().CastRay(ray, result)) {
            return false;
        }
        const JPH::BodyInterface& bi = system_.GetBodyInterface();
        hit.body = BodyHandle::from_packed(bi.GetUserData(result.mBodyID));
        hit.fraction = result.mFraction;
        const JPH::RVec3 point = ray.GetPointOnRay(result.mFraction);
        hit.point = from_jolt(point);
        hit.normal = Vec3{0.0f};
        const JPH::BodyLockRead lock(system_.GetBodyLockInterface(), result.mBodyID);
        if (lock.Succeeded()) {
            hit.normal = from_jolt(lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point));
        }
        return true;
    }

private:
    static constexpr std::size_t kTempAllocatorBytes = 8 * 1024 * 1024; // a step's scratch, never grows
    static constexpr std::uint32_t kMaxJointsPerBody = 64;
#define TY_STRINGIFY_(x) #x
#define TY_STRINGIFY(x) TY_STRINGIFY_(x)
    static constexpr const char* kBackendName = "Jolt " TY_STRINGIFY(JPH_VERSION_MAJOR) "." TY_STRINGIFY(
        JPH_VERSION_MINOR) "." TY_STRINGIFY(JPH_VERSION_PATCH);
#undef TY_STRINGIFY
#undef TY_STRINGIFY_

    // Declaration order is destruction order in reverse: the filters and
    // allocators outlive the system that points at them.
    BroadPhaseLayers broad_phase_layers_;
    ObjectVsBroadPhaseFilter object_vs_broad_phase_;
    ObjectPairFilter object_pairs_;
    ContactRecorder contacts_;
    core::HandlePool<BodyRecord, BodyTag> bodies_;
    core::HandlePool<JointRecord, JointTag> joints_;
    JointHandle doomed_joints_[kMaxJointsPerBody];
    std::uint32_t doomed_count_ = 0;
    JPH::TempAllocatorImpl temp_allocator_;
    JPH::JobSystem* job_system_;
    JPH::PhysicsSystem system_;
    bool reported_overflow_ = false;
};

} // namespace

std::unique_ptr<PhysicsWorld> create_jolt_world(const WorldDesc& desc) {
    TY_EXTERNAL_ALLOCATIONS();
    return std::make_unique<JoltWorld>(desc);
}

} // namespace tynima::physics
