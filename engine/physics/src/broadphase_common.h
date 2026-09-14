#pragma once

// Shared by the broadphase implementations: the proxy record and the table
// that hands out handles for it. Private to the physics module.
#include <tynima/core/handle.h>
#include <tynima/physics/broadphase.h>

#include <cstdint>
#include <vector>

namespace tynima::physics {

struct Proxy {
    Aabb box; // the caller's box, exactly: what overlap answers are about
    std::uint64_t user_data = 0;
    ProxyHandle self;
    std::uint32_t live_index = 0; // where it sits in ProxyTable::live()
};

[[nodiscard]] inline ProxyPair ordered_pair(ProxyHandle a, ProxyHandle b) noexcept {
    return a.index < b.index ? ProxyPair{a, b} : ProxyPair{b, a};
}

// Proxies by handle, with stable addresses while they live, plus a dense
// list of the live ones for the implementations that scan. P extends Proxy.
template <typename P>
class ProxyTable {
public:
    explicit ProxyTable(std::uint32_t capacity) : pool_(capacity) { live_.reserve(capacity); }

    ProxyHandle add(const Aabb& box, std::uint64_t user_data) {
        const ProxyHandle handle = pool_.create();
        if (!handle) {
            return handle;
        }
        P* p = pool_.get(handle);
        p->box = box;
        p->user_data = user_data;
        p->self = handle;
        p->live_index = static_cast<std::uint32_t>(live_.size());
        live_.push_back(p);
        return handle;
    }

    bool remove(ProxyHandle handle) {
        P* p = pool_.get(handle);
        if (p == nullptr) {
            return false;
        }
        P* last = live_.back();
        live_[p->live_index] = last;
        last->live_index = p->live_index;
        live_.pop_back();
        return pool_.destroy(handle);
    }

    [[nodiscard]] P* get(ProxyHandle handle) noexcept { return pool_.get(handle); }
    [[nodiscard]] const P* get(ProxyHandle handle) const noexcept { return pool_.get(handle); }
    [[nodiscard]] bool valid(ProxyHandle handle) const noexcept { return pool_.contains(handle); }
    [[nodiscard]] std::uint64_t user_data(ProxyHandle handle) const noexcept {
        const P* p = pool_.get(handle);
        return p != nullptr ? p->user_data : 0;
    }
    [[nodiscard]] Aabb box(ProxyHandle handle) const noexcept {
        const P* p = pool_.get(handle);
        return p != nullptr ? p->box : Aabb::empty();
    }
    [[nodiscard]] std::uint32_t count() const noexcept { return pool_.size(); }
    [[nodiscard]] std::uint32_t capacity() const noexcept { return pool_.capacity(); }
    [[nodiscard]] const std::vector<P*>& live() const noexcept { return live_; }

private:
    core::HandlePool<P, ProxyTag> pool_;
    std::vector<P*> live_;
};

} // namespace tynima::physics
