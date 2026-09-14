#pragma once

#include <tynima/core/handle.h>
#include <tynima/core/math.h>

#include <cstdint>
#include <memory>
#include <vector>

// The broadphase: which pairs of bodies might be touching, found without
// testing every pair against every other. It works on proxies — a box and a
// value the caller attaches (a body handle) — and knows nothing about shapes;
// the narrowphase takes the pairs it finds and answers for real.
//
// Three implementations behind one interface, all giving the same answers:
//   brute force        every pair tested, O(n^2): the reference, and fine for tens of bodies
//   sweep and prune    endpoints sorted along one axis, kept sorted as things move
//   dynamic AABB tree  fat boxes in a balanced binary tree; also the fast one for queries
//
// Every box is closed (see math::Aabb): two boxes sharing a face overlap.
namespace tynima::physics {

using math::Aabb;

struct ProxyTag {};
using ProxyHandle = core::Handle<ProxyTag>;

// Two proxies whose boxes overlap, a.index < b.index, so a pair is one
// thing, not two, and two implementations' results compare directly.
struct ProxyPair {
    ProxyHandle a;
    ProxyHandle b;
    friend constexpr bool operator==(const ProxyPair& x, const ProxyPair& y) noexcept = default;
};

// Callback for queries: the proxy and its user data. Allocation-free by
// design; the template overloads below take lambdas.
using ProxyVisitor = void (*)(void* user, ProxyHandle proxy, std::uint64_t user_data);

class Broadphase {
public:
    virtual ~Broadphase() = default;
    Broadphase(const Broadphase&) = delete;
    Broadphase& operator=(const Broadphase&) = delete;

    [[nodiscard]] virtual const char* name() const noexcept = 0;

    // The null handle when the broadphase is full.
    [[nodiscard]] virtual ProxyHandle add(const Aabb& box, std::uint64_t user_data) = 0;
    // The box moved or changed size. A stale handle is ignored.
    virtual void update(ProxyHandle proxy, const Aabb& box) = 0;
    virtual bool remove(ProxyHandle proxy) = 0; // false for a stale handle
    [[nodiscard]] virtual bool valid(ProxyHandle proxy) const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t user_data(ProxyHandle proxy) const = 0; // 0 for a stale handle
    [[nodiscard]] virtual Aabb box(ProxyHandle proxy) const = 0;                // empty for a stale handle
    [[nodiscard]] virtual std::uint32_t proxy_count() const noexcept = 0;

    // Replaces `out` with every overlapping pair. Keep the vector across
    // calls and it stops allocating once it has grown to the scene.
    virtual void find_pairs(std::vector<ProxyPair>& out) = 0;

    // Every proxy whose box overlaps `box`, in no particular order.
    virtual void query(const Aabb& box, ProxyVisitor visit, void* user) const = 0;
    // Every proxy whose box the segment origin + direction * t, t in
    // [0, max_t], passes through, in no particular order: candidates for
    // the caller to narrow. `direction` need not be unit length.
    virtual void cast_ray(const math::Vec3& origin, const math::Vec3& direction, float max_t,
                          ProxyVisitor visit, void* user) const = 0;

    // An implementation's own consistency check, O(n) or worse: for tests.
    [[nodiscard]] virtual bool self_check() const { return true; }

    template <typename Fn>
    void query(const Aabb& box, Fn&& fn) const {
        query(box, &Broadphase::thunk<Fn>, const_cast<void*>(static_cast<const void*>(&fn)));
    }
    template <typename Fn>
    void cast_ray(const math::Vec3& origin, const math::Vec3& direction, float max_t, Fn&& fn) const {
        cast_ray(origin, direction, max_t, &Broadphase::thunk<Fn>,
                 const_cast<void*>(static_cast<const void*>(&fn)));
    }

protected:
    Broadphase() = default;

private:
    template <typename Fn>
    static void thunk(void* user, ProxyHandle proxy, std::uint64_t user_data) {
        (*static_cast<Fn*>(user))(proxy, user_data);
    }
};

// `capacity` proxies at most; nothing grows after construction.
[[nodiscard]] std::unique_ptr<Broadphase> create_brute_force_broadphase(std::uint32_t capacity);
[[nodiscard]] std::unique_ptr<Broadphase> create_sweep_and_prune(std::uint32_t capacity);
// `margin` fattens each proxy's box in the tree, so a proxy moving less than
// that stays put: the tree is touched only when something moves further.
[[nodiscard]] std::unique_ptr<Broadphase> create_aabb_tree(std::uint32_t capacity, float margin = 0.1f);

} // namespace tynima::physics
