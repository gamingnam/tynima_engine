// Sweep and prune. Every box projects to an interval on the sweep axis; the
// interval endpoints live in one array kept sorted. Bodies move a little
// per frame, so the array stays nearly sorted and an insertion-sort step
// per moved endpoint keeps it that way in about linear time. find_pairs()
// walks the array once: a box that starts while others are still open is
// tested against exactly those, in all three axes.
//
// The sweep axis is the one the scene is most spread along, re-chosen when
// another axis becomes clearly better — the pile spreads across the floor,
// so x or z, never y.
#include <tynima/physics/broadphase.h>

#include "broadphase_common.h"

#include <algorithm>
#include <initializer_list>

namespace tynima::physics {

namespace {

struct SapProxy : Proxy {
    std::uint32_t endpoint[2] = {0, 0}; // index of the min and max endpoint in the array
};

struct Endpoint {
    float value;
    SapProxy* proxy;
    bool is_max;
};

// Sort order: by value; a min before a max at the same value, so two boxes
// that share a face are still open together (closed intervals).
[[nodiscard]] bool before(const Endpoint& a, const Endpoint& b) noexcept {
    return a.value < b.value || (a.value == b.value && !a.is_max && b.is_max);
}

[[nodiscard]] float axis_value(const Aabb& box, int axis, bool is_max) noexcept {
    const math::Vec3& v = is_max ? box.max : box.min;
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

class SweepAndPrune final : public Broadphase {
public:
    explicit SweepAndPrune(std::uint32_t capacity) : proxies_(capacity) {
        endpoints_.reserve(2 * static_cast<std::size_t>(capacity));
        active_.reserve(capacity);
    }

    const char* name() const noexcept override { return "sweep and prune"; }

    ProxyHandle add(const Aabb& box, std::uint64_t user_data) override {
        const ProxyHandle handle = proxies_.add(box, user_data);
        if (!handle) {
            return handle;
        }
        SapProxy* p = proxies_.get(handle);
        // Append both endpoints, then let each sink to its place.
        for (const bool is_max : {false, true}) {
            const auto index = static_cast<std::uint32_t>(endpoints_.size());
            endpoints_.push_back({axis_value(box, axis_, is_max), p, is_max});
            p->endpoint[is_max ? 1 : 0] = index;
            sift(index);
        }
        return handle;
    }

    void update(ProxyHandle proxy, const Aabb& box) override {
        SapProxy* p = proxies_.get(proxy);
        if (p == nullptr) {
            return;
        }
        p->box = box;
        for (const bool is_max : {false, true}) {
            const std::uint32_t index = p->endpoint[is_max ? 1 : 0];
            endpoints_[index].value = axis_value(box, axis_, is_max);
            sift(index);
        }
    }

    bool remove(ProxyHandle proxy) override {
        SapProxy* p = proxies_.get(proxy);
        if (p == nullptr) {
            return false;
        }
        // Erase the later endpoint first so the earlier index stays valid.
        std::uint32_t first = p->endpoint[0], second = p->endpoint[1];
        if (first > second) {
            std::swap(first, second);
        }
        erase_endpoint(second);
        erase_endpoint(first);
        return proxies_.remove(proxy);
    }

    bool valid(ProxyHandle proxy) const noexcept override { return proxies_.valid(proxy); }
    std::uint64_t user_data(ProxyHandle proxy) const override { return proxies_.user_data(proxy); }
    Aabb box(ProxyHandle proxy) const override { return proxies_.box(proxy); }
    std::uint32_t proxy_count() const noexcept override { return proxies_.count(); }

    void find_pairs(std::vector<ProxyPair>& out) override {
        out.clear();
        choose_axis();
        active_.clear();
        for (const Endpoint& e : endpoints_) {
            if (e.is_max) {
                // Close the interval: swap-remove from the open set.
                for (std::size_t i = 0; i < active_.size(); ++i) {
                    if (active_[i] == e.proxy) {
                        active_[i] = active_.back();
                        active_.pop_back();
                        break;
                    }
                }
                continue;
            }
            for (const SapProxy* other : active_) {
                if (other->box.overlaps(e.proxy->box)) {
                    out.push_back(ordered_pair(other->self, e.proxy->self));
                }
            }
            active_.push_back(e.proxy);
        }
    }

    void query(const Aabb& box, ProxyVisitor visit, void* user) const override {
        // The sorted array is a pair finder; a query walks it and stops once
        // it is past the box on the sweep axis.
        const float end = axis_value(box, axis_, true);
        for (const Endpoint& e : endpoints_) {
            if (e.value > end) {
                break;
            }
            if (!e.is_max && e.proxy->box.overlaps(box)) {
                visit(user, e.proxy->self, e.proxy->user_data);
            }
        }
    }

    void cast_ray(const math::Vec3& origin, const math::Vec3& direction, float max_t, ProxyVisitor visit,
                  void* user) const override {
        for (const SapProxy* p : proxies_.live()) {
            if (p->box.intersects_ray(origin, direction, max_t)) {
                visit(user, p->self, p->user_data);
            }
        }
    }

    bool self_check() const override {
        if (endpoints_.size() != 2 * static_cast<std::size_t>(proxies_.count())) {
            return false;
        }
        for (std::size_t i = 0; i < endpoints_.size(); ++i) {
            const Endpoint& e = endpoints_[i];
            if (e.proxy->endpoint[e.is_max ? 1 : 0] != i) {
                return false;
            }
            if (e.value != axis_value(e.proxy->box, axis_, e.is_max)) {
                return false;
            }
            if (i > 0 && before(e, endpoints_[i - 1])) {
                return false;
            }
        }
        return true;
    }

private:
    // Moves endpoints_[index] left or right until the array is sorted again.
    void sift(std::uint32_t index) {
        while (index > 0 && before(endpoints_[index], endpoints_[index - 1])) {
            swap_endpoints(index, index - 1);
            --index;
        }
        while (index + 1 < endpoints_.size() && before(endpoints_[index + 1], endpoints_[index])) {
            swap_endpoints(index, index + 1);
            ++index;
        }
    }

    void swap_endpoints(std::uint32_t a, std::uint32_t b) {
        std::swap(endpoints_[a], endpoints_[b]);
        link(a);
        link(b);
    }

    // Tells the proxy where its endpoint now is.
    void link(std::uint32_t index) {
        const Endpoint& e = endpoints_[index];
        e.proxy->endpoint[e.is_max ? 1 : 0] = index;
    }

    void erase_endpoint(std::uint32_t index) {
        endpoints_.erase(endpoints_.begin() + index);
        for (std::size_t i = index; i < endpoints_.size(); ++i) {
            link(static_cast<std::uint32_t>(i));
        }
    }

    // Sweeps along the axis the box centres vary most in. Re-sorting is
    // O(n log n), so the axis only changes when another is clearly better.
    void choose_axis() {
        const std::vector<SapProxy*>& live = proxies_.live();
        if (live.size() < 2) {
            return;
        }
        math::Vec3 mean{0.0f};
        for (const SapProxy* p : live) {
            mean += p->box.center();
        }
        mean *= 1.0f / static_cast<float>(live.size());
        math::Vec3 variance{0.0f};
        for (const SapProxy* p : live) {
            const math::Vec3 d = p->box.center() - mean;
            variance += d * d;
        }
        const float v[3] = {variance.x, variance.y, variance.z};
        int best = 0;
        for (int axis = 1; axis < 3; ++axis) {
            if (v[axis] > v[best]) {
                best = axis;
            }
        }
        if (best != axis_ && v[best] > 1.5f * v[axis_]) {
            axis_ = best;
            for (Endpoint& e : endpoints_) {
                e.value = axis_value(e.proxy->box, axis_, e.is_max);
            }
            std::sort(endpoints_.begin(), endpoints_.end(), before);
            for (std::size_t i = 0; i < endpoints_.size(); ++i) {
                link(static_cast<std::uint32_t>(i));
            }
        }
    }

    ProxyTable<SapProxy> proxies_;
    std::vector<Endpoint> endpoints_;
    std::vector<SapProxy*> active_; // scratch for find_pairs
    int axis_ = 0;
};

} // namespace

std::unique_ptr<Broadphase> create_sweep_and_prune(std::uint32_t capacity) {
    return std::make_unique<SweepAndPrune>(capacity);
}

} // namespace tynima::physics
