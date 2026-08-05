#include <core/stl/memory.h>
#include <mimalloc.h>
#include <cstring>

namespace kimix::detail {

void *allocator_allocate(size_t size, size_t alignment) noexcept {
    if (alignment <= alignof(std::max_align_t)) {
        auto p = mi_malloc(size);
        return p;
    }
    return mi_malloc_aligned(size, alignment);
}

void allocator_deallocate(void *p, size_t) noexcept {
    mi_free(p);
}

void *allocator_reallocate(void *p, size_t size, size_t alignment) noexcept {
    if (alignment <= alignof(std::max_align_t)) {
        return mi_realloc(p, size);
    }
    return mi_realloc_aligned(p, size, alignment);
}

} // namespace kimix::detail
