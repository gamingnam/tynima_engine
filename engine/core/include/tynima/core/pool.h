#pragma once

#include <tynima/core/assert.h>

#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

namespace tynima::core {

// Fixed-capacity pool of T with O(1) allocate and free through an intrusive
// free list; addresses are stable for an object's lifetime. Use it where the
// owner keeps the pointers itself; where lifetimes are looser, prefer
// HandlePool (handle.h), which adds the generation check. Full pools return
// nullptr rather than growing: like the arena, the capacity is a budget.
template <typename T>
class Pool {
public:
    explicit Pool(std::size_t capacity) noexcept : capacity_(capacity) {
        if (capacity == 0) {
            return;
        }
        slots_ = static_cast<Slot*>(::operator new(sizeof(Slot) * capacity, std::align_val_t{alignof(Slot)}));
        // Thread the free list through every slot, in order, so a fresh pool
        // hands out ascending addresses.
        for (std::size_t i = 0; i < capacity; ++i) {
            slots_[i].next_free = i + 1 < capacity ? static_cast<std::uint32_t>(i + 1) : kEnd;
        }
        free_head_ = 0;
    }
    ~Pool() {
        TY_ASSERT(size_ == 0, "Pool destroyed with live objects — they will not be destructed");
        if (slots_ != nullptr) {
            ::operator delete(slots_, std::align_val_t{alignof(Slot)});
        }
    }
    Pool(Pool&& other) noexcept
        : slots_(std::exchange(other.slots_, nullptr)), capacity_(std::exchange(other.capacity_, 0)),
          size_(std::exchange(other.size_, 0)), free_head_(std::exchange(other.free_head_, kEnd)) {}
    Pool& operator=(Pool&&) = delete;
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    // Constructs a T in a free slot; nullptr when the pool is full.
    template <typename... Args>
    [[nodiscard]] T* allocate(Args&&... args) noexcept {
        if (free_head_ == kEnd) {
            return nullptr;
        }
        Slot& slot = slots_[free_head_];
        free_head_ = slot.next_free;
        ++size_;
        return new (slot.storage) T(std::forward<Args>(args)...);
    }

    // Destroys the object and returns its slot to the free list.
    void free(T* object) noexcept {
        if (object == nullptr) {
            return;
        }
        TY_ASSERT(owns(object), "Pool::free of an object from elsewhere");
        object->~T();
        const auto index = static_cast<std::uint32_t>(reinterpret_cast<Slot*>(object) - slots_);
        slots_[index].next_free = free_head_;
        free_head_ = index;
        --size_;
    }

    [[nodiscard]] bool owns(const T* object) const noexcept {
        const auto* p = reinterpret_cast<const Slot*>(object);
        return p >= slots_ && p < slots_ + capacity_ && (p - slots_) >= 0;
    }
    [[nodiscard]] std::size_t index_of(const T* object) const noexcept {
        return static_cast<std::size_t>(reinterpret_cast<const Slot*>(object) - slots_);
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool full() const noexcept { return free_head_ == kEnd; }

private:
    static constexpr std::uint32_t kEnd = 0xFFFFFFFFu;

    union Slot {
        alignas(T) unsigned char storage[sizeof(T)];
        std::uint32_t next_free;
    };
    static_assert(sizeof(Slot) >= sizeof(std::uint32_t));

    Slot* slots_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
    std::uint32_t free_head_ = kEnd;
};

} // namespace tynima::core
