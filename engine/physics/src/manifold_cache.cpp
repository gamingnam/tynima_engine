// Manifolds that outlive a frame. An open-addressing table keyed by the
// pair of bodies; a frame stamp on each entry says whether the pair was
// seen this frame, and end_frame() rebuilds the table without the ones
// that were not. Two tables, swapped, so the rebuild allocates nothing.
#include <tynima/physics/collision.h>

namespace tynima::physics {

struct ManifoldCache::Entry {
    BodyHandle a;
    BodyHandle b;
    std::uint32_t frame = 0; // 0: empty
    Manifold manifold;
};

namespace {

std::uint32_t table_size(std::uint32_t capacity) noexcept {
    std::uint32_t size = 16;
    while (size < 2 * capacity) {
        size *= 2;
    }
    return size;
}

std::uint32_t pair_hash(BodyHandle a, BodyHandle b) noexcept {
    std::uint64_t h = a.packed() * 0x9E3779B97F4A7C15ull ^ (b.packed() + 0x7F4A7C15ull);
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return static_cast<std::uint32_t>(h);
}

} // namespace

ManifoldCache::ManifoldCache(std::uint32_t capacity)
    : base_(nullptr), entries_(nullptr), size_(table_size(capacity)), capacity_(capacity) {
    base_ = new Entry[2 * static_cast<std::size_t>(size_)];
    entries_ = base_;
}

ManifoldCache::~ManifoldCache() {
    delete[] base_;
}

void ManifoldCache::begin_frame() noexcept {
    if (++frame_ == 0) {
        frame_ = 1;
    }
}

Manifold* ManifoldCache::find(BodyHandle a, BodyHandle b) noexcept {
    const std::uint32_t mask = size_ - 1;
    std::uint32_t slot = pair_hash(a, b) & mask;
    for (std::uint32_t probe = 0; probe < size_; ++probe, slot = (slot + 1) & mask) {
        Entry& e = entries_[slot];
        if (e.frame == 0) {
            return nullptr;
        }
        if (e.a == a && e.b == b) {
            return &e.manifold;
        }
    }
    return nullptr;
}

Manifold* ManifoldCache::update(BodyHandle a, BodyHandle b, const Manifold& fresh) noexcept {
    const std::uint32_t mask = size_ - 1;
    std::uint32_t slot = pair_hash(a, b) & mask;
    Entry* entry = nullptr;
    for (std::uint32_t probe = 0; probe < size_; ++probe, slot = (slot + 1) & mask) {
        Entry& e = entries_[slot];
        if (e.frame == 0) {
            if (count_ == capacity_) {
                return nullptr;
            }
            e.a = a;
            e.b = b;
            e.manifold.count = 0;
            ++count_;
            entry = &e;
            break;
        }
        if (e.a == a && e.b == b) {
            entry = &e;
            break;
        }
    }
    if (entry == nullptr) {
        return nullptr;
    }
    entry->frame = frame_;

    // Match fresh points to last frame's by where they sit on each body,
    // and carry the impulses over.
    Manifold merged = fresh;
    const Manifold& old = entry->manifold;
    for (std::uint32_t i = 0; i < merged.count; ++i) {
        ContactPoint& point = merged.points[i];
        point.normal_impulse = 0.0f;
        point.tangent_impulse[0] = point.tangent_impulse[1] = 0.0f;
        for (std::uint32_t j = 0; j < old.count; ++j) {
            const Vec3 da = point.local_a - old.points[j].local_a;
            const Vec3 db = point.local_b - old.points[j].local_b;
            if (dot(da, da) < kMatchDistance * kMatchDistance &&
                dot(db, db) < kMatchDistance * kMatchDistance) {
                point.normal_impulse = old.points[j].normal_impulse;
                point.tangent_impulse[0] = old.points[j].tangent_impulse[0];
                point.tangent_impulse[1] = old.points[j].tangent_impulse[1];
                break;
            }
        }
    }
    entry->manifold = merged;
    return &entry->manifold;
}

void ManifoldCache::end_frame() noexcept {
    // Rebuild into the spare table with only this frame's entries, then
    // make the spare the live one.
    Entry* spare = entries_ == base_ ? base_ + size_ : base_;
    for (std::uint32_t i = 0; i < size_; ++i) {
        spare[i].frame = 0;
    }
    const std::uint32_t mask = size_ - 1;
    std::uint32_t kept = 0;
    for (std::uint32_t i = 0; i < size_; ++i) {
        const Entry& e = entries_[i];
        if (e.frame != frame_) {
            continue;
        }
        std::uint32_t slot = pair_hash(e.a, e.b) & mask;
        while (spare[slot].frame != 0) {
            slot = (slot + 1) & mask;
        }
        spare[slot] = e;
        ++kept;
    }
    count_ = kept;
    entries_ = spare;
}

std::uint32_t ManifoldCache::count() const noexcept {
    return count_;
}

std::uint32_t ManifoldCache::capacity() const noexcept {
    return capacity_;
}

void ManifoldCache::each(Visitor fn, void* user) noexcept {
    for (std::uint32_t i = 0; i < size_; ++i) {
        Entry& e = entries_[i];
        if (e.frame != 0) {
            fn(user, e.a, e.b, e.manifold);
        }
    }
}

void ManifoldCache::each(ConstVisitor fn, void* user) const noexcept {
    for (std::uint32_t i = 0; i < size_; ++i) {
        const Entry& e = entries_[i];
        if (e.frame != 0) {
            fn(user, e.a, e.b, e.manifold);
        }
    }
}

} // namespace tynima::physics
