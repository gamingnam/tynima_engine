#include <tynima/core/memory.h>

#include <tynima/core/profile.h>

#include <atomic>
#include <cstdlib>
#include <new>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace {

std::atomic<std::uint64_t> g_allocations{0};
std::atomic<std::uint64_t> g_frees{0};
std::atomic<std::uint64_t> g_bytes{0};
thread_local std::uint64_t t_engine_allocations = 0;
thread_local int t_external_depth = 0;

// Every block carries its size in a header, so frees can be accounted without
// relying on the sized-delete overloads, which callers may or may not hit.
// Alignment is preserved by making the header a whole alignment unit.
struct Header {
    std::size_t size;
    std::size_t alignment;
};

std::size_t header_span(std::size_t alignment) noexcept {
    const std::size_t unit = alignment > sizeof(Header) ? alignment : sizeof(Header);
    return unit;
}

void* raw_allocate(std::size_t bytes, std::size_t alignment) noexcept {
#if defined(_MSC_VER)
    return _aligned_malloc(bytes, alignment);
#else
    void* p = nullptr;
    if (posix_memalign(&p, alignment < sizeof(void*) ? sizeof(void*) : alignment, bytes) != 0) {
        return nullptr;
    }
    return p;
#endif
}

void raw_free(void* p) noexcept {
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

void* allocate(std::size_t size, std::size_t alignment) noexcept {
    const std::size_t span = header_span(alignment);
    auto* block = static_cast<unsigned char*>(raw_allocate(span + size, span));
    if (block == nullptr) {
        return nullptr;
    }
    auto* header = reinterpret_cast<Header*>(block + span - sizeof(Header));
    header->size = size;
    header->alignment = alignment;
    void* user = block + span;
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    g_bytes.fetch_add(size, std::memory_order_relaxed);
    if (t_external_depth == 0) {
        ++t_engine_allocations;
    }
#if defined(TYNIMA_PROFILE)
    TracyAlloc(user, size);
#endif
    return user;
}

void release(void* user) noexcept {
    if (user == nullptr) {
        return;
    }
    auto* header = static_cast<Header*>(user) - 1;
    const std::size_t span = header_span(header->alignment);
    g_frees.fetch_add(1, std::memory_order_relaxed);
    g_bytes.fetch_sub(header->size, std::memory_order_relaxed);
#if defined(TYNIMA_PROFILE)
    TracyFree(user);
#endif
    raw_free(static_cast<unsigned char*>(user) - span);
}

void* allocate_or_throw(std::size_t size, std::size_t alignment) {
    void* p = allocate(size == 0 ? 1 : size, alignment);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

} // namespace

namespace tynima::core {

std::uint64_t heap_allocation_count() noexcept {
    return g_allocations.load(std::memory_order_relaxed);
}
std::uint64_t heap_free_count() noexcept {
    return g_frees.load(std::memory_order_relaxed);
}
std::uint64_t heap_bytes_in_use() noexcept {
    return g_bytes.load(std::memory_order_relaxed);
}

std::uint64_t engine_allocation_count() noexcept {
    return t_engine_allocations;
}

ExternalAllocationScope::ExternalAllocationScope() noexcept {
    ++t_external_depth;
}

ExternalAllocationScope::~ExternalAllocationScope() {
    --t_external_depth;
}

} // namespace tynima::core

// ---- the replaceable global allocation functions, all of them ----

void* operator new(std::size_t size) {
    return allocate_or_throw(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}
void* operator new[](std::size_t size) {
    return allocate_or_throw(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocate_or_throw(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocate_or_throw(size, static_cast<std::size_t>(alignment));
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return allocate(size == 0 ? 1 : size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return allocate(size == 0 ? 1 : size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return allocate(size == 0 ? 1 : size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return allocate(size == 0 ? 1 : size, static_cast<std::size_t>(alignment));
}

void operator delete(void* p) noexcept {
    release(p);
}
void operator delete[](void* p) noexcept {
    release(p);
}
void operator delete(void* p, std::size_t) noexcept {
    release(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    release(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
    release(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
    release(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    release(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    release(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
    release(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
    release(p);
}
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    release(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    release(p);
}
