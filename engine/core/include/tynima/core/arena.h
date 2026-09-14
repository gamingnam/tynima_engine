#pragma once

#include <tynima/core/assert.h>

#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace tynima::core {

// A bump allocator over one fixed block: allocation is a pointer increment,
// and everything is freed at once by reset(). Nothing is ever destructed, so
// only trivially destructible types belong here. The frame arena is reset at
// the top of every frame; a Marker gives the same trick to a scope.
//
// It never grows. Running out returns nullptr (and asserts in Debug): the
// capacity is a budget, and a budget that silently expands is not one.
class Arena {
public:
    struct Marker {
        std::size_t offset = 0;
    };

    explicit Arena(std::size_t capacity) noexcept
        : base_(capacity > 0 ? static_cast<std::byte*>(::operator new(capacity, std::align_val_t{kAlignment}))
                             : nullptr),
          capacity_(capacity) {}
    ~Arena() {
        if (base_ != nullptr) {
            ::operator delete(base_, std::align_val_t{kAlignment});
        }
    }
    Arena(Arena&& other) noexcept
        : base_(std::exchange(other.base_, nullptr)), capacity_(std::exchange(other.capacity_, 0)),
          offset_(std::exchange(other.offset_, 0)), high_water_(std::exchange(other.high_water_, 0)) {}
    Arena& operator=(Arena&&) = delete;
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    // `alignment` must be a power of two. nullptr when the arena is exhausted.
    [[nodiscard]] void* allocate(std::size_t size, std::size_t alignment = alignof(std::max_align_t)) noexcept {
        TY_ASSERT((alignment & (alignment - 1)) == 0, "Arena alignment must be a power of two");
        const std::size_t start = (offset_ + alignment - 1) & ~(alignment - 1);
        if (start + size > capacity_ || start < offset_) {
            TY_ASSERT(false, "Arena exhausted: raise its capacity or allocate less per frame");
            return nullptr;
        }
        offset_ = start + size;
        if (offset_ > high_water_) {
            high_water_ = offset_;
        }
        return base_ + start;
    }

    // Constructs one T. Nothing will ever destroy it.
    template <typename T, typename... Args>
    [[nodiscard]] T* create(Args&&... args) noexcept {
        static_assert(std::is_trivially_destructible_v<T>, "Arena never runs destructors");
        void* memory = allocate(sizeof(T), alignof(T));
        return memory != nullptr ? new (memory) T(std::forward<Args>(args)...) : nullptr;
    }

    // `count` value-initialized Ts (so default member initializers apply).
    // An empty span when the arena is exhausted.
    template <typename T>
    [[nodiscard]] std::span<T> create_array(std::size_t count) noexcept {
        static_assert(std::is_trivially_destructible_v<T>, "Arena never runs destructors");
        if (count == 0) {
            return {};
        }
        void* memory = allocate(sizeof(T) * count, alignof(T));
        if (memory == nullptr) {
            return {};
        }
        T* first = static_cast<T*>(memory);
        for (std::size_t i = 0; i < count; ++i) {
            new (first + i) T();
        }
        return {first, count};
    }

    [[nodiscard]] Marker mark() const noexcept { return {offset_}; }
    void reset_to(Marker marker) noexcept {
        TY_ASSERT(marker.offset <= offset_, "Arena marker is from after the current position");
        offset_ = marker.offset;
    }
    void reset() noexcept { offset_ = 0; }

    [[nodiscard]] std::size_t used() const noexcept { return offset_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    // The most ever used since construction: what the budget actually needs to be.
    [[nodiscard]] std::size_t high_water() const noexcept { return high_water_; }

private:
    static constexpr std::size_t kAlignment = 64; // a cache line; every alignment request divides it

    std::byte* base_;
    std::size_t capacity_;
    std::size_t offset_ = 0;
    std::size_t high_water_ = 0;
};

} // namespace tynima::core
