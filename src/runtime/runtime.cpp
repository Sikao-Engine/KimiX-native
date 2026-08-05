#include <runtime/runtime.h>

namespace kimix {
namespace runtime {

const char* core_version() noexcept {
    return kimix::version_string;
}

} // namespace runtime
} // namespace kimix

// ---------------------------------------------------------------------------
// C-FFI — stable ABI entry points exported from the runtime shared library.
// ---------------------------------------------------------------------------

extern "C" {

const char* kimix_runtime_version(void) {
    return kimix::runtime::version_string;
}

const char* kimix_runtime_core_version(void) {
    return kimix::runtime::core_version();
}

} // extern "C"
