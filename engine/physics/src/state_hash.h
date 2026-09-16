#pragma once

#include <tynima/physics/physics.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

// FNV-1a over the bytes of what a body is: the same on every platform the
// engine builds for (all little-endian, all IEEE floats).
namespace tynima::physics {

class StateHasher {
public:
    void add(const void* bytes, std::size_t size) noexcept {
        const auto* p = static_cast<const unsigned char*>(bytes);
        for (std::size_t i = 0; i < size; ++i) {
            hash_ ^= p[i];
            hash_ *= 0x100000001b3ull;
        }
    }
    template <typename T>
    void add(const T& value) noexcept {
        static_assert(!std::is_pointer_v<T>);
        add(&value, sizeof value);
    }
    void add_body(BodyHandle handle, const BodyState& state) noexcept {
        add(handle.index);
        add(handle.generation);
        add(state.position.x);
        add(state.position.y);
        add(state.position.z);
        add(state.rotation.x);
        add(state.rotation.y);
        add(state.rotation.z);
        add(state.rotation.w);
        add(state.linear_velocity.x);
        add(state.linear_velocity.y);
        add(state.linear_velocity.z);
        add(state.angular_velocity.x);
        add(state.angular_velocity.y);
        add(state.angular_velocity.z);
        add(static_cast<std::uint8_t>(state.active ? 1 : 0));
    }
    [[nodiscard]] std::uint64_t value() const noexcept { return hash_; }

private:
    std::uint64_t hash_ = 0xcbf29ce484222325ull;
};

} // namespace tynima::physics
