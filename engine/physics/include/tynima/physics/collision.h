#pragma once

#include <tynima/core/handle.h>
#include <tynima/core/math.h>
#include <tynima/physics/physics.h>

#include <cstdint>

// The narrowphase: given two convex shapes and where they are, do they
// touch, how deep, along which direction, and where — as a manifold of up
// to four contact points a solver can hold a stack still with.
//
//   GJK    the distance between two convex cores and the closest points, or
//          the fact that they overlap. Works on support mappings alone, so
//          one algorithm covers every convex shape.
//   EPA    when the cores overlap: the shortest way out, from the simplex
//          GJK stopped at, by expanding a polytope of the Minkowski
//          difference until the closest face stops moving.
//   collide()  puts them together with the convex radius trick — a sphere
//          is a point plus a radius, a capsule a segment plus a radius, so
//          only boxes ever need EPA — then builds the manifold by clipping
//          the two supporting faces against each other.
//   ManifoldCache  keeps manifolds from frame to frame, matching points by
//          where they sit on each body, so the solver's impulses carry over.
//
// Normals point from A to B. Depth is positive when the surfaces overlap and
// negative while they are still apart but within the speculative distance.
namespace tynima::physics {

using math::Quat;
using math::Vec3;

struct Pose {
    Vec3 position{0.0f};
    Quat rotation = Quat::identity();

    [[nodiscard]] Vec3 to_world(const Vec3& local) const noexcept {
        return position + rotation.rotate(local);
    }
    [[nodiscard]] Vec3 to_local(const Vec3& world) const noexcept {
        return math::conjugate(rotation).rotate(world - position);
    }
    [[nodiscard]] Vec3 direction_to_local(const Vec3& world) const noexcept {
        return math::conjugate(rotation).rotate(world);
    }
};

// A shape at a pose, as the algorithms see it: a convex core with a radius
// around it. Box: the box itself, radius 0. Sphere: its centre, radius r.
// Capsule: its axis segment, radius r.
struct Convex {
    Shape shape;
    Pose pose;

    // The point of the core farthest along `direction` (world space; the
    // direction need not be unit length).
    [[nodiscard]] Vec3 support(const Vec3& direction) const noexcept;
    [[nodiscard]] float radius() const noexcept;
    // The core's face (4 points, a box), edge (2, a capsule) or nothing (a
    // sphere) that faces `direction` most: what a manifold is clipped from.
    // Points are in world space, in order around the face.
    [[nodiscard]] std::uint32_t supporting_face(const Vec3& direction, Vec3 out[4]) const noexcept;
    // The world-space box around the whole shape, radius included.
    [[nodiscard]] math::Aabb bounds() const noexcept;
};

// GJK's working set: up to four points of the Minkowski difference A - B,
// each remembering which support points of A and B made it, with the
// barycentric weight of the closest point on it to the origin.
struct Simplex {
    struct Vertex {
        Vec3 w;  // wa - wb
        Vec3 wa; // support of A
        Vec3 wb; // support of B
        float weight = 0.0f;
    };
    Vertex v[4];
    std::uint32_t count = 0;
};

struct GjkResult {
    bool intersecting = false; // the cores overlap; distance is 0 and the points mean nothing
    float distance = 0.0f;     // between the cores
    Vec3 on_a{0.0f};           // closest point on A's core
    Vec3 on_b{0.0f};           // closest point on B's core
    Simplex simplex;           // where it stopped, for EPA
};

[[nodiscard]] GjkResult gjk(const Convex& a, const Convex& b) noexcept;

struct EpaResult {
    bool ok = false;
    Vec3 normal{0.0f}; // from A to B: move B this way by `depth` to separate the cores
    float depth = 0.0f;
    Vec3 on_a{0.0f}; // deepest points, on the cores
    Vec3 on_b{0.0f};
};

// Needs the cores to overlap; `simplex` is what gjk() returned.
[[nodiscard]] EpaResult epa(const Convex& a, const Convex& b, const Simplex& simplex) noexcept;

struct ContactPoint {
    Vec3 on_a{0.0f}; // on A's surface, world space
    Vec3 on_b{0.0f}; // on B's surface
    Vec3 local_a{0.0f}; // the same points in each body's frame: the point's identity across frames
    Vec3 local_b{0.0f};
    float depth = 0.0f; // along the manifold normal; > 0 when overlapping
    // The solver's accumulated impulses, carried over by the ManifoldCache
    // for warm starting. Zero for a point that is new this frame.
    float normal_impulse = 0.0f;
    float tangent_impulse[2] = {0.0f, 0.0f};
};

struct Manifold {
    static constexpr std::uint32_t kMaxPoints = 4;
    Vec3 normal{0.0f}; // from A to B
    std::uint32_t count = 0;
    ContactPoint points[kMaxPoints];
};

// The whole narrowphase for one pair. True when the surfaces overlap or
// are within `speculative_distance` of each other; `out` then holds the
// normal and up to four points. False (and out.count == 0) otherwise.
[[nodiscard]] bool collide(const Convex& a, const Convex& b, float speculative_distance,
                           Manifold& out) noexcept;

// Manifolds that persist across frames, one per touching pair. Each frame:
// begin_frame(), then update() every pair the narrowphase found, then
// end_frame() to drop the pairs that were not. A point that was there last
// frame (within kMatchDistance of the same spot on both bodies) keeps its
// impulses; everything else starts from zero.
class ManifoldCache {
public:
    static constexpr float kMatchDistance = 0.02f;

    explicit ManifoldCache(std::uint32_t capacity);
    ~ManifoldCache();
    ManifoldCache(const ManifoldCache&) = delete;
    ManifoldCache& operator=(const ManifoldCache&) = delete;

    void begin_frame() noexcept;
    // Stores `fresh` for the pair, carrying impulses over from last frame's
    // points that match. Returns the stored manifold (null when full).
    Manifold* update(BodyHandle a, BodyHandle b, const Manifold& fresh) noexcept;
    void end_frame() noexcept;

    [[nodiscard]] Manifold* find(BodyHandle a, BodyHandle b) noexcept;
    [[nodiscard]] std::uint32_t count() const noexcept;
    [[nodiscard]] std::uint32_t capacity() const noexcept;

    // Visits every manifold as fn(user, a, b, manifold).
    using Visitor = void (*)(void* user, BodyHandle a, BodyHandle b, Manifold& manifold);
    void each(Visitor fn, void* user) noexcept;
    template <typename Fn>
    void each(Fn&& fn) noexcept {
        each([](void* user, BodyHandle a, BodyHandle b, Manifold& m) { (*static_cast<Fn*>(user))(a, b, m); },
             &fn);
    }

private:
    struct Entry;
    Entry* base_;    // two tables of size_ entries, back to back
    Entry* entries_; // the live one
    std::uint32_t size_;
    std::uint32_t capacity_;
    std::uint32_t count_ = 0;
    std::uint32_t frame_ = 1;
};

} // namespace tynima::physics
