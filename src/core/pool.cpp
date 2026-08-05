#include <core/logging.h>
#include <core/pool.h>

namespace kimix {

void detail::memory_pool_check_memory_leak(size_t expected, size_t actual) noexcept {
    if (expected != actual) [[unlikely]] {
        KIMIX_WARNING_WITH_LOCATION(
            "Leaks detected in pool: expected {} objects but got {}.",
            expected, actual);
    }
}

} // namespace kimix
