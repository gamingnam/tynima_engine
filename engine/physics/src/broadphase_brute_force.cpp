// The reference broadphase: every pair tested. Nothing clever, so it is the
// one the other two are checked against, and it is not slow for a few dozen
// proxies — 100 bodies is 4950 box tests, a microsecond's work.
#include <tynima/physics/broadphase.h>

#include "broadphase_common.h"

namespace tynima::physics {

namespace {

class BruteForceBroadphase final : public Broadphase {
public:
    explicit BruteForceBroadphase(std::uint32_t capacity) : proxies_(capacity) {}

    const char* name() const noexcept override { return "brute force"; }

    ProxyHandle add(const Aabb& box, std::uint64_t user_data) override {
        return proxies_.add(box, user_data);
    }
    void update(ProxyHandle proxy, const Aabb& box) override {
        if (Proxy* p = proxies_.get(proxy)) {
            p->box = box;
        }
    }
    bool remove(ProxyHandle proxy) override { return proxies_.remove(proxy); }
    bool valid(ProxyHandle proxy) const noexcept override { return proxies_.valid(proxy); }
    std::uint64_t user_data(ProxyHandle proxy) const override { return proxies_.user_data(proxy); }
    Aabb box(ProxyHandle proxy) const override { return proxies_.box(proxy); }
    std::uint32_t proxy_count() const noexcept override { return proxies_.count(); }

    void find_pairs(std::vector<ProxyPair>& out) override {
        out.clear();
        const std::vector<Proxy*>& live = proxies_.live();
        for (std::size_t i = 0; i < live.size(); ++i) {
            for (std::size_t j = i + 1; j < live.size(); ++j) {
                if (live[i]->box.overlaps(live[j]->box)) {
                    out.push_back(ordered_pair(live[i]->self, live[j]->self));
                }
            }
        }
    }

    void query(const Aabb& box, ProxyVisitor visit, void* user) const override {
        for (const Proxy* p : proxies_.live()) {
            if (p->box.overlaps(box)) {
                visit(user, p->self, p->user_data);
            }
        }
    }

    void cast_ray(const math::Vec3& origin, const math::Vec3& direction, float max_t, ProxyVisitor visit,
                  void* user) const override {
        for (const Proxy* p : proxies_.live()) {
            if (p->box.intersects_ray(origin, direction, max_t)) {
                visit(user, p->self, p->user_data);
            }
        }
    }

private:
    ProxyTable<Proxy> proxies_;
};

} // namespace

std::unique_ptr<Broadphase> create_brute_force_broadphase(std::uint32_t capacity) {
    return std::make_unique<BruteForceBroadphase>(capacity);
}

} // namespace tynima::physics
