#include <core/pool.h>

#include <cstdio>

namespace kimix {

void detail::memory_pool_check_memory_leak(size_t expected, size_t actual) noexcept {
    if (expected != actual) [[unlikely]] {
        std::fprintf(stderr, "[kimix][warning] Leaks detected in pool: expected %zu objects but got %zu (%s:%d)\n",
                     expected, actual, __FILE__, __LINE__);
    }
}

} // namespace kimix
