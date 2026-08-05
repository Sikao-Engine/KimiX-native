#include <core/string_scratch.h>
#include <core/stl/format.h>

#include <cstdio>
#include <cstring>
#include <cstdarg>

namespace kimix {

// ---------------------------------------------------------------------------
// StringScratch non-inline utility functions (free-standing helpers)
// ---------------------------------------------------------------------------

// Append a formatted string (printf-style) to a scratch buffer
void scratch_append_format(StringScratch &scratch, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    // Determine the required buffer size
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = std::vsnprintf(nullptr, 0, fmt, args_copy);
    va_end(args_copy);

    if (needed > 0) {
        scratch.reserve(scratch.size() + static_cast<size_t>(needed) + 1);
        // Build the formatted string into a temporary buffer
        kimix::string buf(static_cast<size_t>(needed), '\0');
        std::vsnprintf(buf.data(), static_cast<size_t>(needed) + 1, fmt, args);
        scratch << kimix::string_view{buf};
    }

    va_end(args);
}

// Append a hex representation of an integer
void scratch_append_hex(StringScratch &scratch, uint64_t value) {
    char buf[19]; // "0x" + 16 hex digits + null
    std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(value));
    scratch << buf;
}

// Append a pointer address
void scratch_append_ptr(StringScratch &scratch, const void *ptr) {
    scratch_append_hex(scratch, reinterpret_cast<uintptr_t>(ptr));
}

} // namespace kimix
