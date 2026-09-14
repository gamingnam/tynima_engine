#pragma once

#include <cstddef>
#include <cstdint>

// Heap accounting. Core replaces the global operator new/delete family with
// malloc-backed versions that count, so any C++ allocation anywhere in the
// process — a std::vector growing, a std::string being built — shows up here
// and in Tracy's memory view. C allocations (SDL, cgltf, stb) go straight to
// malloc and are not counted.
//
// "Anywhere in the process" includes C++ inside Apple's Metal driver and
// other system frameworks, which allocate on every frame we hand them. Those
// are not the engine's doing, so platform/ and rhi/ wrap their calls into
// SDL in an ExternalAllocationScope, and the per-thread *engine* count
// excludes whatever happens inside one. The frame rule is about that count.
namespace tynima::core {

// Process-wide totals: every C++ allocation, engine or not, on any thread.
[[nodiscard]] std::uint64_t heap_allocation_count() noexcept;
[[nodiscard]] std::uint64_t heap_free_count() noexcept;
[[nodiscard]] std::uint64_t heap_bytes_in_use() noexcept;

// Allocations made on the calling thread outside any ExternalAllocationScope.
[[nodiscard]] std::uint64_t engine_allocation_count() noexcept;

// Marks the calling thread as inside external code (SDL, the GPU driver, the
// OS) until destroyed. Nested scopes are fine. Used at the engine's
// boundaries; application code should never need one.
class ExternalAllocationScope {
public:
    ExternalAllocationScope() noexcept;
    ~ExternalAllocationScope();
    ExternalAllocationScope(const ExternalAllocationScope&) = delete;
    ExternalAllocationScope& operator=(const ExternalAllocationScope&) = delete;
};

// Counts this thread's engine allocations made while alive. Wrap a frame in
// one and assert that allocations() is zero at the end: the "no per-frame
// allocation" rule, enforced. external() is the rest, for the curious.
class HeapAllocationScope {
public:
    HeapAllocationScope() noexcept : engine_start_(engine_allocation_count()), total_start_(heap_allocation_count()) {}
    [[nodiscard]] std::uint64_t allocations() const noexcept { return engine_allocation_count() - engine_start_; }
    [[nodiscard]] std::uint64_t total() const noexcept { return heap_allocation_count() - total_start_; }

private:
    std::uint64_t engine_start_;
    std::uint64_t total_start_;
};

} // namespace tynima::core

// For platform/ and rhi/: put at the top of a function that calls into SDL.
#define TY_EXTERNAL_ALLOCATIONS() const ::tynima::core::ExternalAllocationScope ty_external_allocation_scope_
