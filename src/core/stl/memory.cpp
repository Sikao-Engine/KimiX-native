/*
 * memory.cpp — the out-of-line kimix::detail::allocator_* helpers declared in
 * stl/memory.h.
 *
 * One definition per declared name, all forwarding to the kimix::allocate
 * gateway (core/memory.h), so the STL wrappers, SharedFunction and every
 * container share the one tracked heap. The alignment-parameterised forms pick
 * the plain path when the platform already guarantees the alignment and the
 * aligned entry point only when it does not — the same split the previous
 * mi_malloc/mi_malloc_aligned pair made.
 *
 * Nothing here is inline: the point of the gateway is that core/memory.cpp is
 * the single heap entry point, so a header-inline copy would fork it again.
 * The declarations carry KIMIX_CORE_API (stl/memory.h).
 */
#include <core/stl/memory.h>

#include <cstddef>

namespace kimix::detail {

void *allocator_allocate(size_t size) noexcept {
    return kimix::allocate(size);
}

void *allocator_allocate(size_t size, size_t alignment) noexcept {
    if (alignment <= alignof(std::max_align_t)) {
        return kimix::allocate(size);
    }
    return kimix::allocate_aligned(size, alignment);
}

void allocator_deallocate(void *p) noexcept {
    kimix::deallocate(p);
}

void *allocator_reallocate(void *p, size_t new_size) noexcept {
    return kimix::reallocate(p, new_size);
}

void *allocator_reallocate(void *p, size_t new_size, size_t alignment) noexcept {
    if (alignment <= alignof(std::max_align_t)) {
        return kimix::reallocate(p, new_size);
    }
    return kimix::reallocate_aligned(p, new_size, alignment);
}

} // namespace kimix::detail
