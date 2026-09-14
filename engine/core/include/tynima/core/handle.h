#pragma once

#include <tynima/core/assert.h>

#include <cstdint>
#include <new>
#include <utility>

namespace tynima::core {

// A generational handle: which slot, and which lifetime of that slot. A slot
// that is destroyed and reused gets a new generation, so every handle to the
// old object stops resolving — a use-after-free is a null lookup, not
// undefined behaviour. Generation 0 is reserved for the null handle.
//
// `Tag` only keeps handle types apart: a Handle<BufferTag> cannot be passed
// where a Handle<TextureTag> is expected, even though both are two integers.
template <typename Tag>
struct Handle {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    [[nodiscard]] static constexpr Handle null() noexcept { return {}; }
    [[nodiscard]] constexpr bool is_null() const noexcept { return generation == 0; }
    constexpr explicit operator bool() const noexcept { return generation != 0; }

    // One integer, for hashing and serialization.
    [[nodiscard]] constexpr std::uint64_t packed() const noexcept {
        return (static_cast<std::uint64_t>(generation) << 32) | index;
    }
    [[nodiscard]] static constexpr Handle from_packed(std::uint64_t packed) noexcept {
        return {static_cast<std::uint32_t>(packed & 0xFFFFFFFFu), static_cast<std::uint32_t>(packed >> 32)};
    }

    friend constexpr bool operator==(Handle a, Handle b) noexcept = default;
};

// Fixed-capacity storage that hands out Handles instead of pointers. O(1)
// create, destroy and lookup; stable addresses while an object lives, so a
// T* from get() is fine to hold for the duration of a call and no longer.
// Full pools return the null handle rather than growing.
template <typename T, typename Tag = T>
class HandlePool {
public:
    using HandleType = Handle<Tag>;

    explicit HandlePool(std::uint32_t capacity) noexcept : capacity_(capacity) {
        if (capacity == 0) {
            return;
        }
        slots_ = static_cast<Slot*>(::operator new(sizeof(Slot) * capacity, std::align_val_t{alignof(Slot)}));
        for (std::uint32_t i = 0; i < capacity; ++i) {
            slots_[i].generation = 1;
            slots_[i].live = false;
            slots_[i].next_free = i + 1 < capacity ? i + 1 : kEnd;
        }
        free_head_ = 0;
    }
    ~HandlePool() {
        // Handles do not own; the pool does. Anything still alive is destroyed here.
        for (std::uint32_t i = 0; i < capacity_; ++i) {
            if (slots_[i].live) {
                object(i)->~T();
            }
        }
        if (slots_ != nullptr) {
            ::operator delete(slots_, std::align_val_t{alignof(Slot)});
        }
    }
    HandlePool(HandlePool&& other) noexcept
        : slots_(std::exchange(other.slots_, nullptr)), capacity_(std::exchange(other.capacity_, 0)),
          size_(std::exchange(other.size_, 0)), free_head_(std::exchange(other.free_head_, kEnd)) {}
    HandlePool& operator=(HandlePool&&) = delete;
    HandlePool(const HandlePool&) = delete;
    HandlePool& operator=(const HandlePool&) = delete;

    // Constructs a T; the null handle when the pool is full.
    template <typename... Args>
    [[nodiscard]] HandleType create(Args&&... args) noexcept {
        if (free_head_ == kEnd) {
            return HandleType::null();
        }
        const std::uint32_t index = free_head_;
        Slot& slot = slots_[index];
        free_head_ = slot.next_free;
        new (slot.storage) T(std::forward<Args>(args)...);
        slot.live = true;
        ++size_;
        return {index, slot.generation};
    }

    // Destroys the object and retires every handle to it. False for a null
    // or stale handle — destroying twice is a bug in the caller, but a
    // harmless one here.
    bool destroy(HandleType handle) noexcept {
        Slot* slot = resolve(handle);
        if (slot == nullptr) {
            return false;
        }
        object(handle.index)->~T();
        slot->live = false;
        if (++slot->generation == 0) {
            slot->generation = 1; // never hand out the null generation
        }
        slot->next_free = free_head_;
        free_head_ = handle.index;
        --size_;
        return true;
    }

    // nullptr for a null or stale handle: the check that makes this worth it.
    [[nodiscard]] T* get(HandleType handle) noexcept {
        return resolve(handle) != nullptr ? object(handle.index) : nullptr;
    }
    [[nodiscard]] const T* get(HandleType handle) const noexcept {
        return resolve(handle) != nullptr ? object(handle.index) : nullptr;
    }
    [[nodiscard]] bool contains(HandleType handle) const noexcept { return resolve(handle) != nullptr; }

    [[nodiscard]] std::uint32_t size() const noexcept { return size_; }
    [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool full() const noexcept { return free_head_ == kEnd; }

    // Visits every live object as fn(HandleType, T&). Creating or destroying
    // during the visit is not allowed.
    template <typename Fn>
    void for_each(Fn&& fn) {
        for (std::uint32_t i = 0; i < capacity_; ++i) {
            if (slots_[i].live) {
                fn(HandleType{i, slots_[i].generation}, *object(i));
            }
        }
    }

private:
    static constexpr std::uint32_t kEnd = 0xFFFFFFFFu;

    struct Slot {
        alignas(T) unsigned char storage[sizeof(T)];
        std::uint32_t generation;
        std::uint32_t next_free;
        bool live;
    };

    [[nodiscard]] T* object(std::uint32_t index) noexcept { return reinterpret_cast<T*>(slots_[index].storage); }
    [[nodiscard]] const T* object(std::uint32_t index) const noexcept {
        return reinterpret_cast<const T*>(slots_[index].storage);
    }
    [[nodiscard]] Slot* resolve(HandleType handle) const noexcept {
        if (handle.generation == 0 || handle.index >= capacity_) {
            return nullptr;
        }
        Slot& slot = slots_[handle.index];
        return slot.live && slot.generation == handle.generation ? &slot : nullptr;
    }

    Slot* slots_ = nullptr;
    std::uint32_t capacity_ = 0;
    std::uint32_t size_ = 0;
    std::uint32_t free_head_ = kEnd;
};

} // namespace tynima::core
