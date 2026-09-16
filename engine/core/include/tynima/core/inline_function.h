#pragma once

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace tynima::core {

// A callable stored in place: a lambda and its captures, in `Capacity` bytes
// of the object itself, called through one function pointer. Where
// std::function would reach for the heap once the captures outgrow its small
// buffer, this refuses to compile — which is the point in code that runs
// every frame and must never allocate. Captures must be trivially copyable
// (references, pointers, plain values: what a frame's callbacks capture), so
// copying one is a memcpy and there is nothing to destroy.
template <typename Signature, std::size_t Capacity = 64>
class InlineFunction;

template <typename R, typename... Args, std::size_t Capacity>
class InlineFunction<R(Args...), Capacity> {
public:
    InlineFunction() = default;

    template <typename F>
        requires(!std::is_same_v<std::decay_t<F>, InlineFunction>)
    InlineFunction(F&& f) noexcept { // NOLINT(google-explicit-constructor): assigns like std::function
        assign(std::forward<F>(f));
    }

    template <typename F>
    void assign(F&& f) noexcept {
        using Fn = std::decay_t<F>;
        static_assert(sizeof(Fn) <= Capacity,
                      "InlineFunction: the captures do not fit; capture less, by reference, or raise Capacity");
        static_assert(alignof(Fn) <= alignof(std::max_align_t), "InlineFunction: over-aligned callable");
        static_assert(std::is_trivially_copyable_v<Fn> && std::is_trivially_destructible_v<Fn>,
                      "InlineFunction: captures must be trivially copyable (no std::string, no std::vector, "
                      "nothing that owns memory)");
        new (storage_) Fn(std::forward<F>(f));
        call_ = [](void* storage, Args... args) -> R {
            return (*static_cast<Fn*>(storage))(std::forward<Args>(args)...);
        };
    }

    R operator()(Args... args) {
        return call_(storage_, std::forward<Args>(args)...);
    }

    [[nodiscard]] explicit operator bool() const noexcept { return call_ != nullptr; }
    void reset() noexcept { call_ = nullptr; }

private:
    alignas(std::max_align_t) unsigned char storage_[Capacity];
    R (*call_)(void*, Args...) = nullptr;
};

} // namespace tynima::core
